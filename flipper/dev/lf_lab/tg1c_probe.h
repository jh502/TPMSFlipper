// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
/* Continental TG1C LF Lab — probe engine (plan v3 §3, §4b-§4e).
 *
 * Header-only and SDK-free. This is the whole probe session: preflight, the
 * burst train, the receive-only waits, the stop conditions and the session
 * log. It reaches the hardware only through the TG1cPort function table, so
 * tests/tg1c_probe.c runs this exact code on the host against a simulated
 * clock, receiver and LF engine. tg1c_lf_lab.c supplies the real port.
 *
 * Every transmit decision comes from tg1c_seq.h. This file adds the timing:
 *  - a burst is a train of frames for burst_ms with the receiver live, then a
 *    post-burst listen; the LF envelope is prepared once per burst and freed at
 *    burst end (a diluted burst, v0.3.1, rewrites that one envelope in the gap
 *    between frames when the next frame is the other of its two; a scan burst,
 *    v0.3.2, rewrites it before every frame with the group's next candidate);
 *  - on a CRC-valid bootloader reply or an unfamiliar status pair the carrier
 *    is stopped within one loop pass (about 1 ms) and no frame is started
 *    afterwards;
 *  - nothing is written to storage here; the log, the evidence ring and the
 *    per-frame telemetry log are emitted by tg1c_probe_write_csv / _raw / _tele
 *    after the receiver has stopped.
 */
#include "tg1c_frame.h"
#include "tg1c_btldr_rx.h"
#include "tg1c_ring.h"
#include "tg1c_carrier.h"
#include "tg1c_seq.h"
#include <stdlib.h>

#define TG1C_APP_VERSION "0.3.6"
/* One row per burst. The other selections send at most 3 wake bursts + 9
 * CONNECT bursts; a sweep's worst case is TG1C_SWEEP_MAX_BURSTS (123), a
 * dilution test's TG1C_DIL_MAX_BURSTS (78), an offset test's and a scan's
 * TG1C_OFF_MAX_BURSTS and TG1C_SCAN_MAX_BURSTS (83 each). */
#define TG1C_LOG_ROWS (TG1C_SWEEP_MAX_BURSTS + 1u)
_Static_assert(TG1C_LOG_ROWS > TG1C_DIL_MAX_BURSTS, "the row table holds the longest dilution session");
_Static_assert(TG1C_LOG_ROWS > TG1C_OFF_MAX_BURSTS, "the row table holds the longest offset session");
_Static_assert(TG1C_LOG_ROWS > TG1C_SCAN_MAX_BURSTS, "the row table holds the longest scan session");
_Static_assert(TG1C_LOG_ROWS < 0xFF, "a telemetry entry names its row in one byte; 0xFF means none");
#define TG1C_FRAME_TIMEOUT_MS 100 /* longest frame: 12 ms lead + 266 slots = 46 ms */
#define TG1C_UI_PERIOD_MS 250
#define TG1C_WORKER_STACK (6 * 1024)
/* Receiver diagnostics (v0.2.3): RSSI is read every TG1C_RSSI_PERIOD_MS while
 * a row is open; a reading at or above TG1C_RSSI_STRONG_DBM counts as strong.
 * The spare's telemetry measured -33 to -57 dBm on the bench. */
#define TG1C_RSSI_PERIOD_MS 5u
#define TG1C_RSSI_STRONG_DBM (-70)

/* Per-frame telemetry log (v0.2.2). Known-status frames are kept up to
 * TG1C_TELE_KNOWN_MAX; the remaining places are held for unknown-status
 * frames, so a long run of ordinary replies cannot crowd out the frames that
 * stopped the session.
 *
 * A sweep (v0.3.0) has up to 25 answered rows, so it keeps only the first
 * TG1C_TELE_SWEEP_HEAD known-status frames of a row and its latest one; every
 * unknown-status frame is still kept. 25 rows x 3 frames fit in the 80 places.
 * A dilution test (v0.3.1) is thinned the same way; it has at most 16 answered
 * rows (the sync and one per trial). An offset test and a scan (v0.3.2) have at
 * most 17 (the sync and one per offset or group). */
#define TG1C_TELE_LOG 96
#define TG1C_TELE_KNOWN_MAX 80
#define TG1C_TELE_NO_ROW 0xFF
#define TG1C_TELE_SWEEP_HEAD 2
#define TG1C_TELE_F_KNOWN 0x01 /* B4/B7 is 87/01 or 87/02 */
#define TG1C_TELE_F_RX_NXP 0x02 /* receiver on 434.00 MHz, else 433.92 MHz */
#define TG1C_TELE_PHASE_SHIFT 4
typedef enum {
    TG1C_PH_BURST = 0, /* frames being sent */
    TG1C_PH_LISTEN, /* the 2 s after a burst, same frequency */
    TG1C_PH_QUIET, /* quiet wait before the first CONNECT */
    TG1C_PH_WAIT, /* CONNECT spacing wait, or a sweep's wait for its next burst */
    TG1C_PH_OBSERVE, /* receive-only after a stop */
    TG1C_PH_COUNT
} TG1cPhase;
typedef struct {
    uint32_t tick;
    uint8_t raw[9]; /* ID, B4, pressure, temperature, B7, XOR */
    uint8_t row; /* index into rows[], or TG1C_TELE_NO_ROW */
    uint8_t flags; /* TG1C_TELE_F_* | phase << TG1C_TELE_PHASE_SHIFT */
    int8_t rssi_dbm;
} TG1cTeleEntry; /* 16 bytes */

typedef struct TG1cProbe TG1cProbe;

/* Everything the engine needs from the platform. Times are milliseconds. */
typedef struct {
    void* ctx;
    uint32_t (*tick_ms)(void* ctx);
    void (*delay_tick)(void* ctx); /* sleep about 1 ms */
    size_t (*free_heap)(void* ctx);
    bool (*cancelled)(void* ctx);
    /* Receiver. rx_pump hands decoded frames and raw intervals back through
     * tg1c_probe_on_frame / tg1c_probe_on_record, on the calling thread. */
    bool (*rx_start)(void* ctx, uint32_t hz);
    bool (*rx_retune)(void* ctx, uint32_t hz);
    void (*rx_pump)(void* ctx, uint32_t timeout_ms);
    uint32_t (*rx_overruns)(void* ctx);
    void (*rx_stop)(void* ctx);
    /* LF envelope engine (tg1c_lf_dma). */
    bool (*lf_prepare)(void* ctx, const uint8_t* slots, size_t nslots, uint16_t half_us, uint16_t lead_us);
    const char* (*lf_failure)(void* ctx);
    bool (*lf_start)(void* ctx);
    bool (*lf_done)(void* ctx);
    bool (*lf_error)(void* ctx);
    void (*lf_stop)(void* ctx);
    void (*lf_free)(void* ctx);
    /* Progress display. Any string may be NULL (leave that line as it is). */
    void (*show)(void* ctx, const TG1cProbe* probe, const char* title, const char* detail, const char* hint);
    /* Receiver signal strength now, in dBm. */
    int (*rx_rssi)(void* ctx);
    /* v0.3.1: replace the prepared envelope with another frame of the same
     * slot count and timing, in the same buffer; only between frames (carrier
     * off). False leaves the burst unusable: the engine stops it with a fault. */
    bool (*lf_reload)(void* ctx, const uint8_t* slots, size_t nslots, uint16_t half_us, uint16_t lead_us);
} TG1cPort;

typedef struct {
    uint32_t intervals; /* raw intervals delivered */
    uint32_t clean_max, clean_runs; /* tg1c_carrier.h: longest run, runs of 40 or more */
    int16_t rssi_peak; /* dBm */
    int32_t rssi_sum;
    uint32_t rssi_samples, rssi_strong;
} TG1cRxStats;
#define TG1C_RX_BURST 0
#define TG1C_RX_WAIT 1

/* One CSV row per burst (plan v3 §4e). A row also collects what is received in
 * the listen and wait after the burst, up to the start of the next one. */
typedef struct {
    uint32_t tick;
    uint8_t payload;
    uint16_t lead_us, gap_ms, burst_ms, frames;
    uint32_t rx_hz;
    TG1cTally tally;
    int32_t first_reply_ms; /* -1: nothing decoded */
    int16_t first_reply_frame; /* v0.3.2: frames completed in the burst at that moment; -1: nothing decoded */
    uint32_t sync_base, sync_like_base, overrun_base;
    uint32_t sync_hits, sync_like_hits, overruns;
    bool crc_valid;
    const char* fault;
    /* What the receiver saw, decoded or not (v0.2.3), in two parts (v0.2.4):
     * [TG1C_RX_BURST] the burst and its listen, on rx_hz; [TG1C_RX_WAIT] the
     * quiet wait, spacing wait or observe that follows, on 433.92 MHz. */
    TG1cRxStats rx[2];
    /* Sweep (v0.3.0); role NONE and bit -1 in every other selection. */
    uint8_t role; /* TG1cRole */
    bool variant; /* the frame sent was the one-bit variant, not the stock wake */
    int8_t bit; /* the wake-frame bit this row belongs to; -1: none */
    bool timed, have_spare_tick;
    uint8_t tele_kept; /* known-status entries of this row in the telemetry log */
    uint8_t tele_last; /* index of the entry that holds this row's latest frame */
    uint32_t planned_tick; /* planned burst start, when timed */
    uint32_t last_spare_tick; /* last known-status spare frame of the burst and its listen */
    /* Dilution test (v0.3.1); zero in every other selection. */
    uint8_t ratio, trial; /* the trial this row belongs to; 0: none (sync rows) */
    bool diluted; /* the burst mixed stock and filler frames */
    uint16_t fill_frames; /* of `frames`, how many were the filler */
    /* Offset test and scan (v0.3.2); zero in every other selection. An offset
     * test's rows also carry ratio 16 and trial = offset + 1. */
    uint8_t offset; /* a diluted burst's stock frames: frame index % ratio == offset */
    bool scan; /* the burst cycled through the candidates of a scan group */
    uint8_t scan_first, scan_n; /* the group this row belongs to; scan_n 0: none (sync rows) */
} TG1cLogRow;

