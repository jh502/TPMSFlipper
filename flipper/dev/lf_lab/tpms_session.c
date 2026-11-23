// SPDX-License-Identifier: GPL-2.0-or-later AND MIT
// From flipper-zero-tpms, Copyright (c) 2026 Vishov Vladimir, MIT licence
// (licenses/MIT-flipper-zero-tpms.txt), with this project's changes.
#include "tpms_session.h"
#include "tpms_preset.h"
#include "tpms_lf.h"

#include <furi.h>
#include <furi_hal.h>
#include <lib/subghz/devices/devices.h>
#include <lib/subghz/devices/cc1101_int/cc1101_int_interconnect.h>
#include <toolbox/level_duration.h>
#include <stm32wbxx_ll_tim.h>

#define TAG "TpmsSession"

/** Capacity of the interval buffer. FSK noise comes in a dense stream, so
 * the headroom has to be sizeable: at 20 kBaud this is about 0.1 s of
 * air time. */
#define TPMS_STREAM_CAPACITY 2048

/** How many intervals we take out of the buffer in one go. */
#define TPMS_PUMP_BATCH 128

typedef struct { uint32_t sequence, rx_lost; int32_t pulse; } CapturedInterval;

struct TpmsSession {
    const SubGhzDevice* device;
    FuriStreamBuffer* stream;
    TpmsDecoder* decoder;

    uint32_t frequency;
    TpmsModulation modulation;

    TpmsSessionFrameCallback frame_callback;
    void* frame_context;

    TpmsSessionArchiveCallback archive_callback;
    void* archive_context;
    TpmsSessionRawCallback raw_callback;
    void* raw_context;

    volatile uint32_t overruns;
    bool running, paused, lf, narrow, lf_only;
    const char* fault_reason;
    volatile bool capture_fault;
    volatile uint32_t sequence;
    uint32_t consumed_sequence, lf_high, fast_cycles;
    bool lf_have_high;

    /* Signal level at the moment the last batch of intervals arrived. */
    float last_rssi;

    /* The decoding buffer lives here rather than on the stack: pump() is
     * called from the CLI command thread, where the stack is modest. */
    CapturedInterval batch[TPMS_PUMP_BATCH];
};

static void tpms_session_capture_callback(bool level, uint32_t duration, void* context) {
    /* Interrupt context: just push into the buffer, nothing heavy. */
    TpmsSession* session = context;
    ++session->sequence;
    if(!duration || duration > INT32_MAX) { session->overruns++; return; }
    CapturedInterval item = {session->sequence, session->overruns,
                             level ? (int32_t)duration : -(int32_t)duration};
    if(furi_stream_buffer_spaces_available(session->stream) < sizeof(item) ||
       furi_stream_buffer_send(session->stream, &item, sizeof(item), 0) != sizeof(item))
        session->overruns++;

}

/* Read capture supplies high time and total period, not two durations.
 * This experimental path never starts the RFID carrier timer. */
static void tpms_session_lf_callback(bool high, uint32_t duration, void* context) {
    TpmsSession* session = context;
    if(LL_TIM_IsActiveFlag_CC3OVR(TIM2) || LL_TIM_IsActiveFlag_CC4OVR(TIM2)) {
        LL_TIM_ClearFlag_CC3OVR(TIM2);LL_TIM_ClearFlag_CC4OVR(TIM2);
        LL_TIM_DisableIT_CC3(TIM2);LL_TIM_DisableIT_CC4(TIM2);
        session->fault_reason="LF timer overcapture";
        session->capture_fault=true;return;
    }
    if(high) { session->lf_high=duration; session->lf_have_high=true; return; }
    if(duration < 32) session->fast_cycles++; else session->fast_cycles=0;
    if(session->fast_cycles >= 256) {
        /* An undemodulated carrier can overwhelm IRQ handling. Bound it. */
        LL_TIM_DisableIT_CC3(TIM2); LL_TIM_DisableIT_CC4(TIM2);
        session->fault_reason="LF carrier too dense";
        session->capture_fault=true;
        return;
    }
    if(session->lf_have_high && session->lf_high && duration>session->lf_high && duration<=INT32_MAX) {
        tpms_session_capture_callback(true,session->lf_high,session);
        tpms_session_capture_callback(false,duration-session->lf_high,session);
    }
    session->lf_have_high=false;
}