struct TG1cProbe {
    const TG1cPort* port;
    /* Operator options. */
    uint8_t sel, rxf;
    uint16_t burst_ms, lead_us, gap_ms;
    uint16_t burst_option_ms; /* the Burst option; a sweep overrides it with 10 s */

    TG1cSeq seq;
    TG1cBtldrRx rx;
    TG1cRing ring; /* armed by the first sync or sync-like hit */
    TG1cRawRecord* ring_store;
    /* Armed only by a CRC-valid reply, so an earlier hit cannot use it up
     * (v0.2.4). Its storage is part of this struct, not a second allocation. */
    TG1cRing reply_ring;
    TG1cRawRecord reply_store[TG1C_RING_RECORDS];
    uint32_t ring_hit; /* sync hits, of both kinds, when `ring` was armed */
    uint32_t rx_hz;
    uint32_t last_sequence;
    bool have_sequence;
    bool rx_live;

    TG1cLogRow rows[TG1C_LOG_ROWS];
    uint8_t row_count;
    TG1cLogRow* row; /* burst in progress or last sent */
    uint32_t burst_start;
    uint16_t burst_frames; /* frames sent by the action in progress */

    uint8_t target; /* v0.3.3: TG1cTarget; "spare" below means this sensor */
    uint8_t tgt_b4, tgt_b7; /* pair of the latest known-status target frame of this action */
    uint8_t unk_b4, unk_b7; /* first pair of the session that was not normal for its sender */
    /* v0.3.5, park command. act_any: in the action in progress the target's
     * frames do not latch, whatever their pair. act_stop_tgt: its burst is cut
     * at the first frame from the target. tgt_any, any_*: such a frame arrived
     * in this action, and the pair of the latest one. */
    bool act_any, act_stop_tgt, tgt_any;
    uint8_t any_b4, any_b7;
    bool spare_seen; /* known-status frame from the spare during this action */
    uint32_t spare_tick; /* tick of the latest such frame */
    bool unknown_seen; /* latched for the session */
    bool have_tele;
    uint32_t last_tele_tick;
    uint32_t tele_id;
    uint8_t tele_b4, tele_b7;
    uint8_t tele_b5, tele_b6; /* v0.3.6: pressure and temperature bytes of the latest frame */
    uint32_t tele_total;
    TG1cTeleEntry tele[TG1C_TELE_LOG];
    uint8_t tele_count, tele_known;
    uint32_t tele_dropped;
    uint8_t phase; /* TG1cPhase of the action in progress */
    TG1cCarrier carrier;
    uint32_t rssi_tick;

    const char* fault;
    uint32_t session_tick;
    size_t heap_start, heap_ready, heap_need;
    uint8_t slots[TG1C_MAX_FRAME_SLOTS];
    uint8_t slots_fill[TG1C_SLOTS_A]; /* v0.3.1: the filler frame of a diluted burst; v0.3.2: also a scan burst's next frame */
    uint8_t scan_bytes[TG1C_SCAN_GROUP_MAX][TG1C_WAKE_BYTES]; /* v0.3.2: the candidates of the scan burst in progress */
    char text[512]; /* formatting buffer, kept off the stack; the longest CSV row is under 400 */
};

static const char* const kTg1cSelLong[TG1C_SEL_COUNT] = {
    "A stock wake",
    "B CONNECT@5A42",
    "C CONNECT@615E (hypothesis)",
    "D CONNECT@615E inverted body (hypothesis)",
    "Sequence A-B-C-D",
    "Bit sweep of wake frame A",
    "Dilution of wake frame A",
    "Offset test of wake frame A",
    "Scan of candidate frames",
    "Park command (frame E)",
};
static const char* const kTg1cPayloadTitle[TG1C_PAYLOAD_COUNT] = {
    "A wake",
    "B CONN 5A42",
    "C CONN 615E?",
    "D CONN 615E~?",
    "E park cmd",
};
static const char* const kTg1cRxfLong[TG1C_RXF_COUNT] = {"alternate", "434000000", "433920000"};

static inline void tg1c_probe_init(
    TG1cProbe* p,
    const TG1cPort* port,
    uint8_t sel,
    uint8_t burst_idx,
    uint8_t lead_idx,
    uint8_t gap_idx,
    uint8_t rxf) {
    memset(p, 0, sizeof(*p));
    p->port = port;
    p->sel = sel < TG1C_SEL_COUNT ? sel : TG1C_SEL_DEFAULT;
    p->rxf = rxf < TG1C_RXF_COUNT ? rxf : TG1C_RXF_DEFAULT;
    p->burst_ms = kTg1cBurstMs[burst_idx < TG1C_BURST_OPTIONS ? burst_idx : TG1C_BURST_DEFAULT];
    p->burst_option_ms = p->burst_ms;
    /* The grid's timing is built on 10 s bursts, whatever the option says. */
    if(tg1c_sel_is_grid(p->sel)) p->burst_ms = (uint16_t)TG1C_SWEEP_BURST_MS;
    /* So is the park command: 10 s is what the spare has been reliable with. */
    if(p->sel == TG1C_SEL_PARK) p->burst_ms = (uint16_t)TG1C_PARK_BURST_MS;
    p->lead_us = kTg1cLeadUs[lead_idx < TG1C_LEAD_OPTIONS ? lead_idx : TG1C_LEAD_DEFAULT];
    p->gap_ms = kTg1cGapMs[gap_idx < TG1C_GAP_OPTIONS ? gap_idx : TG1C_GAP_DEFAULT];
    p->rx_hz = TG1C_RX_HZ_WAKE;
    tg1c_btldr_rx_init(&p->rx);
    tg1c_carrier_init(&p->carrier);
    tg1c_ring_init(&p->ring, NULL, 0);
    tg1c_ring_init(&p->reply_ring, p->reply_store, TG1C_RING_RECORDS);
    tg1c_seq_init(&p->seq, p->sel, p->rxf);
}

/* Which sensor the session is aimed at (TG1cTarget): its telemetry is the
 * control and its normal status pairs are the known ones. Call after
 * tg1c_probe_init. */
static inline void tg1c_probe_set_target(TG1cProbe* p, uint8_t target) {
    tg1c_seq_set_target(&p->seq, target);
    p->target = p->seq.target;
}

/* Which 12 bits a sweep covers (0 = bits 0-11 .. 3 = bits 36-47). Call after
 * tg1c_probe_init; no effect on any other selection. */
static inline void tg1c_probe_set_sweep_range(TG1cProbe* p, uint8_t range) {
    tg1c_seq_set_sweep_range(&p->seq, range);
}

/* Which filler a dilution test uses (0 = bit 3, 1 = bit 40). Call after
 * tg1c_probe_init; no effect on any other selection. */
static inline void tg1c_probe_set_dilution_fill(TG1cProbe* p, uint8_t fill) {
    tg1c_seq_set_dilution_fill(&p->seq, fill);
}

/* What a scan covers: the set (TG1cScanSet), the group size (choice 0..3 = 1,
 * 2, 4, 8) and the first candidate (choice 0..15 = index 0, 16, .. 240). Call
 * after tg1c_probe_init; no effect on any other selection. */
static inline void tg1c_probe_set_scan(TG1cProbe* p, uint8_t set, uint8_t group_choice, uint8_t from_choice) {
    tg1c_seq_set_scan(&p->seq, set, group_choice, from_choice);
}

/* The letter a row's burst goes by in the logs: A-D for the stock payloads, V
 * for a one-bit variant of the wake, M for a diluted burst (stock and filler
 * mixed), S for a scan burst (candidate frames). */
static inline char tg1c_row_letter(const TG1cLogRow* r) {
    return r->scan ? 'S' : r->diluted ? 'M' : r->variant ? 'V' : (char)('A' + r->payload);
}

static inline void tg1c_probe_free(TG1cProbe* p) {
    free(p->ring_store);
    p->ring_store = NULL;
    tg1c_ring_init(&p->ring, NULL, 0);
}

/* ----- receiver callbacks --------------------------------------------------- */

static inline void tg1c_probe_note_first_reply(TG1cProbe* p) {
    if(p->row && p->row->first_reply_ms < 0) {
        p->row->first_reply_ms = (int32_t)(p->port->tick_ms(p->port->ctx) - p->burst_start);
        p->row->first_reply_frame = (int16_t)(p->row->frames > INT16_MAX ? INT16_MAX : p->row->frames);
    }
}

/* A checksum-valid TG1C telemetry frame: its nine bytes and the RSSI the
 * receiver reported for it. */
static inline void
    tg1c_probe_on_frame(TG1cProbe* p, uint32_t id, const uint8_t raw[9], int rssi_dbm) {
    uint8_t b4 = raw[4], b7 = raw[7];
    bool known = tg1c_frame_known(p->target, id, b4, b7);
    /* Sweep: after the first frames of a burst, each known-status frame replaces
     * the row's previous latest one in place. Frames heard in the wait that
     * follows (no LF on) are not thinned. */
    bool thin = tg1c_sel_is_grid(p->sel) && known && p->row && p->phase <= TG1C_PH_LISTEN;
    TG1cTeleEntry* reuse = NULL;
    if(thin && p->row->tele_kept > TG1C_TELE_SWEEP_HEAD) reuse = &p->tele[p->row->tele_last];
    if(reuse || (p->tele_count < TG1C_TELE_LOG && (!known || p->tele_known < TG1C_TELE_KNOWN_MAX))) {
        TG1cTeleEntry* e = reuse ? reuse : &p->tele[p->tele_count++];
        if(reuse) p->tele_dropped++; /* the frame it overwrites */
        e->tick = p->port->tick_ms(p->port->ctx);
        memcpy(e->raw, raw, sizeof(e->raw));
        e->row = p->row ? (uint8_t)(p->row - p->rows) : TG1C_TELE_NO_ROW;
        e->flags = (uint8_t)((known ? TG1C_TELE_F_KNOWN : 0) |
                             (p->rx_hz == TG1C_RX_HZ_NXP ? TG1C_TELE_F_RX_NXP : 0) |
                             (p->phase << TG1C_TELE_PHASE_SHIFT));
        e->rssi_dbm = (int8_t)(rssi_dbm < -128 ? -128 : rssi_dbm > 127 ? 127 : rssi_dbm);
        if(known && !reuse) {
            p->tele_known++;
            if(thin) {
                p->row->tele_kept++;
                p->row->tele_last = (uint8_t)(e - p->tele);
            }
        }
    } else {
        p->tele_dropped++;
    }
    p->tele_id = id;
    p->tele_b4 = b4;
    p->tele_b7 = b7;
    p->tele_b5 = raw[5];
    p->tele_b6 = raw[6];
    p->tele_total++;
    p->have_tele = true;
    p->last_tele_tick = p->port->tick_ms(p->port->ctx);
    tg1c_probe_note_first_reply(p);
    if(p->row) tg1c_tally_add_for(&p->row->tally, p->target, id, b4, b7);
    bool mine = id == tg1c_target_id(p->target);
    if(mine) {
        p->tgt_any = true;
        p->any_b4 = b4;
        p->any_b7 = b7;
    }
    if(!known && mine && p->act_any) {
        /* Park command: the target's answer to frame E, or to the wake after
         * it. Logged as not known; no latch. */
    } else if(!known) {
        if(!p->unknown_seen) {
            p->unk_b4 = b4;
            p->unk_b7 = b7;
        }
        p->unknown_seen = true; /* §2: any sensor, at any time */
    } else if(mine) {
        p->spare_seen = true;
        p->spare_tick = p->last_tele_tick;
        p->tgt_b4 = b4;
        p->tgt_b7 = b7;
        if(p->row && p->phase <= TG1C_PH_LISTEN) {
            p->row->last_spare_tick = p->last_tele_tick;
            p->row->have_spare_tick = true;
        }
    }
}

/* The part of the open row that the action in progress belongs to. */
static inline TG1cRxStats* tg1c_probe_stats(TG1cProbe* p) {
    if(!p->row) return NULL;
    return &p->row->rx[p->phase >= TG1C_PH_QUIET ? TG1C_RX_WAIT : TG1C_RX_BURST];
}

/* One raw interval record from the receiver. */
static inline void tg1c_probe_on_record(TG1cProbe* p, const TG1cRawRecord* record) {
    /* Lost intervals break a partial bootloader match and a carrier run. */
    if(p->have_sequence && record->sequence != p->last_sequence + 1) {
        tg1c_btldr_rx_break(&p->rx);
        tg1c_carrier_break(&p->carrier);
    }
    p->last_sequence = record->sequence;
    p->have_sequence = true;
    if(record->pulse == 0) return;

    tg1c_ring_push(&p->ring, record);
    tg1c_ring_push(&p->reply_ring, record);
    bool level = record->pulse > 0;
    uint32_t dur = level ? (uint32_t)record->pulse : (uint32_t)(-(int64_t)record->pulse);
    TG1cRxStats* st = tg1c_probe_stats(p);
    bool full_run = tg1c_carrier_feed(&p->carrier, dur);
    if(st) {
        st->intervals++;
        if(full_run) st->clean_runs++;
        if(p->carrier.cur > st->clean_max) st->clean_max = p->carrier.cur;
    }
    unsigned ev = tg1c_btldr_rx_feed(&p->rx, level, dur);
    if(!ev) return;
    if(ev & (TG1C_BL_EV_SYNC | TG1C_BL_EV_SYNC_LIKE)) {
        if(tg1c_ring_trigger(&p->ring, p->rx.last_variant, !(ev & TG1C_BL_EV_SYNC))) {
            p->ring.rx_hz = p->rx_hz;
            p->ring_hit = p->rx.sync_hits + p->rx.sync_like_hits;
        }
    }
    if(ev & TG1C_BL_EV_REPLY) {
        /* The first-hit ring holds this reply only if its own sync armed it. */
        if(p->ring_hit == p->rx.sync_hits + p->rx.sync_like_hits) tg1c_ring_mark_crc_valid(&p->ring);
        bool reversed = p->rx.reply_variant == TG1C_BL_VAR_REVERSED ||
                        p->rx.reply_variant == TG1C_BL_VAR_REVERSED_INVERTED;
        if(tg1c_ring_trigger_post(
               &p->reply_ring, p->rx.reply_variant, reversed, (uint16_t)TG1C_RING_REPLY_POST)) {
            p->reply_ring.rx_hz = p->rx_hz;
            tg1c_ring_mark_crc_valid(&p->reply_ring);
        }
        if(p->row) p->row->crc_valid = true;
        tg1c_probe_note_first_reply(p);
    }
}

/* ----- engine --------------------------------------------------------------- */

static inline uint32_t tg1c_probe_now(TG1cProbe* p) {
    return p->port->tick_ms(p->port->ctx);
}
static inline bool tg1c_probe_cancelled(TG1cProbe* p) {
    return p->port->cancelled(p->port->ctx);
}
/* A latch event that must stop transmission and end the current wait. Once the
 * sequencer is locked (observe phase) nothing further interrupts. */
static inline bool tg1c_probe_stop_event(TG1cProbe* p) {
    return (!p->seq.tx_locked && (p->rx.reply_valid || p->unknown_seen)) || (p->act_stop_tgt && p->tgt_any);
}
static inline void tg1c_probe_show(TG1cProbe* p, const char* title, const char* detail, const char* hint) {
    p->port->show(p->port->ctx, p, title, detail, hint);
}
static inline void tg1c_probe_fmt_hz(char* out, size_t cap, uint32_t hz) {
    snprintf(out, cap, "%lu.%02lu", (unsigned long)(hz / 1000000u), (unsigned long)(hz / 10000u % 100u));
}

/* Read the RSSI into the open row, at most every TG1C_RSSI_PERIOD_MS. */
static inline void tg1c_probe_sample_rssi(TG1cProbe* p) {
    TG1cRxStats* r = tg1c_probe_stats(p);
    if(!r || !p->rx_live) return;
    uint32_t now = tg1c_probe_now(p);
    if(r->rssi_samples && now - p->rssi_tick < TG1C_RSSI_PERIOD_MS) return;
    p->rssi_tick = now;
    int dbm = p->port->rx_rssi(p->port->ctx);
    if(dbm < -128) dbm = -128;
    if(dbm > 0) dbm = 0;
    if(!r->rssi_samples || dbm > r->rssi_peak) r->rssi_peak = (int16_t)dbm;
    r->rssi_sum += dbm;
    r->rssi_samples++;
    if(dbm >= TG1C_RSSI_STRONG_DBM) r->rssi_strong++;
}

static inline void tg1c_probe_row_close(TG1cProbe* p) {
    TG1cLogRow* r = p->row;
    if(!r) return;
    r->sync_hits = p->rx.sync_hits - r->sync_base;
    r->sync_like_hits = p->rx.sync_like_hits - r->sync_like_base;
    r->overruns = p->port->rx_overruns(p->port->ctx) - r->overrun_base;
}

/* Receive-only for wait_ms. quiet_ms > 0 makes it the quiet wait: return once
 * quiet_ms have passed since the last telemetry frame, wait_ms being the cap.
 * Ends early on cancel, and on a latch event unless already locked. */
static inline void tg1c_probe_listen(
    TG1cProbe* p,
    const char* title,
    const char* what,
    uint32_t wait_ms,
    uint32_t quiet_ms) {
    uint32_t t0 = tg1c_probe_now(p), shown = 0;
    bool first = true;
    for(;;) {
        uint32_t now = tg1c_probe_now(p), elapsed = now - t0;
        if(tg1c_probe_cancelled(p) || tg1c_probe_stop_event(p) || elapsed >= wait_ms) break;
        if(quiet_ms) {
            uint32_t since = p->have_tele ? now - p->last_tele_tick : UINT32_MAX;
            if(since >= quiet_ms) break;
        }
        if(first || now - shown >= TG1C_UI_PERIOD_MS) {
            char hz[12], detail[32];
            /* What is left: of the cap, or of the quiet period if that ends sooner. */
            uint32_t left = wait_ms - elapsed;
            if(quiet_ms && p->have_tele && quiet_ms - (now - p->last_tele_tick) < left)
                left = quiet_ms - (now - p->last_tele_tick);
            tg1c_probe_fmt_hz(hz, sizeof(hz), p->rx_hz);
            snprintf(
                detail,
                sizeof(detail),
                "%s %lus RX %s",
                what,
                (unsigned long)((left + 999) / 1000),
                hz);
            tg1c_probe_show(p, title, detail, NULL);
            shown = now;
            first = false;
        }
        /* As the bridge's receive loop: block briefly for data, then yield. */
        p->port->rx_pump(p->port->ctx, 10);
        tg1c_probe_sample_rssi(p);
        p->port->delay_tick(p->port->ctx);
    }
}

/* Screen title for a grid action: "Sync wake 2/3", "Bit 17 variant", "1in8 t2
 * diluted", "Off 9 diluted", "B38-3B scan". Leaves title alone for the other
 * selections. */