static void tpms_session_frame_callback(const TpmsFrame* frame, void* context) {
    TpmsSession* session = context;
    if(session->frame_callback)
        session->frame_callback(frame, session->last_rssi, session->frame_context);
}

/** Load the radio configuration the protocols of this modulation need. */
static void tpms_session_load_preset(TpmsSession* session) {
    if(session->modulation == TpmsModulationOok) {
        /* The stock OOK preset: 270 kHz of bandwidth around the carrier,
         * which suits chip rates from 50 to 170 us alike. */
        subghz_devices_load_preset(session->device, FuriHalSubGhzPresetOok270Async, NULL);
    } else {
        uint8_t preset[sizeof(tpms_fsk_preset)];
        tpms_session_get_preset(session,preset,sizeof(preset));
        subghz_devices_load_preset(session->device,FuriHalSubGhzPresetCustom,preset);
    }
    subghz_devices_set_frequency(session->device, session->frequency);
}

static TpmsSession* session_alloc(size_t capacity) {
    TpmsSession* session = malloc(sizeof(TpmsSession));
    memset(session, 0, sizeof(TpmsSession));

    session->stream = furi_stream_buffer_alloc(
        sizeof(CapturedInterval) * capacity, sizeof(CapturedInterval));
    session->decoder = tpms_decoder_alloc(tpms_session_frame_callback, session);
    return session;
}

TpmsSession* tpms_session_alloc(void) {return session_alloc(TPMS_STREAM_CAPACITY);}
/* Stock USB wake pumps continuously; 512 intervals retain ~25 ms at 20 kBaud.
 * Save 18 KiB for the LF envelope and CLI stack. Overruns remain reported. */
TpmsSession* tpms_session_alloc_bench(void) {return session_alloc(512);}

TpmsSession* tpms_session_alloc_lf(void) {
    TpmsSession* session=malloc(sizeof(TpmsSession));
    memset(session,0,sizeof(TpmsSession));
    session->stream=furi_stream_buffer_alloc(sizeof(CapturedInterval)*1024,sizeof(CapturedInterval));
    session->lf_only=session->lf=session->running=session->paused=true;
    return session;
}
const char* tpms_session_fault_reason(TpmsSession* session) {return session->fault_reason;}

void tpms_session_free(TpmsSession* session) {
    furi_check(session);
    if(session->running) tpms_session_stop(session);
    if(session->decoder) tpms_decoder_free(session->decoder);
    furi_stream_buffer_free(session->stream);
    free(session);
}

void tpms_session_set_frame_callback(
    TpmsSession* session,
    TpmsSessionFrameCallback callback,
    void* context) {
    furi_check(session);
    session->frame_callback = callback;
    session->frame_context = context;
}

void tpms_session_set_raw_callback(
    TpmsSession* session,
    TpmsSessionRawCallback callback,
    void* context) {
    furi_check(session);
    session->raw_callback = callback;
    session->raw_context = context;
}

void tpms_session_set_archive_callback(TpmsSession* session, TpmsSessionArchiveCallback callback, void* context) {
    session->archive_callback = callback;
    session->archive_context = context;
}