static inline void tg1c_probe_sweep_title(const TG1cAction* a, char* title, size_t cap) {
    if(a->role == TG1C_ROLE_SYNC)
        snprintf(title, cap, "Sync wake %u/%u", a->burst_no, a->burst_max);
    else if(a->scan_n && a->role == TG1C_ROLE_RESYNC)
        snprintf(
            title,
            cap,
            "%c%02X-%02X resync %u/%u",
            a->scan_set == TG1C_SCAN_NIBBLE ? 'N' : 'B',
            a->scan_first,
            a->scan_first + a->scan_n - 1u,
            a->burst_no,
            a->burst_max);
    else if(a->scan_n)
        snprintf(
            title,
            cap,
            "%c%02X-%02X %s",
            a->scan_set == TG1C_SCAN_NIBBLE ? 'N' : 'B',
            a->scan_first,
            a->scan_first + a->scan_n - 1u,
            tg1c_role_name(a->role, TG1C_SEL_SCAN));
    else if(a->off_test && a->role == TG1C_ROLE_RESYNC)
        snprintf(title, cap, "Off %u resync %u/%u", a->offset, a->burst_no, a->burst_max);
    else if(a->off_test)
        snprintf(title, cap, "Off %u %s", a->offset, tg1c_role_name(a->role, TG1C_SEL_OFFSET));
    else if(a->ratio && a->role == TG1C_ROLE_RESYNC)
        snprintf(title, cap, "1in%u t%u resync %u/%u", a->ratio, a->trial, a->burst_no, a->burst_max);
    else if(a->ratio)
        snprintf(
            title, cap, "1in%u t%u %s", a->ratio, a->trial, tg1c_role_name(a->role, TG1C_SEL_DILUTION));
    else if(a->role == TG1C_ROLE_RESYNC)
        snprintf(title, cap, "Bit %d resync %u/%u", a->sweep_bit, a->burst_no, a->burst_max);
    else if(a->role != TG1C_ROLE_NONE)
        snprintf(title, cap, "Bit %d %s", a->sweep_bit, tg1c_role_str(a->role));
}

/* One burst: a train of frames for burst_ms with the receiver live, then the
 * post-burst listen. */
static inline void tg1c_probe_burst(TG1cProbe* p, const TG1cAction* a) {
    const TG1cPort* port = p->port;
    char title[28], detail[32], hz[12], mix[32];
    tg1c_probe_sweep_title(a, title, sizeof(title));
    if(a->role == TG1C_ROLE_NONE)
        snprintf(
            title, sizeof(title), "%s %u/%u", kTg1cPayloadTitle[a->payload], a->burst_no, a->burst_max);
    tg1c_probe_fmt_hz(hz, sizeof(hz), p->rx_hz);
    if(a->scan)
        snprintf(mix, sizeof(mix), "%u %s candidates", a->scan_n, tg1c_scan_set_str(a->scan_set));
    else if(a->off_test)
        snprintf(mix, sizeof(mix), "1 in %u at +%u, fill bit %d", a->ratio, a->offset, a->fill_bit);
    else
        snprintf(mix, sizeof(mix), "wake 1 in %u, fill bit %d", a->ratio, a->fill_bit);
    const char* hint =
        (a->diluted || a->scan) ? mix :
        a->bit >= 0 ? "one-bit variant of wake" :
        (a->payload == TG1C_PAYLOAD_C || a->payload == TG1C_PAYLOAD_D) ? "hypothesis frame" :
        a->payload == TG1C_PAYLOAD_E ? "disable-shipping frame" : "";

    tg1c_probe_row_close(p);
    if(p->row_count >= TG1C_LOG_ROWS) {
        p->fault = "log full";
        return;
    }
    TG1cLogRow* r = &p->rows[p->row_count++];
    memset(r, 0, sizeof(*r));
    r->tick = tg1c_probe_now(p);
    r->payload = (uint8_t)a->payload;
    r->lead_us = p->lead_us;
    r->gap_ms = p->gap_ms;
    r->burst_ms = p->burst_ms;
    r->rx_hz = p->rx_hz;
    r->first_reply_ms = -1;
    r->first_reply_frame = -1;
    r->role = a->role;
    r->variant = a->bit >= 0;
    r->bit = a->sweep_bit;
    r->timed = a->timed;
    r->planned_tick = a->planned_tick;
    r->ratio = a->ratio;
    r->trial = a->trial;
    r->diluted = a->diluted;
    r->offset = a->offset;
    r->scan = a->scan;
    r->scan_first = a->scan_first;
    r->scan_n = a->scan_n;
    r->sync_base = p->rx.sync_hits;
    r->sync_like_base = p->rx.sync_like_hits;
    r->overrun_base = port->rx_overruns(port->ctx);
    p->row = r;
    p->burst_start = r->tick;
    p->phase = TG1C_PH_BURST;

    size_t nslots = a->bit >= 0 ? tg1c_build_wake_variant(a->bit, p->slots, sizeof(p->slots)) :
                                  tg1c_build_frame(a->payload, p->slots, sizeof(p->slots));
    /* Diluted burst: the filler is built once; it must be the stock frame's length. */
    if(nslots && a->diluted &&
       tg1c_build_wake_variant(a->fill_bit, p->slots_fill, sizeof(p->slots_fill)) != nslots)
        nslots = 0;
    /* Scan burst: the group's candidates are worked out once; frame 0 is the
     * first of them. Later frames are built into slots_fill, one at a time. */
    uint8_t scan_n = a->scan ? a->scan_n : 0;
    if(scan_n > TG1C_SCAN_GROUP_MAX) scan_n = 0, nslots = 0;
    for(uint8_t j = 0; j < scan_n; j++)
        if(!tg1c_scan_bytes(a->scan_set, (uint16_t)(a->scan_first + j), p->scan_bytes[j])) nslots = 0;
    if(a->scan && (!scan_n || !nslots)) nslots = 0;
    if(nslots && a->scan) nslots = tg1c_build_wake_bytes(p->scan_bytes[0], p->slots, sizeof(p->slots));
    if(!nslots) {
        p->fault = r->fault = "frame build failed";
        return;
    }
    /* The envelope holds the filler, not the stock frame. A diluted burst
     * whose stock frame is not at offset 0 starts on the filler. */
    bool fill_loaded = a->diluted && !tg1c_dilution_is_stock(0, a->ratio, a->offset);
    if(!port->lf_prepare(
           port->ctx, fill_loaded ? p->slots_fill : p->slots, nslots, TG1C_HALFBIT_US, p->lead_us)) {
        p->fault = r->fault = port->lf_failure(port->ctx);
        port->lf_free(port->ctx);
        return;
    }

    uint32_t shown = 0;
    bool first = true;
    while(!tg1c_probe_cancelled(p) && !tg1c_probe_stop_event(p) &&
          tg1c_probe_now(p) - p->burst_start < p->burst_ms) {
        if(!port->lf_start(port->ctx)) {
            p->fault = r->fault = "LF engine start failed";
            break;
        }
        uint32_t frame_start = tg1c_probe_now(p);
        while(!port->lf_done(port->ctx) && !port->lf_error(port->ctx) && !tg1c_probe_cancelled(p) &&
              !tg1c_probe_stop_event(p) &&
              tg1c_probe_now(p) - frame_start < TG1C_FRAME_TIMEOUT_MS) {
            port->rx_pump(port->ctx, 0);
            tg1c_probe_sample_rssi(p);
            port->delay_tick(port->ctx);
        }
        bool complete = port->lf_done(port->ctx) && !port->lf_error(port->ctx);
        port->lf_stop(port->ctx); /* carrier off now, whatever ended the wait */
        if(!complete) {
            if(!tg1c_probe_cancelled(p) && !tg1c_probe_stop_event(p))
                p->fault = r->fault = "LF engine interrupted";
            break;
        }
        r->frames++;
        if(fill_loaded) r->fill_frames++;
        p->burst_frames = r->frames;
        uint32_t gap = tg1c_probe_now(p);
        /* Diluted burst: frame index % ratio == offset is stock, every other
         * frame the filler. The swap is done here, with the carrier off, so the
         * gap that follows absorbs the time it takes. Not after a stop: once the
         * session is ending nothing of the LF engine is touched but stop/free. */
        if(a->diluted && !tg1c_probe_cancelled(p) && !tg1c_probe_stop_event(p) &&
           tg1c_dilution_is_stock(r->frames, a->ratio, a->offset) == fill_loaded) {
            fill_loaded = !fill_loaded;
            if(!port->lf_reload(
                   port->ctx, fill_loaded ? p->slots_fill : p->slots, nslots, TG1C_HALFBIT_US, p->lead_us)) {
                p->fault = r->fault = port->lf_failure(port->ctx);
                break;
            }
        }
        /* Scan burst: frame i carries candidate i % n of the group. With more
         * than one candidate every frame differs from the one before, so the
         * next one is built and loaded here, in the same carrier-off gap. */
        if(scan_n > 1 && !tg1c_probe_cancelled(p) && !tg1c_probe_stop_event(p)) {
            if(tg1c_build_wake_bytes(p->scan_bytes[r->frames % scan_n], p->slots_fill, sizeof(p->slots_fill)) != nslots) {
                p->fault = r->fault = "frame build failed";
                break;
            }
            if(!port->lf_reload(port->ctx, p->slots_fill, nslots, TG1C_HALFBIT_US, p->lead_us)) {
                p->fault = r->fault = port->lf_failure(port->ctx);
                break;
            }
        }
        do {
            port->rx_pump(port->ctx, 0);
            tg1c_probe_sample_rssi(p);
            port->delay_tick(port->ctx);
        } while(!tg1c_probe_cancelled(p) && !tg1c_probe_stop_event(p) &&
                tg1c_probe_now(p) - gap < p->gap_ms);

        uint32_t now = tg1c_probe_now(p);
        if(first || now - shown >= TG1C_UI_PERIOD_MS) {
            snprintf(detail, sizeof(detail), "TX %u fr  RX %s", r->frames, hz);
            tg1c_probe_show(p, title, detail, hint);
            shown = now;
            first = false;
        }
    }
    port->lf_free(port->ctx); /* §3: freed at burst end */

    if(!p->fault && !tg1c_probe_cancelled(p) && !tg1c_probe_stop_event(p)) {
        p->phase = TG1C_PH_LISTEN;
        tg1c_probe_listen(p, title, "listen", a->wait_ms, 0);
    }
}

/* Run one session from receiver start to receiver stop. No storage access. */
static inline void tg1c_probe_run(TG1cProbe* p) {
    const TG1cPort* port = p->port;
    p->session_tick = tg1c_probe_now(p);
    p->heap_start = port->free_heap(port->ctx);
    tg1c_probe_show(p, "Starting", kTg1cSelLong[p->sel], "");

    p->rx_live = port->rx_start(port->ctx, p->rx_hz);
    if(!p->rx_live) {
        p->fault = "receiver start failed";
    } else {
        /* §3 preflight: envelope + ring + 16 KiB reserve, or refuse. */
        size_t max_slots = tg1c_selection_max_slots(p->sel);
        p->heap_need = tg1c_preflight_need(max_slots, TG1C_HALFBIT_US, p->lead_us);
        p->heap_ready = port->free_heap(port->ctx);
        if(!tg1c_preflight_ok(p->heap_ready, max_slots, TG1C_HALFBIT_US, p->lead_us)) {
            p->fault = "low memory - not started";
        } else {
            p->ring_store = malloc(TG1C_RING_RECORDS * sizeof(TG1cRawRecord));
            if(p->ring_store)
                tg1c_ring_init(&p->ring, p->ring_store, TG1C_RING_RECORDS);
            else
                p->fault = "ring allocation failed";
        }
    }

    TG1cOutcome outcome = {0};
    if(p->fault) {
        outcome.fault = true;
        outcome.nothing_sent = true; /* refused before any transmission */
        tg1c_seq_next(&p->seq);
        tg1c_seq_report(&p->seq, &outcome);
    }

    while(!p->fault) {
        TG1cAction a = tg1c_seq_next(&p->seq);
        if(a.kind == TG1C_ACT_DONE) break;

        /* Retune only between actions, never during a burst (§4d). */
        if(a.rx_hz != p->rx_hz) {
            if(port->rx_retune(port->ctx, a.rx_hz)) {
                p->rx_hz = a.rx_hz;
                tg1c_btldr_rx_break(&p->rx);
                tg1c_carrier_break(&p->carrier);
                p->have_sequence = false;
            } else {
                p->fault = "retune failed";
            }
        }

        p->spare_seen = false;
        p->spare_tick = 0;
        p->burst_frames = 0;
        p->tgt_any = false;
        p->act_any = a.any_status;
        p->act_stop_tgt = a.kind == TG1C_ACT_BURST && a.stop_on_target;
        if(!p->fault) {
            switch(a.kind) {
            case TG1C_ACT_GRID_WAIT: {
                /* Receive-only until the planned start of the next sweep burst.
                 * Signed difference: a start already past means no wait, and a
                 * tick wrap in between does not matter. */
                int32_t left = (int32_t)(a.planned_tick - tg1c_probe_now(p));
                char title[28];
                tg1c_probe_sweep_title(&a, title, sizeof(title));
                p->phase = TG1C_PH_WAIT;
                if(left > 0) tg1c_probe_listen(p, title, "wait", (uint32_t)left, 0);
                break;
            }
            case TG1C_ACT_BURST:
                tg1c_probe_burst(p, &a);
                break;
            case TG1C_ACT_QUIET_WAIT:
                p->phase = TG1C_PH_QUIET;
                tg1c_probe_listen(p, "Quiet wait", "quiet", a.cap_ms, a.wait_ms);
                break;
            case TG1C_ACT_SPACING:
                p->phase = TG1C_PH_WAIT;
                tg1c_probe_listen(p, "CONNECT spacing", "RX only", a.wait_ms, 0);
                break;
            case TG1C_ACT_OBSERVE:
                p->phase = TG1C_PH_OBSERVE;
                /* Back ends the observe and keeps the latched result. */
                tg1c_probe_show(p, "RX only, TX off", "observe", "Back = finish now");
                tg1c_probe_listen(p, "RX only, TX off", "observe", a.wait_ms, a.quiet_ms);
                break;
            default:
                break;
            }
        }
        memset(&outcome, 0, sizeof(outcome));
        outcome.spare_telemetry = p->spare_seen;
        outcome.last_spare_tick = p->spare_tick;
        outcome.unknown_status = p->unknown_seen;
        outcome.tgt_b4 = p->tgt_b4;
        outcome.tgt_b7 = p->tgt_b7;
        outcome.unk_b4 = p->unk_b4;
        outcome.unk_b7 = p->unk_b7;
        outcome.tgt_any = p->tgt_any;
        outcome.any_b4 = p->any_b4;
        outcome.any_b7 = p->any_b7;
        p->act_any = p->act_stop_tgt = false; /* nothing is excused or cut outside its action */
        outcome.reply_valid = p->rx.reply_valid;
        outcome.fault = p->fault != NULL;
        outcome.cancelled = tg1c_probe_cancelled(p);
        outcome.nothing_sent = a.kind == TG1C_ACT_BURST && p->burst_frames == 0;
        tg1c_seq_report(&p->seq, &outcome);
    }

    /* Carrier off and receiver stopped before the caller touches storage. */
    port->lf_stop(port->ctx);
    port->lf_free(port->ctx);
    if(p->rx_live) {
        tg1c_probe_row_close(p);
        port->rx_stop(port->ctx);
        p->rx_live = false;
    }
    p->row = NULL;
}

/* ----- output (call only after tg1c_probe_run has returned) ----------------- */

typedef bool (*TG1cSink)(void* ctx, const void* data, size_t len);

static inline bool tg1c_probe_put(TG1cProbe* p, TG1cSink sink, void* ctx, int n) {
    if(n < 0) return false;
    if(n >= (int)sizeof(p->text)) n = (int)sizeof(p->text) - 1;
    return sink(ctx, p->text, (size_t)n);
}