bool tpms_session_start(TpmsSession* session, uint32_t frequency, TpmsModulation modulation) {
    furi_check(session);
    furi_check(!session->running);

    subghz_devices_init();
    session->device = subghz_devices_get_by_name(SUBGHZ_DEVICE_CC1101_INT_NAME);
    if(!session->device) {
        FURI_LOG_E(TAG, "no cc1101_int device");
        subghz_devices_deinit();
        return false;
    }

    if(!subghz_devices_is_frequency_valid(session->device, frequency)) {
        FURI_LOG_E(TAG, "frequency %lu is not valid", frequency);
        subghz_devices_deinit();
        session->device = NULL;
        return false;
    }

    session->frequency = frequency;
    session->modulation = modulation;

    subghz_devices_begin(session->device);
    subghz_devices_reset(session->device);
    subghz_devices_idle(session->device);
    tpms_session_load_preset(session);

    session->overruns = session->sequence = session->consumed_sequence = 0;
    session->capture_fault = false;
    session->paused = session->lf = false;
    session->last_rssi = 0.0f;
    furi_stream_buffer_reset(session->stream);
    tpms_decoder_set_modulation(session->decoder, modulation);
    if(session->decoder) tpms_decoder_reset(session->decoder);

    subghz_devices_start_async_rx(session->device, tpms_session_capture_callback, session);
    session->running = true;
    return true;
}

void tpms_session_stop(TpmsSession* session) {
    furi_check(session);
    if(!session->running) return;

    if(session->lf) {
        if(!session->paused) furi_hal_rfid_tim_read_capture_stop();
        furi_hal_rfid_pins_reset();
        session->running=session->lf=session->paused=false;
        return;
    }
    if(!session->paused) subghz_devices_stop_async_rx(session->device);
    subghz_devices_idle(session->device);
    subghz_devices_sleep(session->device);
    subghz_devices_end(session->device);
    subghz_devices_deinit();

    session->device = NULL;
    session->running = false;
}

bool tpms_session_retune(TpmsSession* session, uint32_t frequency, TpmsModulation modulation) {
    furi_check(session);
    if(!session->running || session->lf || session->paused) return false;
    if(session->frequency == frequency && session->modulation == modulation) return true;
    if(!subghz_devices_is_frequency_valid(session->device, frequency)) return false;

    subghz_devices_stop_async_rx(session->device);
    subghz_devices_idle(session->device);

    session->frequency = frequency;
    session->modulation = modulation;
    tpms_session_load_preset(session);

    furi_stream_buffer_reset(session->stream);
    tpms_decoder_set_modulation(session->decoder, modulation);
    if(session->decoder) tpms_decoder_reset(session->decoder);

    subghz_devices_start_async_rx(session->device, tpms_session_capture_callback, session);
    return true;
}

void tpms_session_pause(TpmsSession* session) {
    if(!session->running || session->paused) return;
    if(session->lf) furi_hal_rfid_tim_read_capture_stop();
    else subghz_devices_stop_async_rx(session->device);
    session->paused=true;
}
void tpms_session_resume(TpmsSession* session) {
    if(!session->running || !session->paused || session->capture_fault) return;
    if(session->decoder) tpms_decoder_reset(session->decoder);
    if(session->lf) {
        furi_hal_rfid_pins_reset();
        furi_hal_gpio_write(&gpio_nfc_irq_rfid_pull,false);
        session->lf_have_high=false;
        /* CC1101 and RFID share TIM2. Do not inherit stale overcapture flags. */
        LL_TIM_ClearFlag_CC3OVR(TIM2);
        LL_TIM_ClearFlag_CC4OVR(TIM2);
        furi_hal_rfid_tim_read_capture_start(tpms_session_lf_callback,session);
    } else subghz_devices_start_async_rx(session->device,tpms_session_capture_callback,session);
    session->paused=false;
}
void tpms_session_reset_capture(TpmsSession* session) {
    furi_check(session->running);
    tpms_session_pause(session);
    furi_stream_buffer_reset(session->stream);
    session->overruns=session->sequence=session->consumed_sequence=0;
    session->capture_fault=false;session->fault_reason=NULL;session->fast_cycles=0;
    tpms_session_resume(session);
}
bool tpms_session_set_lf_capture(TpmsSession* session, bool enabled) {
    if(session->lf_only) {
        if(!enabled) tpms_session_pause(session);
        return true;
    }
    if(session->lf==enabled) return true;
    tpms_session_stop(session);
    if(!enabled) return tpms_session_start(session,433920000,TpmsModulationFsk);
    furi_hal_rfid_pins_reset();
    /* Release the antenna shunt while leaving carrier drive static/off. */
    furi_hal_gpio_write(&gpio_nfc_irq_rfid_pull,false);
    session->lf=session->running=session->paused=true;
    tpms_session_reset_capture(session);
    return true;
}
bool tpms_session_is_lf(TpmsSession* session) {return session->lf;}
bool tpms_session_capture_fault(TpmsSession* session) {return session->capture_fault;}
uint32_t tpms_session_sequence(TpmsSession* session) {return session->sequence;}
void tpms_session_set_vdo_only(TpmsSession* session,bool enabled) {
    if(session->decoder) tpms_decoder_set_vdo_only(session->decoder,enabled);
}