/* The session log (plan v3 §4e): '#' header lines, then one row per burst. */
static inline bool
    tg1c_probe_write_csv(TG1cProbe* p, uint32_t stack_free_min, TG1cSink sink, void* ctx) {
    char* t = p->text;
    size_t cap = sizeof(p->text);
    bool ok = tg1c_probe_put(
        p, sink, ctx, snprintf(t, cap, "#app,continental_tg1c_lf,%s\n", TG1C_APP_VERSION));
    if(ok)
        ok = tg1c_probe_put(
            p,
            sink,
            ctx,
            snprintf(
                t,
                cap,
                "#selection,%s\n#options,burst_ms=%u,lead_us=%u,gap_ms=%u,connect_rx=%s\n",
                kTg1cSelLong[p->sel],
                p->burst_ms,
                p->lead_us,
                p->gap_ms,
                kTg1cRxfLong[p->rxf]));
    if(ok)
        ok = tg1c_probe_put(
            p,
            sink,
            ctx,
            snprintf(
                t,
                cap,
                "#frame,halfbit_us=%u,eom_slots=%u,connect_spacing_ms=%lu\n",
                (unsigned)TG1C_HALFBIT_US,
                (unsigned)TG1C_EOM_SLOTS,
                (unsigned long)TG1C_CONNECT_SPACING_MS));
    if(ok)
        ok = tg1c_probe_put(
            p,
            sink,
            ctx,
            snprintf(
                t,
                cap,
                "#free_heap_start,%lu\n#free_heap_ready,%lu\n#preflight_need,%lu\n"
                "#worker_stack,%u\n#stack_free_min,%lu\n",
                (unsigned long)p->heap_start,
                (unsigned long)p->heap_ready,
                (unsigned long)p->heap_need,
                (unsigned)TG1C_WORKER_STACK,
                (unsigned long)stack_free_min));
    bool sweep = p->sel == TG1C_SEL_SWEEP, dil = p->sel == TG1C_SEL_DILUTION;
    bool off = p->sel == TG1C_SEL_OFFSET, scan = p->sel == TG1C_SEL_SCAN, grid = sweep || dil || off || scan;
    /* v0.3.3: which sensor the session was aimed at, and the pairs counted as
     * normal for it. */
    if(ok)
        ok = tg1c_probe_put(
            p,
            sink,
            ctx,
            snprintf(
                t,
                cap,
                "#target,%s,id=%08lX,known=%s\n#observe,status_quiet_ms=%lu,cap_ms=%lu\n",
                tg1c_target_str(p->target),
                (unsigned long)tg1c_target_id(p->target),
                p->target == TG1C_TARGET_BAD ? "Fx/01;Fx/02" : "87/01;87/02",
                (unsigned long)TG1C_OBSERVE_QUIET_MS,
                (unsigned long)TG1C_OBSERVE_MS));
    /* v0.3.6: the latest frame as the screen read it. Battery and class are
     * readings of B4 and are candidates (tg1c_seq.h). */
    if(ok && p->have_tele)
        ok = tg1c_probe_put(
            p,
            sink,
            ctx,
            snprintf(
                p->text,
                sizeof(p->text),
                "#last_frame,id=%08lX,b4=%02X,b7=%02X,kpa=%u,temp_c=%d,battery=%s,class=%s\n"
                "#note_last_frame,kpa is B5 x 1.38 and temp_c is B6 - 50; battery (ok / ? / lo?) and class "
                "(norm? / ship? / other) are candidate readings of B4 and prove nothing about the mode\n",
                (unsigned long)p->tele_id,
                p->tele_b4,
                p->tele_b7,
                tg1c_kpa(p->tele_b5),
                tg1c_temp_c(p->tele_b6),
                tg1c_batt_str(tg1c_b4_batt(p->tele_b4)),
                tg1c_class_str(tg1c_b4_class(p->tele_b4))));
    if(ok && p->sel == TG1C_SEL_PARK) {
        uint8_t pb[TG1C_WAKE_BYTES];
        char before[8] = "-", after[8] = "-", fresh[8] = "-", ack[8] = "-";
        tg1c_park_bytes(pb);
        if(p->seq.pk_have_before) snprintf(before, sizeof(before), "%02X/%02X", p->seq.pk_before_b4, p->seq.pk_before_b7);
        if(p->seq.pk_have_after) snprintf(after, sizeof(after), "%02X/%02X", p->seq.pk_after_b4, p->seq.pk_after_b7);
        if(p->seq.pk_have_new) snprintf(fresh, sizeof(fresh), "%02X/%02X", p->seq.pk_new_b4, p->seq.pk_new_b7);
        if(p->seq.pk_have_ack) snprintf(ack, sizeof(ack), "%02X/%02X", p->seq.pk_ack_b4, p->seq.pk_ack_b7);
        ok = tg1c_probe_put(
            p,
            sink,
            ctx,
            snprintf(
                t,
                cap,
                "#park,frame=%02X%02X%02X%02X%02X%02X,burst_ms=%u,burst_forced=1,burst_option_ms=%u,"
                "wake_bursts_max=%u,cmd_bursts_max=%u,cmd_sent=%u,before=%s,ack=%s,after=%s,changed=%s,new=%s\n",
                pb[0],
                pb[1],
                pb[2],
                pb[3],
                pb[4],
                pb[5],
                p->burst_ms,
                p->burst_option_ms,
                (unsigned)TG1C_PARK_WAKE_BURSTS,
                (unsigned)TG1C_PARK_CMD_BURSTS,
                p->seq.pk_cmd_sent,
                before,
                ack,
                after,
                !p->seq.pk_have_after ? "-" : tg1c_seq_park_changed(&p->seq) ? "yes" : "no",
                fresh));
        if(ok)
            ok = tg1c_probe_put(
                p,
                sink,
                ctx,
                snprintf(
                    t,
                    cap,
                    "#note_park,E is the frame a workshop tool sends for DISABLE SHIPPING MODE, read from "
                    "its firmware and not captured on air; before, ack, after: the target's pair in the "
                    "wake before it, in its first frame in answer to it, and in the wake after it; "
                    "changed: the high nibble of B4 differs between before and after; new: a pair that "
                    "latched; a changed pair is a candidate, not proof of Park mode\n"));
    }
    if(ok && off) {
        uint8_t fb[TG1C_WAKE_BYTES] = {0};
        tg1c_wake_bytes(p->seq.dl_fill, fb);
        ok = tg1c_probe_put(
            p,
            sink,
            ctx,
            snprintf(
                t,
                cap,
                "#offset_test,fill_bit=%u,fill_frame=%02X%02X%02X%02X%02X%02X,ratio=%u,offsets=0-%u,"
                "period_ms=%lu,burst_ms=%lu,burst_forced=1,burst_option_ms=%u,diluted_n=1,control_n=2,"
                "control_lead_ms=%lu,stock_frame=index%%ratio==offset\n",
                p->seq.dl_fill,
                fb[0],
                fb[1],
                fb[2],
                fb[3],
                fb[4],
                fb[5],
                (unsigned)TG1C_OFF_RATIO,
                (unsigned)TG1C_OFF_ITEMS - 1u,
                (unsigned long)TG1C_LISTEN_PERIOD_MS,
                (unsigned long)TG1C_SWEEP_BURST_MS,
                p->burst_option_ms,
                (unsigned long)TG1C_CONTROL_LEAD_MS));
    }
    if(ok && scan)
        ok = tg1c_probe_put(
            p,
            sink,
            ctx,
            snprintf(
                t,
                cap,
                "#scan,set=%s,from=%02X,group=%u,groups=%u,candidates=%u,period_ms=%lu,burst_ms=%lu,"
                "burst_forced=1,burst_option_ms=%u,scan_n=1,control_n=2,control_lead_ms=%lu,"
                "frame=candidate[index%%group]\n",
                tg1c_scan_set_str(p->seq.sc_set),
                p->seq.sc_from,
                p->seq.sc_group,
                p->seq.sw_items,
                (unsigned)tg1c_scan_count(p->seq.sc_set),
                (unsigned long)TG1C_LISTEN_PERIOD_MS,
                (unsigned long)TG1C_SWEEP_BURST_MS,
                p->burst_option_ms,
                (unsigned long)TG1C_CONTROL_LEAD_MS));
    if(ok && dil) {
        uint8_t fb[TG1C_WAKE_BYTES] = {0};
        tg1c_wake_bytes(p->seq.dl_fill, fb);
        ok = tg1c_probe_put(
            p,
            sink,
            ctx,
            snprintf(
                t,
                cap,
                "#dilution,fill_bit=%u,fill_frame=%02X%02X%02X%02X%02X%02X,ratios=2/4/8/16/32,trials=%u,"
                "period_ms=%lu,burst_ms=%lu,burst_forced=1,burst_option_ms=%u,diluted_n=1,control_n=2,"
                "stock_frame=index%%ratio==0,control_lead_ms=%lu\n",
                p->seq.dl_fill,
                fb[0],
                fb[1],
                fb[2],
                fb[3],
                fb[4],
                fb[5],
                (unsigned)TG1C_DIL_TRIALS,
                (unsigned long)TG1C_LISTEN_PERIOD_MS,
                (unsigned long)TG1C_SWEEP_BURST_MS,
                p->burst_option_ms,
                (unsigned long)TG1C_CONTROL_LEAD_MS));
    }
    if(ok && sweep)
        ok = tg1c_probe_put(
            p,
            sink,
            ctx,
            snprintf(
                t,
                cap,
                "#sweep,bits=%u-%u,period_ms=%lu,burst_ms=%lu,burst_forced=1,burst_option_ms=%u,"
                "variant_n=1,control_n=2,tries=%u,control_lead_ms=%lu\n",
                p->seq.sw_first,
                p->seq.sw_first + (unsigned)TG1C_SWEEP_BITS - 1u,
                (unsigned long)TG1C_LISTEN_PERIOD_MS,
                (unsigned long)TG1C_SWEEP_BURST_MS,
                p->burst_option_ms,
                (unsigned)TG1C_SWEEP_TRIES,
                (unsigned long)TG1C_CONTROL_LEAD_MS));
    if(ok) {
        const char* control = p->seq.control_ok ? "responded" :
                              (p->sel == TG1C_SEL_A || p->sel == TG1C_SEL_SEQUENCE || grid) ?
                                                  "no_response" :
                                                  "not_run";
        char status[32];
        tg1c_seq_status_text(&p->seq, status, sizeof(status));
        ok = tg1c_probe_put(
            p,
            sink,
            ctx,
            snprintf(
                t,
                cap,
                "#control,%s\n#status,%s\n#fault,%s\n#telemetry_log,kept=%u,dropped=%lu\n",
                control,
                status,
                p->fault ? p->fault : "",
                p->tele_count,
                (unsigned long)p->tele_dropped));
    }
    /* Sweep: one line per bit of the range, then the totals and where it stood
     * when the session ended. `frame` is the six data bytes sent for that bit. */
    for(uint8_t i = 0; ok && sweep && i < TG1C_SWEEP_BITS; i++) {
        uint8_t bytes[TG1C_WAKE_BYTES] = {0};
        tg1c_wake_bytes(p->seq.sw_first + i, bytes);
        ok = tg1c_probe_put(
            p,
            sink,
            ctx,
            snprintf(
                t,
                cap,
                "#bit,%u,%s,frame=%02X%02X%02X%02X%02X%02X,variant_bursts=%u\n",
                p->seq.sw_first + i,
                tg1c_bit_result_str(p->seq.sw_result[i]),
                bytes[0],
                bytes[1],
                bytes[2],
                bytes[3],
                bytes[4],
                bytes[5],
                p->seq.sw_tries[i]));
    }
    if(ok && sweep) {
        char at[8];
        if(tg1c_seq_sweep_bit(&p->seq) >= 0)
            snprintf(at, sizeof(at), "%d", tg1c_seq_sweep_bit(&p->seq));
        else
            snprintf(at, sizeof(at), "-");
        ok = tg1c_probe_put(
            p,
            sink,
            ctx,
            snprintf(
                t,
                cap,
                "#sweep_result,woke=%u,silent=%u,unverified=%u,not_run=%u,ended_at_bit=%s,"
                "next_role=%s,strays=%u\n"
                "#note_sweep,woke: the spare answered while the variant was on air; silent: no "
                "answer, and the stock control one period later answered; the period is a bench "
                "candidate, so check first_reply_ms (near 5000 in a variant, near 4000 in a control, "
                "when the timing held)\n",
                tg1c_seq_sweep_count(&p->seq, TG1C_BIT_WOKE),
                tg1c_seq_sweep_count(&p->seq, TG1C_BIT_SILENT),
                tg1c_seq_sweep_count(&p->seq, TG1C_BIT_UNVERIFIED),
                tg1c_seq_sweep_count(&p->seq, TG1C_BIT_NOT_RUN),
                at,
                p->seq.status == TG1C_ST_SWEEP_DONE ? "-" : tg1c_role_str(p->seq.sw_role),
                p->seq.sw_strays));
    }
    /* Dilution: one line per trial of the ladder, then per-ratio totals. */
    for(uint8_t i = 0; ok && dil && i < TG1C_DIL_ITEMS; i++)
        ok = tg1c_probe_put(
            p,
            sink,
            ctx,
            snprintf(
                t,
                cap,
                "#trial,%u,%u,%s,diluted_bursts=%u\n",
                kTg1cDilRatio[i / TG1C_DIL_TRIALS],
                (unsigned)(i % TG1C_DIL_TRIALS + 1u),
                tg1c_bit_result_str(p->seq.sw_result[i]),
                p->seq.sw_tries[i]));
    if(ok && dil) {
        char at[12];
        if(tg1c_seq_dil_ratio(&p->seq))
            snprintf(at, sizeof(at), "%u/%u", tg1c_seq_dil_ratio(&p->seq), tg1c_seq_dil_trial(&p->seq));
        else
            snprintf(at, sizeof(at), "-");
        int n = snprintf(t, cap, "#dilution_result");
        for(int ri = 0; ri < (int)TG1C_DIL_RATIOS; ri++) {
            unsigned woke = tg1c_seq_dil_count(&p->seq, ri, TG1C_BIT_WOKE);
            n += snprintf(
                t + n,
                cap - (size_t)n,
                ",ratio%u=%u/%u",
                kTg1cDilRatio[ri],
                woke,
                woke + tg1c_seq_dil_count(&p->seq, ri, TG1C_BIT_SILENT));
        }
        n += snprintf(
            t + n,
            cap - (size_t)n,
            ",woke=%u,silent=%u,unverified=%u,not_run=%u,ended_at=%s,next_role=%s,strays=%u\n",
            tg1c_seq_dil_count(&p->seq, -1, TG1C_BIT_WOKE),
            tg1c_seq_dil_count(&p->seq, -1, TG1C_BIT_SILENT),
            tg1c_seq_dil_count(&p->seq, -1, TG1C_BIT_UNVERIFIED),
            tg1c_seq_dil_count(&p->seq, -1, TG1C_BIT_NOT_RUN),
            at,
            p->seq.status == TG1C_ST_DILUTION_DONE ? "-" : tg1c_role_name(p->seq.sw_role, p->sel),
            p->seq.sw_strays);
        ok = tg1c_probe_put(p, sink, ctx, n);
        if(ok)
            ok = tg1c_probe_put(
                p,
                sink,
                ctx,
                snprintf(
                    t,
                    cap,
                    "#note_dilution,ratioN=a/b: the spare answered a of the b diluted bursts that have a "
                    "verified result (woke, or silent with the stock control one period later "
                    "answered); in a diluted burst the stock wake is every Nth frame, the rest are the "
                    "filler; the period is a bench candidate\n"));
    }
    /* Offset test: one line per offset, then the map and the totals. */
    for(uint8_t i = 0; ok && off && i < TG1C_OFF_ITEMS; i++)
        ok = tg1c_probe_put(
            p,
            sink,
            ctx,
            snprintf(
                t,
                cap,
                "#offset,%u,%s,diluted_bursts=%u\n",
                i,
                tg1c_bit_result_str(p->seq.sw_result[i]),
                p->seq.sw_tries[i]));
    if(ok && off) {
        char at[8], map[TG1C_OFF_ITEMS + 1u];
        if(tg1c_seq_off_offset(&p->seq) >= 0)
            snprintf(at, sizeof(at), "%d", tg1c_seq_off_offset(&p->seq));
        else
            snprintf(at, sizeof(at), "-");
        tg1c_seq_off_map(&p->seq, map);
        ok = tg1c_probe_put(
            p,
            sink,
            ctx,
            snprintf(
                t,
                cap,
                "#offset_result,map=%s,woke=%u,silent=%u,unverified=%u,not_run=%u,ended_at=%s,"
                "next_role=%s,strays=%u\n",
                map,
                tg1c_seq_item_count(&p->seq, TG1C_BIT_WOKE),
                tg1c_seq_item_count(&p->seq, TG1C_BIT_SILENT),
                tg1c_seq_item_count(&p->seq, TG1C_BIT_UNVERIFIED),
                tg1c_seq_item_count(&p->seq, TG1C_BIT_NOT_RUN),
                at,
                p->seq.status == TG1C_ST_OFFSET_DONE ? "-" : tg1c_role_name(p->seq.sw_role, p->sel),
                p->seq.sw_strays));
        if(ok)
            ok = tg1c_probe_put(
                p,
                sink,
                ctx,
                snprintf(
                    t,
                    cap,
                    "#note_offset,map: one character per offset 0-15, W the spare answered the diluted "
                    "burst, . it did not and the stock control one period later answered, ? neither "
                    "answered, - not run; the stock wake is the frames with index %% 16 == offset, the "
                    "rest are the filler; first_reply_frame counts the frames completed when the first "
                    "reply was decoded; the period is a bench candidate\n"));
    }
    /* Scan: one line per group with the frames it carried, then the totals. */
    for(uint8_t g = 0; ok && scan && g < p->seq.sw_items; g++) {
        uint8_t first = tg1c_seq_scan_first(&p->seq, g), cnt = tg1c_seq_scan_n(&p->seq, g);
        int n = snprintf(
            t,
            cap,
            "#group,%02X,%02X,%s,frames=",
            first,
            first + cnt - 1u,
            tg1c_bit_result_str(p->seq.sw_result[g]));
        for(uint8_t j = 0; j < cnt; j++) {
            uint8_t fb[TG1C_WAKE_BYTES] = {0};
            tg1c_scan_bytes(p->seq.sc_set, (uint16_t)(first + j), fb);
            n += snprintf(
                t + n,
                cap - (size_t)n,
                "%s%02X%02X%02X%02X%02X%02X",
                j ? "/" : "",
                fb[0],
                fb[1],
                fb[2],
                fb[3],
                fb[4],
                fb[5]);
        }
        n += snprintf(t + n, cap - (size_t)n, ",scan_bursts=%u\n", p->seq.sw_tries[g]);
        ok = tg1c_probe_put(p, sink, ctx, n);
    }
    if(ok && scan) {
        char at[8];
        if(tg1c_seq_scan_n(&p->seq, p->seq.sw_idx))
            snprintf(at, sizeof(at), "%02X", tg1c_seq_scan_first(&p->seq, p->seq.sw_idx));
        else
            snprintf(at, sizeof(at), "-");
        int n = snprintf(
            t,
            cap,
            "#scan_result,woke=%u,silent=%u,unverified=%u,not_run=%u,woke_groups=",
            tg1c_seq_item_count(&p->seq, TG1C_BIT_WOKE),
            tg1c_seq_item_count(&p->seq, TG1C_BIT_SILENT),
            tg1c_seq_item_count(&p->seq, TG1C_BIT_UNVERIFIED),
            tg1c_seq_item_count(&p->seq, TG1C_BIT_NOT_RUN));
        bool any = false;
        for(uint8_t g = 0; g < p->seq.sw_items; g++) {
            if(p->seq.sw_result[g] != TG1C_BIT_WOKE) continue;
            n += snprintf(t + n, cap - (size_t)n, "%s%02X", any ? "/" : "", tg1c_seq_scan_first(&p->seq, g));
            any = true;
        }
        n += snprintf(
            t + n,
            cap - (size_t)n,
            "%s,ended_at=%s,next_role=%s,strays=%u\n",
            any ? "" : "-",
            at,
            p->seq.status == TG1C_ST_SCAN_DONE ? "-" : tg1c_role_name(p->seq.sw_role, p->sel),
            p->seq.sw_strays);
        ok = tg1c_probe_put(p, sink, ctx, n);
        if(ok)
            ok = tg1c_probe_put(
                p,
                sink,
                ctx,
                snprintf(
                    t,
                    cap,
                    "#note_scan,a group is named by its first and last candidate index in hex; woke: the "
                    "spare answered while the group's frames were on air (frame i of the burst is "
                    "candidate i %% group); silent: no answer, and the stock control one period later "
                    "answered; a woke group names no single frame; byte index 39 is the stock wake; "
                    "the period is a bench candidate\n"));
    }
    if(ok && !grid)
        ok = tg1c_probe_put(
            p,
            sink,
            ctx,
            snprintf(
                t,
                cap,
                "#note,C and D are hypotheses (615E as a 16-bit LF ID is unproven); a reply or "
                "a new status is a candidate; silence is inconclusive\n"));
    if(ok)
        ok = tg1c_probe_put(
            p,
            sink,
            ctx,
            snprintf(
                t,
                cap,
                "tick,payload,lf_id,body,lead_us,gap_ms,burst_ms,frames,rx_hz,telemetry,"
                "sensor_id,pairs,first_reply_ms,sync,sync_like,crc_valid,reply,overruns,"
                "tx_fault,intervals,clean_max,clean_runs,rssi_peak,rssi_avg,rssi_strong,"
                "rssi_samples,wait_intervals,wait_clean_max,wait_clean_runs,wait_rssi_peak,"
                "wait_rssi_avg,wait_rssi_strong,wait_rssi_samples%s,first_reply_frame\n",
                sweep ? ",role,bit,planned_tick,last_spare_tick" :
                dil   ? ",role,ratio,trial,stock_frames,fill_frames,planned_tick,last_spare_tick" :
                off   ? ",role,ratio,offset,stock_frames,fill_frames,planned_tick,last_spare_tick" :
                scan  ? ",role,first,group,planned_tick,last_spare_tick" :
                        ""));

    for(uint8_t i = 0; ok && i < p->row_count; i++) {
        const TG1cLogRow* r = &p->rows[i];
        char lf_id[8], id[12], pairs[48], reply[2 * TG1C_BL_MAX_BYTES + 1];
        uint16_t lf = tg1c_payload_lf_id((TG1cPayload)r->payload);
        if(lf)
            snprintf(lf_id, sizeof(lf_id), "%04X", lf);
        else
            snprintf(lf_id, sizeof(lf_id), "-");
        id[0] = 0;
        if(r->tally.frames) snprintf(id, sizeof(id), "%08lX", (unsigned long)r->tally.last_id);
        tg1c_tally_pairs(&r->tally, pairs, sizeof(pairs));
        reply[0] = 0;
        if(r->crc_valid)
            for(uint8_t b = 0; b < p->rx.reply_len; b++)
                snprintf(reply + 2 * b, 3, "%02X", p->rx.reply[b]);
        ok = tg1c_probe_put(
            p,
            sink,
            ctx,
            snprintf(
                t,
                cap,
                "%lu,%c,%s,%s,%u,%u,%u,%u,%lu,%u,%s,%s,%ld,%lu,%lu,%u,%s,%lu,%s",
                (unsigned long)r->tick,
                tg1c_row_letter(r),
                lf_id,
                tg1c_payload_body_polarity((TG1cPayload)r->payload),
                r->lead_us,
                r->gap_ms,
                r->burst_ms,
                r->frames,
                (unsigned long)r->rx_hz,
                r->tally.frames,
                id,
                pairs,
                (long)r->first_reply_ms,
                (unsigned long)r->sync_hits,
                (unsigned long)r->sync_like_hits,
                r->crc_valid ? 1u : 0u,
                reply,
                (unsigned long)r->overruns,
                r->fault ? r->fault : ""));
        for(int part = 0; ok && part < 2; part++) {
            const TG1cRxStats* x = &r->rx[part];
            ok = tg1c_probe_put(
                p,
                sink,
                ctx,
                snprintf(
                    t,
                    cap,
                    ",%lu,%lu,%lu,%d,%ld,%lu,%lu",
                    (unsigned long)x->intervals,
                    (unsigned long)x->clean_max,
                    (unsigned long)x->clean_runs,
                    x->rssi_samples ? (int)x->rssi_peak : 0,
                    x->rssi_samples ? (long)((int64_t)x->rssi_sum / (int64_t)x->rssi_samples) : 0l,
                    (unsigned long)x->rssi_strong,
                    (unsigned long)x->rssi_samples));
        }
        if(ok && grid) {
            /* Sweep: role, bit, planned_tick, last_spare_tick. Dilution: role,
             * ratio, trial, stock_frames, fill_frames, planned_tick,
             * last_spare_tick. Offset test: as dilution with the offset in
             * place of the trial. Scan: role, first (hex), group, planned_tick,
             * last_spare_tick. "-" where there is none. */
            char bit[40], planned[12], last[12];
            snprintf(bit, sizeof(bit), "%s", (dil || off || scan) ? "-,-" : "-");
            snprintf(planned, sizeof(planned), "-");
            snprintf(last, sizeof(last), "-");
            if(sweep && r->bit >= 0) snprintf(bit, sizeof(bit), "%d", r->bit);
            if(dil && r->ratio) snprintf(bit, sizeof(bit), "%u,%u", r->ratio, r->trial);
            if(off && r->ratio) snprintf(bit, sizeof(bit), "%u,%u", r->ratio, r->offset);
            if(scan && r->scan_n) snprintf(bit, sizeof(bit), "%02X,%u", r->scan_first, r->scan_n);
            if(dil || off) {
                size_t n = strlen(bit);
                snprintf(
                    bit + n,
                    sizeof(bit) - n,
                    ",%u,%u",
                    (unsigned)(uint16_t)(r->frames - r->fill_frames),
                    (unsigned)r->fill_frames);
            }
            if(r->timed) snprintf(planned, sizeof(planned), "%lu", (unsigned long)r->planned_tick);
            if(r->have_spare_tick)
                snprintf(last, sizeof(last), "%lu", (unsigned long)r->last_spare_tick);
            ok = tg1c_probe_put(
                p,
                sink,
                ctx,
                snprintf(t, cap, ",%s,%s,%s,%s", tg1c_role_name(r->role, p->sel), bit, planned, last));
        }
        /* v0.3.2: the last column of every row, in every selection. */
        if(ok) ok = tg1c_probe_put(p, sink, ctx, snprintf(t, cap, ",%d\n", (int)r->first_reply_frame));
    }
    return ok;
}

/* True when at least one telemetry frame was kept. */
static inline bool tg1c_probe_has_tele(const TG1cProbe* p) {
    return p->tele_count > 0;
}

/* The per-frame telemetry log (v0.2.2): one line per kept frame, in arrival
 * order. row is the 1-based burst row of the session CSV the frame belongs
 * to; ms_in_row counts from that burst's start. */
static inline bool tg1c_probe_write_tele(TG1cProbe* p, TG1cSink sink, void* ctx) {
    static const char* const phase_name[TG1C_PH_COUNT] = {"burst", "listen", "quiet", "wait", "observe"};
    char* t = p->text;
    size_t cap = sizeof(p->text);
    bool ok = tg1c_probe_put(
        p,
        sink,
        ctx,
        snprintf(
            t,
            cap,
            "#app,continental_tg1c_lf,%s\n#session_tick,%lu\n#frames,kept=%u,dropped=%lu\n"
            "tick,row,payload,phase,ms_in_row,rx_hz,id,raw,b4,b7,rssi_dbm,known\n",
            TG1C_APP_VERSION,
            (unsigned long)p->session_tick,
            p->tele_count,
            (unsigned long)p->tele_dropped));
    for(uint8_t i = 0; ok && i < p->tele_count; i++) {
        const TG1cTeleEntry* e = &p->tele[i];
        const TG1cLogRow* r = e->row < p->row_count ? &p->rows[e->row] : NULL;
        uint8_t phase = (uint8_t)(e->flags >> TG1C_TELE_PHASE_SHIFT);
        char raw[2 * sizeof(e->raw) + 1];
        for(size_t b = 0; b < sizeof(e->raw); b++) snprintf(raw + 2 * b, 3, "%02X", e->raw[b]);
        ok = tg1c_probe_put(
            p,
            sink,
            ctx,
            snprintf(
                t,
                cap,
                "%lu,%u,%c,%s,%lu,%lu,%.8s,%s,%02X,%02X,%d,%u\n",
                (unsigned long)e->tick,
                r ? e->row + 1u : 0u,
                r ? tg1c_row_letter(r) : '-',
                phase < TG1C_PH_COUNT ? phase_name[phase] : "?",
                (unsigned long)(r ? e->tick - r->tick : 0),
                (unsigned long)((e->flags & TG1C_TELE_F_RX_NXP) ? TG1C_RX_HZ_NXP : TG1C_RX_HZ_WAKE),
                raw,
                raw,
                e->raw[4],
                e->raw[7],
                e->rssi_dbm,
                (e->flags & TG1C_TELE_F_KNOWN) ? 1u : 0u));
    }
    return ok;
}