size_t tpms_session_pump(TpmsSession* session, uint32_t timeout_ms) {
    furi_check(session);

    const size_t received =
        furi_stream_buffer_receive(
            session->stream, session->batch, sizeof(session->batch), timeout_ms) /
        sizeof(CapturedInterval);

    /* Sample the level as soon as the data arrives: while we decode a
     * batch the transmission ends and the RSSI drops to the noise floor. */
    if(received > 0 && session->running && !session->lf && !session->paused) {
        session->last_rssi = subghz_devices_get_rssi(session->device);
    }

    for(size_t i = 0; i < received; i++) {
        const CapturedInterval* item=&session->batch[i];
        const bool level=item->pulse>0;
        const uint32_t duration=level?(uint32_t)item->pulse:(uint32_t)(-item->pulse);
        session->consumed_sequence=item->sequence;
        TpmsCaptureRecord record={furi_get_tick(),item->sequence,item->rx_lost,item->pulse};
        if(session->archive_callback) session->archive_callback(&record,session->archive_context);
        if(!session->lf) tpms_decoder_feed_sequenced(session->decoder,item->sequence,level,duration);
        if(session->raw_callback) session->raw_callback(level, duration, session->raw_context);
    }
    return received;
}

void tpms_session_wake_pulse(TpmsSession* session, uint32_t duration_ms) {
    furi_check(session);

    tpms_lf_field_start();
    const uint32_t deadline = furi_get_tick() + furi_ms_to_ticks(duration_ms);
    while(furi_get_tick() < deadline) {
        /* Keep receiving: the sensor answers right after activation. */
        tpms_session_pump(session, 20);
    }
    tpms_lf_field_stop();
}

float tpms_session_get_rssi(TpmsSession* session) {
    furi_check(session);
    if(!session->running || session->lf || session->paused) return 0.0f;
    return subghz_devices_get_rssi(session->device);
}

uint32_t tpms_session_get_overruns(TpmsSession* session) {
    furi_check(session);
    return session->overruns;
}

void tpms_session_set_narrow(TpmsSession* session,bool enabled) {
    if(session->narrow==enabled || session->lf) return;
    session->narrow=enabled;
    if(!session->running || session->paused) return;
    tpms_session_pause(session);
    subghz_devices_idle(session->device);
    tpms_session_load_preset(session);
    furi_stream_buffer_reset(session->stream);
    tpms_session_resume(session);
}

size_t tpms_session_get_preset(TpmsSession* session,uint8_t* data,size_t capacity) {
    if(session->lf || session->modulation!=TpmsModulationFsk || capacity<sizeof(tpms_fsk_preset)) return 0;
    memcpy(data,tpms_fsk_preset,sizeof(tpms_fsk_preset));
    if(session->narrow) {
        for(size_t i=0;i+1<sizeof(tpms_fsk_preset) && (data[i] || data[i+1]);i+=2)
            if(data[i]==CC1101_MDMCFG4) data[i+1]=0x79;
    }
    return sizeof(tpms_fsk_preset);
}