/* True when a sync or sync-like hit armed the evidence ring. */
static inline bool tg1c_probe_has_raw(const TG1cProbe* p) {
    return tg1c_ring_triggered(&p->ring);
}

/* True when a CRC-valid reply armed the reply ring (v0.2.4). */
static inline bool tg1c_probe_has_reply_raw(const TG1cProbe* p) {
    return tg1c_ring_triggered(&p->reply_ring);
}

/* Stamped TPRAW02: a '#'-prefixed JSON line the host decoder reads, the magic,
 * then the ring's records oldest first. */
static inline bool tg1c_probe_write_ring(TG1cProbe* p, const TG1cRing* ring, TG1cSink sink, void* ctx) {
    uint16_t n = tg1c_ring_count(ring);
    bool ok = tg1c_probe_put(
        p,
        sink,
        ctx,
        snprintf(
            p->text,
            sizeof(p->text),
            "#{\"app\":\"continental_tg1c_lf\",\"version\":\"%s\",\"encoding\":\"nrz\","
            "\"bit_us\":%u,\"rx_hz\":%lu,\"sync\":\"%s\",\"sync_like\":%s,\"crc_valid\":%s,"
            "\"records\":%u,\"complete\":%s,\"read_only\":%s}\n",
            TG1C_APP_VERSION,
            (unsigned)TG1C_BL_BIT_US,
            (unsigned long)ring->rx_hz,
            tg1c_btldr_variant_str(ring->variant),
            ring->sync_like ? "true" : "false",
            ring->crc_valid ? "true" : "false",
            n,
            tg1c_ring_frozen(ring) ? "true" : "false",
            (tg1c_sel_is_grid(p->sel) || p->sel == TG1C_SEL_PARK) ? "false" : "true"));
    if(ok) ok = sink(ctx, "TPRAW02\n", 8);
    for(uint16_t i = 0; ok && i < n; i++) ok = sink(ctx, tg1c_ring_at(ring, i), sizeof(TG1cRawRecord));
    return ok;
}
/* probe_<tick>.tpraw: around the first sync or sync-like hit of the session. */
static inline bool tg1c_probe_write_raw(TG1cProbe* p, TG1cSink sink, void* ctx) {
    return tg1c_probe_write_ring(p, &p->ring, sink, ctx);
}
/* reply_<tick>.tpraw: the CRC-valid reply, with what led up to it. */
static inline bool tg1c_probe_write_reply_raw(TG1cProbe* p, TG1cSink sink, void* ctx) {
    return tg1c_probe_write_ring(p, &p->reply_ring, sink, ctx);
}
