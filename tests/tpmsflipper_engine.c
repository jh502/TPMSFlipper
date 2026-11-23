// SPDX-License-Identifier: GPL-2.0-or-later
/* Host test for flipper/tpmsflipper/wt_engine.h and wt_core.h: the code the
 * device worker runs, on a simulated clock, receiver, LF coil and sensor.
 *
 * The clock advances 1 ms per delay. The sensor model follows what was
 * measured on real sensors (analysis/tg1c-park-*-20261112):
 *   - it checks for LF once per period (under a second in ship mode, about
 *     31 s in normal mode), counted from the end of its last reply;
 *   - to the wake frame it replies every 150 ms while the LF lasts and for
 *     two more frames after it stops;
 *   - to the exit-ship-mode frame it answers with five groups of frames
 *     1.4 s apart, LF or no LF, and (if the model says the frame works) is in
 *     normal mode from then on.
 */
#include "wt_engine.h"
#include "wt_log.h"
#include "wt_link.h"
#include <assert.h>
#include <stdio.h>

enum { FRAME_NONE = 0, FRAME_A, FRAME_E, FRAME_OTHER };
enum { E_IGNORED = 0, E_WORKS, E_ACK_ONLY, E_SILENCES };

typedef struct {
    bool present;
    uint32_t id;
    uint8_t b4, b5, b6;
    uint32_t period, next_listen;
    int e_effect;
    /* wake reply */
    bool replying;
    uint32_t next_frame;
    /* acknowledgement block */
    uint32_t ack_at[15];
    uint8_t ack_b4[15];
    int ack_n, ack_i;
    uint8_t after_b4;
    bool dead;
} Sensor;

typedef struct {
    WtEngine* e;
    uint32_t now;
    bool cancel;
    uint32_t cancel_at;
    /* receiver */
    bool rx_on, rx_wide, rx_fail;
    int rx_starts, rx_stops;
    /* LF coil */
    bool prepared, running, prepare_fail;
    int frame; /* FRAME_* of the prepared envelope */
    uint32_t frame_end, last_frame_end;
    int prepares, frees, starts;
    int sent[4]; /* complete frames per FRAME_* */
    uint32_t first_tx[4], last_tx[4];
    int progress_calls;
    uint8_t phases_seen;
    Sensor s;
    /* a frame from some other sensor, once */
    uint32_t foreign_at, foreign_id;
} Sim;

static void emit(Sim* sim, uint32_t id, uint8_t b4, uint8_t b5, uint8_t b6, int rssi) {
    uint8_t raw[9] = {(uint8_t)(id >> 24), (uint8_t)(id >> 16), (uint8_t)(id >> 8), (uint8_t)id, b4, b5, b6, 0x01, 0};
    for(int i = 0; i < 8; i++) raw[8] ^= raw[i];
    if(sim->rx_on) wt_engine_on_frame(sim->e, id, raw, rssi);
}
static bool lf_on_air(const Sim* sim) {
    return sim->running || (sim->last_frame_end && sim->now - sim->last_frame_end <= 20);
}
static void sensor_step(Sim* sim) {
    Sensor* s = &sim->s;
    if(sim->foreign_at && sim->now >= sim->foreign_at) {
        sim->foreign_at = 0;
        emit(sim, sim->foreign_id, 0x87, 0xA0, 0x40, -70);
    }
    if(!s->present || s->dead) return;
    if(s->ack_i < s->ack_n) {
        while(s->ack_i < s->ack_n && sim->now >= s->ack_at[s->ack_i]) {
            emit(sim, s->id, s->ack_b4[s->ack_i], s->b5, s->b6, -46);
            s->ack_i++;
        }
        if(s->ack_i == s->ack_n) {
            if(s->e_effect == E_WORKS) s->b4 = s->after_b4, s->period = 31000;
            if(s->e_effect == E_SILENCES) s->dead = true;
        }
        return;
    }
    if(s->replying) {
        if(sim->now >= s->next_frame) {
            emit(sim, s->id, s->b4, s->b5, s->b6, -46);
            s->next_frame += 150;
        }
        if(!lf_on_air(sim) && sim->now - sim->last_frame_end > 300) {
            s->replying = false;
            s->next_listen = sim->now + s->period;
        }
        return;
    }
    if(sim->now < s->next_listen) return;
    if(!lf_on_air(sim)) {
        s->next_listen += s->period;
        return;
    }
    if(sim->frame == FRAME_A) {
        s->replying = true;
        s->next_frame = sim->now + 100;
    } else if(sim->frame == FRAME_E && s->e_effect != E_IGNORED) {
        bool ship = (s->b4 & 0xC0) == 0xC0;
        s->ack_n = 15, s->ack_i = 0;
        for(int g = 0; g < 5; g++)
            for(int k = 0; k < 3; k++) {
                s->ack_at[g * 3 + k] = sim->now + 100 + (uint32_t)g * 1400 + (uint32_t)k * 130;
                s->ack_b4[g * 3 + k] = ship ? (g ? 0x76 : 0x75) : 0x77;
            }
        /* its next listening moment: 32.1 s after the first frame of the answer */
        s->next_listen = sim->now + 100 + (s->e_effect == E_WORKS ? 32100 : 33000);
    } else {
        s->next_listen += s->period;
    }
}

static uint32_t p_tick(void* ctx) {
    return ((Sim*)ctx)->now;
}
static void p_delay(void* ctx) {
    Sim* sim = ctx;
    sim->now++;
    if(sim->cancel_at && sim->now >= sim->cancel_at) sim->cancel = true;
}
static bool p_cancelled(void* ctx) {
    return ((Sim*)ctx)->cancel;
}
static bool p_rx_start(void* ctx, bool wide) {
    Sim* sim = ctx;
    assert(!sim->rx_on);
    sim->rx_starts++;
    if(sim->rx_fail) return false;
    sim->rx_on = true, sim->rx_wide = wide;
    return true;
}
static void p_rx_pump(void* ctx, uint32_t timeout_ms) {
    Sim* sim = ctx;
    if(sim->running && sim->now >= sim->frame_end) { /* the frame has gone out */ }
    sensor_step(sim);
    if(timeout_ms) p_delay(ctx);
}
static int p_rx_rssi(void* ctx) {
    Sim* sim = ctx;
    return sim->s.replying || (sim->s.ack_i < sim->s.ack_n && sim->s.ack_i) ? -46 : -95;
}
static void p_rx_stop(void* ctx) {
    Sim* sim = ctx;
    if(sim->rx_on) sim->rx_stops++;
    sim->rx_on = false;
}
static bool p_lf_prepare(void* ctx, const uint8_t* slots, size_t n, uint16_t half_us, uint16_t lead_us) {
    Sim* sim = ctx;
    uint8_t a[TG1C_SLOTS_A], e[TG1C_SLOTS_A];
    assert(!sim->running && !sim->prepared);
    assert(half_us == 128 && lead_us == 2048 && n == TG1C_SLOTS_A);
    assert(sim->rx_on); /* never transmit with the receiver off */
    sim->prepares++;
    if(sim->prepare_fail) return false;
    tg1c_build_frame(TG1C_PAYLOAD_A, a, sizeof(a));
    tg1c_build_frame(TG1C_PAYLOAD_E, e, sizeof(e));
    sim->frame = !memcmp(slots, a, n) ? FRAME_A : !memcmp(slots, e, n) ? FRAME_E : FRAME_OTHER;
    sim->prepared = true;
    return true;
}
static bool p_lf_start(void* ctx) {
    Sim* sim = ctx;
    assert(sim->prepared && !sim->running);
    sim->running = true;
    sim->frame_end = sim->now + 26;
    sim->starts++;
    return true;
}
static bool p_lf_done(void* ctx) {
    Sim* sim = ctx;
    return sim->running && sim->now >= sim->frame_end;
}
static bool p_lf_error(void* ctx) {
    (void)ctx;
    return false;
}
static void p_lf_stop(void* ctx) {
    Sim* sim = ctx;
    if(sim->running) {
        if(sim->now >= sim->frame_end) {
            if(!sim->sent[sim->frame]) sim->first_tx[sim->frame] = sim->now;
            sim->sent[sim->frame]++;
            sim->last_tx[sim->frame] = sim->now;
        }
        sim->last_frame_end = sim->now;
    }
    sim->running = false;
}
static void p_lf_free(void* ctx) {
    Sim* sim = ctx;
    assert(!sim->running);
    if(sim->prepared) sim->frees++;
    sim->prepared = false;
}
static void p_progress(void* ctx, WtEngine* e) {
    Sim* sim = ctx;
    sim->progress_calls++;
    sim->phases_seen |= (uint8_t)(1u << e->prog.phase);
    assert(e->prog.lf_on || !sim->running);
}

static WtEngine eng;
static WtPort port;
static void sim_init(Sim* sim) {
    memset(sim, 0, sizeof(*sim));
    sim->e = &eng;
    sim->now = 1000;
    port = (WtPort){sim,        p_tick,       p_delay,    p_cancelled, p_rx_start, p_rx_pump, p_rx_rssi, p_rx_stop,
                    p_lf_prepare, p_lf_start, p_lf_done, p_lf_error,  p_lf_stop,  p_lf_free, p_progress};
    wt_engine_init(&eng, &port);
}
static void sensor(Sim* sim, uint32_t id, uint8_t b4, uint32_t period, uint32_t first_listen, int e_effect) {
    sim->s = (Sensor){.present = true, .id = id, .b4 = b4, .b5 = 0xA7, .b6 = 0x33, .period = period,
                      .next_listen = sim->now + first_listen, .e_effect = e_effect, .after_b4 = 0x86};
}
/* After any operation: carrier off, envelope freed, receiver stopped. */
static void idle(const Sim* sim) {
    assert(!sim->running && !sim->prepared && !sim->rx_on);
    assert(sim->prepares == sim->frees + (sim->prepare_fail ? sim->prepares : 0) || sim->prepare_fail);
    assert(sim->rx_starts == sim->rx_stops + (sim->rx_fail ? 1 : 0));
    assert(eng.prog.phase == WT_PH_IDLE && !eng.prog.lf_on);
}

static void test_core(void) {
    char t[16];
    /* B5 x 1.3725 kPa: the car's own readings are whole multiples of it */
    wt_fmt_pressure(t, sizeof(t), 0xA7, WT_P_BAR);
    assert(!strcmp(t, "2.29"));
    wt_fmt_pressure(t, sizeof(t), 0xA7, WT_P_PSI);
    assert(!strcmp(t, "33.2"));
    wt_fmt_pressure(t, sizeof(t), 0xA7, WT_P_KPA);
    assert(!strcmp(t, "229"));
    wt_fmt_pressure(t, sizeof(t), 0xAB, WT_P_BAR);
    assert(!strcmp(t, "2.35"));
    wt_fmt_pressure(t, sizeof(t), 0, WT_P_BAR);
    assert(!strcmp(t, "0.00"));
    wt_fmt_pressure(t, sizeof(t), 0xFF, WT_P_PSI);
    assert(!strcmp(t, "50.8"));
    wt_fmt_pressure(t, sizeof(t), 0xFF, WT_P_BAR);
    assert(!strcmp(t, "3.50"));
    /* 176, 178 and 182 counts: the car showed 241.57, 244.31 and 249.8 kPa, and 35, 35 and 36 psi on the dash */
    assert(wt_kpa_x100(176) == 24156 && wt_kpa_x100(178) == 24431 && wt_kpa_x100(182) == 24980);
    wt_fmt_pressure(t, sizeof(t), 176, WT_P_PSI);
    assert(!strcmp(t, "35.0"));
    wt_fmt_pressure(t, sizeof(t), 182, WT_P_PSI);
    assert(!strcmp(t, "36.2"));
    assert(wt_kpa_x10(0xA7) == 2292);
    assert(wt_temp(0x34, WT_T_C) == 2 && wt_temp(0x34, WT_T_F) == 36 && wt_temp(0, WT_T_C) == -50 && wt_temp(0, WT_T_F) == -58);
    assert(wt_temp(0x49, WT_T_F) == 73);
    /* the status bytes seen so far, by their high digit (docs/TG1C-STATUS-CODES.md) */
    assert(wt_mode(0x87) == WT_MODE_PARK && wt_mode(0x86) == WT_MODE_PARK);
    assert(wt_mode(0x66) == WT_MODE_DRIVE && wt_mode(0x67) == WT_MODE_DRIVE);
    assert(wt_mode(0xF7) == WT_MODE_SHIP && wt_mode(0xF6) == WT_MODE_SHIP);
    assert(wt_mode(0x77) == WT_MODE_BUSY && wt_mode(0x75) == WT_MODE_BUSY && wt_mode(0x76) == WT_MODE_BUSY);
    assert(wt_mode(0xB7) == WT_MODE_UNKNOWN && wt_mode(0x37) == WT_MODE_UNKNOWN && wt_mode(0x00) == WT_MODE_UNKNOWN);
    assert(wt_mode_working(WT_MODE_PARK) && wt_mode_working(WT_MODE_DRIVE));
    assert(!wt_mode_working(WT_MODE_SHIP) && !wt_mode_working(WT_MODE_BUSY) && !wt_mode_working(WT_MODE_UNKNOWN));
    wt_mode_label(t, sizeof(t), 0x86);
    assert(!strcmp(t, "PARK MODE"));
    wt_mode_label(t, sizeof(t), 0x66);
    assert(!strcmp(t, "DRIVE"));
    wt_mode_label(t, sizeof(t), 0xF6);
    assert(!strcmp(t, "SHIP MODE"));
    wt_mode_label(t, sizeof(t), 0x75);
    assert(!strcmp(t, "BUSY"));
    wt_mode_label(t, sizeof(t), 0xB7); /* not placed: the byte itself */
    assert(!strcmp(t, "STATUS B7"));
    /* battery: read from a wake reply only; a frame sent on the sensor's own timing has none */
    assert(wt_batt(0x87) == WT_BATT_OK && wt_batt(0x86) == WT_BATT_OK && wt_batt(0xF6) == WT_BATT_OK);
    assert(wt_batt(0x66) == WT_BATT_NA && wt_batt(0x77) == WT_BATT_NA && wt_batt(0xA7) == WT_BATT_UNSURE);
    assert(wt_bars(-45) == 4 && wt_bars(-56) == 3 && wt_bars(-70) == 2 && wt_bars(-80) == 1 && wt_bars(-95) == 0);

    /* places: two sets of four wheels, the spare tyre, four spare sensors */
    char t2[20];
    assert(WT_SLOTS == 13 && wt_slot(1, WT_RL) == 6 && wt_slot_set(6) == 1 && wt_slot_wheel(6) == WT_RL);
    wt_slot_short(t2, sizeof(t2), wt_slot(0, WT_RL));
    assert(!strcmp(t2, "S-RL"));
    wt_slot_short(t2, sizeof(t2), wt_slot(1, WT_FR));
    assert(!strcmp(t2, "W-FR"));
    wt_slot_short(t2, sizeof(t2), WT_SLOT_TYRE);
    assert(!strcmp(t2, "TYRE"));
    wt_slot_short(t2, sizeof(t2), WT_SLOT_SPARE0 + 3);
    assert(!strcmp(t2, "SP4"));
    wt_slot_short(t2, sizeof(t2), WT_NO_SLOT);
    assert(!strcmp(t2, "--"));
    wt_slot_long(t2, sizeof(t2), wt_slot(1, WT_FR));
    assert(!strcmp(t2, "Winter FR"));
    wt_slot_long(t2, sizeof(t2), WT_SLOT_SPARE0 + 1);
    assert(!strcmp(t2, "Spare sensor 2"));
    wt_date_text(t2, sizeof(t2), (WtDate){2026, 11, 13});
    assert(!strcmp(t2, "13 Nov"));
    wt_date_text(t2, sizeof(t2), (WtDate){0, 0, 0});
    assert(!t2[0]);

    /* settings file: round trip, hand edits, a v0.1 file, rubbish */
    WtConfig c, d;
    char text[WT_CONFIG_TEXT_MAX];
    wt_config_default(&c);
    assert(c.punit == WT_P_BAR && c.tunit == WT_T_C && c.on_car == 0 && c.ask == WT_ASK_NEW && !c.phone && !c.slot[0].id);
    assert(wt_config_write(&c, text, sizeof(text)) && (WT_PHONE_LINK ? !!strstr(text, "phone=off\n") : !strstr(text, "phone")));
    /* the radio band: 433.92 MHz unless the file says 315 */
    assert(c.band == WT_BAND_433 && strstr(text, "band=433\n"));
    assert(wt_band_hz(WT_BAND_433) == 433920000u && wt_band_hz(WT_BAND_315) == 315000000u && wt_band_hz(7) == 433920000u);
    assert(wt_config_write(&c, text, sizeof(text)) && !strstr(text, "summer.")); /* empty places are left out */
    c.punit = WT_P_PSI, c.tunit = WT_T_F, c.on_car = 1, c.ask = WT_ASK_NEVER, c.phone = WT_PHONE_LINK, c.band = WT_BAND_315;
    wt_config_assign(&c, wt_slot(0, WT_RL), 0x0F3A6CB8);
    wt_config_assign(&c, wt_slot(1, WT_FL), 0x0F279DDC);
    wt_config_assign(&c, WT_SLOT_TYRE, 0x0F4EA3FA);
    wt_config_assign(&c, WT_SLOT_SPARE0 + 2, 0x0F61C9F5);
    assert(wt_config_note(&c, 0x0F3A6CB8, 0x86, 0xA7, 0x33, (WtDate){2026, 11, 13}));
    assert(wt_config_note(&c, 0x0F61C9F5, 0x87, 0x02, 0x49, (WtDate){0, 0, 0})); /* a reading, the clock not set */
    assert(!wt_config_note(&c, 0x12345678, 0x86, 0xA7, 0x33, (WtDate){2026, 11, 13})); /* no place: nothing kept */
    size_t n = wt_config_write(&c, text, sizeof(text));
    assert(n && n < sizeof(text));
    assert(strstr(text, "summer.RL=0F3A6CB8 86 A7 33 2026-11-13\n") && strstr(text, "winter.FL=0F279DDC\n"));
    assert(strstr(text, "spare_tyre=0F4EA3FA\n") && strstr(text, "spare3=0F61C9F5 87 02 49\n"));
    assert(strstr(text, "on_car=winter\n") && strstr(text, "ask=never\n"));
    assert(WT_PHONE_LINK ? !!strstr(text, "phone=on\n") : !strstr(text, "phone"));
    wt_config_parse(&d, "phone=on\n", 9); /* the public build does not take the link from a test build's file */
    assert(d.phone == (WT_PHONE_LINK != 0));
    assert(strstr(text, "band=315\n"));
    wt_config_parse(&d, text, n);
    assert(!memcmp(&c, &d, sizeof(c)));
    /* one sensor, one place: moving it empties the old place and takes its reading along */
    wt_config_assign(&c, WT_SLOT_SPARE0, 0x0F3A6CB8);
    assert(c.slot[WT_SLOT_SPARE0].id == 0x0F3A6CB8 && c.slot[WT_SLOT_SPARE0].seen && c.slot[WT_SLOT_SPARE0].b5 == 0xA7);
    assert(!c.slot[wt_slot(0, WT_RL)].id && wt_config_slot_of(&c, 0x0F3A6CB8) == WT_SLOT_SPARE0);
    /* a sensor put where another one is: that one loses its place */
    wt_config_assign(&c, WT_SLOT_TYRE, 0x0F3A6CB8);
    assert(wt_config_slot_of(&c, 0x0F4EA3FA) == WT_NO_SLOT && wt_config_slot_of(&c, 0x0F3A6CB8) == WT_SLOT_TYRE);
    assert(wt_config_slot_of(&c, 0) == WT_NO_SLOT && wt_config_slot_of(&c, 0x12345678) == WT_NO_SLOT);
    assert(wt_config_set_count(&c, 0) == 0 && wt_config_set_count(&c, 1) == 1 && wt_config_spare_count(&c) == 1);
    wt_config_remove(&c, 0x0F3A6CB8);
    assert(!c.slot[WT_SLOT_TYRE].id);
    uint32_t car[WT_PINS];
    wt_config_car_ids(&c, car); /* the winter set is on the car */
    assert(car[WT_FL] == 0x0F279DDC && !car[WT_FR] && !car[WT_WHEELS]);
    /* a file from v0.1: its four wheels become the Summer set */
    const char* v01 = "# TPMSFlipper 0.1.1\npressure=psi\ntemperature=C\nFL=0F279DDC\nFR=00000000\nRL=0F3A6CB8\nRR=0F4EA3FA\n";
    wt_config_parse(&d, v01, strlen(v01));
    assert(d.punit == WT_P_PSI && d.on_car == 0 && d.ask == WT_ASK_NEW && !d.phone); /* older files: link off */
    assert(d.band == WT_BAND_433); /* older files: the band every version before 0.4.1 listened on */
    assert(d.slot[wt_slot(0, WT_FL)].id == 0x0F279DDC && !d.slot[wt_slot(0, WT_FR)].id &&
           d.slot[wt_slot(0, WT_RL)].id == 0x0F3A6CB8 && d.slot[wt_slot(0, WT_RR)].id == 0x0F4EA3FA && !d.slot[wt_slot(0, WT_RL)].seen);
    /* edited by hand, badly */
    const char* hand = "pressure=kPa\r\ntemperature=C \nsummer.RL=f3a6cb8 86 a7 33 2026-11-13\nXX=1\nsummer.FR=notahex\n"
                       "winter.FL=123456789\nnonsense\n=\nwinter.RR=0F4EA3FA 86 A7\nspare1=0F61C9F5 87 02 49 2026-13-40\n"
                       "spare9=0F111111\nask=sometimes\non_car=garage\nphone=yes\nband=868\nband=31\nspare2=0F3A6CB8";
    wt_config_parse(&d, hand, strlen(hand));
    assert(d.punit == WT_P_KPA && d.tunit == WT_T_C && d.ask == WT_ASK_NEW && d.on_car == 0 && !d.phone);
    assert(d.band == WT_BAND_433); /* a band this app does not have is not taken */
    wt_config_parse(&d, "band=315", 8);
    assert(d.band == WT_BAND_315);
    wt_config_parse(&d, hand, strlen(hand));
    assert(!d.slot[wt_slot(0, WT_FR)].id && !d.slot[wt_slot(1, WT_FL)].id);
    assert(d.slot[wt_slot(1, WT_RR)].id == 0x0F4EA3FA && !d.slot[wt_slot(1, WT_RR)].seen); /* two bytes of three: the ID only */
    assert(d.slot[WT_SLOT_SPARE0].id == 0x0F61C9F5 && d.slot[WT_SLOT_SPARE0].seen && !d.slot[WT_SLOT_SPARE0].date.year);
    /* the same sensor on two lines: the later line is its place */
    assert(d.slot[WT_SLOT_SPARE0 + 1].id == 0x0F3A6CB8 && !d.slot[wt_slot(0, WT_RL)].id);
    wt_config_parse(&d, "", 0);
    assert(d.punit == WT_P_BAR && !d.slot[WT_SLOT_TYRE].id);
    /* every place full, with a reading and a date: it fits; a buffer too small gives nothing and no overrun */
    for(uint8_t i = 0; i < WT_SLOTS; i++) {
        c.slot[i] = (WtSlot){.id = 0xFFFFFFF0u + i};
        wt_config_note(&c, 0xFFFFFFF0u + i, 0xFF, 0xFF, 0xFF, (WtDate){2099, 12, 31});
    }
    n = wt_config_write(&c, text, sizeof(text));
    assert(n && n < WT_CONFIG_TEXT_MAX - 40);
    wt_config_parse(&d, text, n);
    assert(!memcmp(&c, &d, sizeof(c)));
    for(size_t cap = 0; cap < n; cap += 7) assert(wt_config_write(&c, text, cap) == 0);
    printf("PASS: core: pressure, temperature, Park/Drive/Ship; places for two sets, a spare tyre and spare sensors; settings file\n");
}

static void test_scan(void) {
    Sim sim;
    WtReading r;

    /* A sensor in ship mode answers inside a second. */
    sim_init(&sim);
    sensor(&sim, 0x0F3A6CB8, 0xF6, 500, 400, E_WORKS);
    uint32_t t0 = sim.now;
    assert(wt_engine_scan(&eng, 0, NULL, 0, &r) == WT_R_OK);
    idle(&sim);
    assert(r.id == 0x0F3A6CB8 && wt_b4(&r) == 0xF6 && r.raw[5] == 0xA7 && r.answer_ms < 1000 && r.frames >= 1);
    assert(r.rssi_dbm == -46 && wt_mode(wt_b4(&r)) == WT_MODE_SHIP);
    assert(sim.now - t0 < 2500 && sim.sent[FRAME_A] < 30 && !sim.sent[FRAME_E] && !sim.sent[FRAME_OTHER]);
    /* the LF stopped at the first frame; it then listened until the reply had ended */
    assert(sim.now - sim.last_frame_end >= WT_TAIL_QUIET_MS);
    assert(!sim.rx_wide && sim.progress_calls > 2);

    /* A working sensor answers at its listening moment: here in the third burst. */
    sim_init(&sim);
    sensor(&sim, 0x0F279DDC, 0x86, 31000, 26500, E_ACK_ONLY);
    t0 = sim.now;
    assert(wt_engine_scan(&eng, 0, NULL, 0, &r) == WT_R_OK);
    idle(&sim);
    assert(r.id == 0x0F279DDC && wt_b4(&r) == 0x86 && r.answer_ms >= 26500 && r.answer_ms < 27000);
    assert(sim.prepares == 3 && sim.sent[FRAME_A] > 2 * 280 && !sim.sent[FRAME_E]);

    /* Its listening moment falls in the 2 s between bursts: caught at the next one. */
    sim_init(&sim);
    sensor(&sim, 0x0F279DDC, 0x86, 31000, 10800, E_ACK_ONLY);
    assert(wt_engine_scan(&eng, 0, NULL, 0, &r) == WT_R_OK && r.answer_ms >= 41800 && r.answer_ms < 42300);
    idle(&sim);

    /* Nothing there: seven full bursts, then "no reply". */
    sim_init(&sim);
    t0 = sim.now;
    assert(wt_engine_scan(&eng, 0, NULL, 0, &r) == WT_R_NO_REPLY);
    idle(&sim);
    assert(sim.prepares == WT_SCAN_BURSTS && sim.now - t0 >= 83000 && sim.now - t0 < 86000);
    assert(sim.sent[FRAME_A] > 7 * 280 && sim.sent[FRAME_A] < 7 * 300 && !sim.sent[FRAME_E]);
    assert(r.id == 0);

    /* Back, mid-burst. */
    sim_init(&sim);
    sim.cancel_at = sim.now + 3333;
    t0 = sim.now;
    assert(wt_engine_scan(&eng, 0, NULL, 0, &r) == WT_R_CANCELLED);
    idle(&sim);
    assert(sim.now - t0 < 3400);

    /* Only the wanted sensor counts when one is named. */
    sim_init(&sim);
    sensor(&sim, 0x0F279DDC, 0x86, 31000, 5000, E_ACK_ONLY);
    assert(wt_engine_scan(&eng, 0x0F3A6CB8, NULL, 0, &r) == WT_R_NO_REPLY);
    idle(&sim);

    /* With none named, the first heard is the one; a second sensor does not replace it. */
    sim_init(&sim);
    sensor(&sim, 0x0F3A6CB8, 0xF6, 500, 400, E_WORKS);
    sim.foreign_at = sim.now + 700, sim.foreign_id = 0x0AAAAAAA;
    assert(wt_engine_scan(&eng, 0, NULL, 0, &r) == WT_R_OK && r.id == 0x0F3A6CB8 && wt_b4(&r) == 0xF6);
    sim_init(&sim);
    sensor(&sim, 0x0F3A6CB8, 0xF6, 500, 900, E_WORKS);
    sim.foreign_at = sim.now + 300, sim.foreign_id = 0x0AAAAAAA;
    assert(wt_engine_scan(&eng, 0, NULL, 0, &r) == WT_R_OK && r.id == 0x0AAAAAAA);
    idle(&sim);

    /* "Not it": two sensors in reach of the coil. Told to ignore the one that
     * answers first, the scan keeps the wake going through its reply and takes
     * the other one. */
    sim_init(&sim);
    sensor(&sim, 0x0FD4D1DA, 0x86, 31000, 400, E_ACK_ONLY); /* the neighbour: answers at once, every burst */
    sim.foreign_at = sim.now + 17300, sim.foreign_id = 0x0F36789E; /* the wheel wanted, in its second burst */
    uint32_t skip[3] = {0x0F279DDC, 0x0FD4D1DA, 0x0F2AD738};
    t0 = sim.now;
    assert(wt_engine_scan(&eng, 0, skip, 3, &r) == WT_R_OK && r.id == 0x0F36789E);
    idle(&sim);
    assert(sim.prepares == 2 && sim.now - t0 > 17300 && r.answer_ms >= 17300 && r.frames == 1);
    /* Without the list the neighbour is what a scan finds. */
    sim_init(&sim);
    sensor(&sim, 0x0FD4D1DA, 0x86, 31000, 400, E_ACK_ONLY);
    sim.foreign_at = sim.now + 17300, sim.foreign_id = 0x0F36789E;
    assert(wt_engine_scan(&eng, 0, NULL, 0, &r) == WT_R_OK && r.id == 0x0FD4D1DA);
    /* Only ignored sensors in reach: all seven bursts, then "no reply". */
    sim_init(&sim);
    sensor(&sim, 0x0FD4D1DA, 0x86, 31000, 400, E_ACK_ONLY);
    t0 = sim.now;
    assert(wt_engine_scan(&eng, 0, skip, 3, &r) == WT_R_NO_REPLY && r.id == 0);
    idle(&sim);
    assert(sim.prepares == WT_SCAN_BURSTS && sim.now - t0 >= 83000);
    /* The list is for one scan: the next one, given none, hears that sensor again; so does exit ship mode. */
    sim_init(&sim);
    sensor(&sim, 0x0FD4D1DA, 0x86, 31000, 400, E_ACK_ONLY);
    assert(wt_engine_scan(&eng, 0, skip, 3, &r) == WT_R_NO_REPLY);
    sim.s.next_listen = sim.now + 300;
    assert(wt_engine_scan(&eng, 0, NULL, 0, &r) == WT_R_OK && r.id == 0x0FD4D1DA && eng.nskip == 0);
    /* a list longer than the engine holds is cut, not overrun */
    uint32_t many[WT_SKIP_MAX + 6];
    for(unsigned i = 0; i < WT_SKIP_MAX + 6; i++) many[i] = 0x0A000000u + i;
    sim_init(&sim);
    sim.cancel_at = sim.now + 50;
    assert(wt_engine_scan(&eng, 0, many, WT_SKIP_MAX + 6, &r) == WT_R_CANCELLED && eng.nskip == WT_SKIP_MAX);

    /* Faults: the coil refuses, the radio refuses. Either way nothing is left on. */
    sim_init(&sim);
    sim.prepare_fail = true;
    assert(wt_engine_scan(&eng, 0, NULL, 0, &r) == WT_R_FAULT && !strcmp(eng.fault, "LF coil not available"));
    assert(!sim.running && !sim.rx_on && !sim.starts);
    sim_init(&sim);
    sim.rx_fail = true;
    assert(wt_engine_scan(&eng, 0, NULL, 0, &r) == WT_R_FAULT && !strcmp(eng.fault, "radio did not start"));
    assert(!sim.prepares && !sim.rx_on);
    printf("PASS: scan: ship-mode and working sensors, a moment missed, no sensor, Back, ignoring neighbours, faults\n");
}

static WtReading scan_first(Sim* sim) {
    WtReading r;
    assert(wt_engine_scan(&eng, 0, NULL, 0, &r) == WT_R_OK);
    idle(sim);
    return r;
}

static void test_ship(void) {
    Sim sim;
    WtShip s;

    /* The case at the wheel on 12 November: F6, the answer 75 then 76, then 86. */
    sim_init(&sim);
    sensor(&sim, 0x0F3A6CB8, 0xF6, 500, 400, E_WORKS);
    WtReading before = scan_first(&sim);
    int a_before = sim.sent[FRAME_A];
    sim.now += 4000; /* the user reads the screen and presses OK */
    sim.s.next_listen = sim.now + 200;
    sim.phases_seen = 0;
    uint32_t t0 = sim.now;
    assert(wt_engine_ship(&eng, &before, &s) == WT_R_OK);
    idle(&sim);
    assert(s.verdict == WT_V_DONE && s.have_ack && s.have_after);
    assert(wt_b4(&s.before) == 0xF6 && wt_b4(&s.ack) == 0x75 && s.ack_last_b4 == 0x76 && wt_b4(&s.after) == 0x86);
    assert(s.cmd_bursts == 1 && s.cmd_frames >= 1 && s.cmd_frames < 15 && s.cmd_frames == sim.sent[FRAME_E]);
    assert(s.ack.answer_ms < 1000 && s.ack.frames == 15);
    /* no command frame after the sensor's first answer */
    assert(sim.last_tx[FRAME_E] <= t0 + 400);
    /* the wake came back only after it had been quiet for 5 s: its answer lasted 5.9 s */
    assert(sim.sent[FRAME_A] > a_before);
    assert(s.after.answer_ms > 15000 && s.after.answer_ms < 25000); /* 32.1 s after the answer began */
    assert(sim.phases_seen == ((1u << WT_PH_SEND) | (1u << WT_PH_SETTLE) | (1u << WT_PH_CHECK)));
    assert(sim.now - t0 > 32000 && sim.now - t0 < 36000);

    /* A sensor that does not know the frame: six bursts unanswered, still in ship mode after. */
    sim_init(&sim);
    sensor(&sim, 0x0F3A6CB8, 0xF6, 500, 400, E_IGNORED);
    before = scan_first(&sim);
    sim.s.next_listen = sim.now + 200;
    t0 = sim.now;
    assert(wt_engine_ship(&eng, &before, &s) == WT_R_OK);
    idle(&sim);
    assert(s.verdict == WT_V_NO_CHANGE && !s.have_ack && s.have_after && wt_b4(&s.after) == 0xF6);
    assert(s.cmd_bursts == WT_CMD_BURSTS && s.cmd_frames > 6 * 280 && s.cmd_frames == sim.sent[FRAME_E]);

    /* It answers and then says nothing more: the app does not guess. */
    sim_init(&sim);
    sensor(&sim, 0x0F3A6CB8, 0xF6, 500, 400, E_SILENCES);
    before = scan_first(&sim);
    sim.s.next_listen = sim.now + 200;
    assert(wt_engine_ship(&eng, &before, &s) == WT_R_OK);
    idle(&sim);
    assert(s.verdict == WT_V_SILENT && s.have_ack && !s.have_after);

    /* A sensor that is already normal (the bench spare): 77 on a later burst, normal after. */
    sim_init(&sim);
    sensor(&sim, 0x0F61C9F5, 0x87, 31000, 3000, E_ACK_ONLY);
    before = scan_first(&sim);
    assert(wt_b4(&before) == 0x87);
    assert(wt_engine_ship(&eng, &before, &s) == WT_R_OK);
    idle(&sim);
    assert(s.have_ack && wt_b4(&s.ack) == 0x77 && s.cmd_bursts == 3 && s.have_after && wt_b4(&s.after) == 0x87);
    assert(s.verdict == WT_V_NO_CHANGE);

    /* Another sensor's frame during the command is not taken for the answer. */
    sim_init(&sim);
    sensor(&sim, 0x0F3A6CB8, 0xF6, 500, 400, E_IGNORED);
    before = scan_first(&sim);
    sim.s.present = false; /* ours has gone quiet */
    sim.foreign_at = sim.now + 2000, sim.foreign_id = 0x0AAAAAAA;
    assert(wt_engine_ship(&eng, &before, &s) == WT_R_OK);
    idle(&sim);
    assert(!s.have_ack && !s.have_after && s.verdict == WT_V_SILENT && s.cmd_bursts == WT_CMD_BURSTS);

    /* Back while sending; Back while checking. What was learned so far is kept. */
    sim_init(&sim);
    sensor(&sim, 0x0F3A6CB8, 0xF6, 500, 400, E_IGNORED);
    before = scan_first(&sim);
    sim.cancel_at = sim.now + 4000;
    a_before = sim.sent[FRAME_A];
    assert(wt_engine_ship(&eng, &before, &s) == WT_R_CANCELLED && s.verdict == WT_V_NONE);
    idle(&sim);
    assert(s.cmd_frames > 0 && sim.sent[FRAME_A] == a_before); /* no wake was sent after the Back */
    sim_init(&sim);
    sensor(&sim, 0x0F3A6CB8, 0xF6, 500, 400, E_WORKS);
    before = scan_first(&sim);
    sim.s.next_listen = sim.now + 200;
    sim.cancel_at = sim.now + 20000;
    assert(wt_engine_ship(&eng, &before, &s) == WT_R_CANCELLED && s.have_ack && !s.have_after && s.verdict == WT_V_NONE);
    idle(&sim);

    /* No sensor read first: nothing is sent. */
    sim_init(&sim);
    memset(&before, 0, sizeof(before));
    assert(wt_engine_ship(&eng, &before, &s) == WT_R_FAULT && !sim.prepares && !sim.rx_starts);

    /* The verdict table. */
    WtShip v = {0};
    v.before.raw[4] = 0xF6;
    assert(wt_verdict(&v) == WT_V_SILENT);
    v.have_after = true, v.after.raw[4] = 0x87;
    assert(wt_verdict(&v) == WT_V_DONE);
    v.after.raw[4] = 0xF7; /* the low digit moves with temperature: not a change */
    assert(wt_verdict(&v) == WT_V_NO_CHANGE);
    v.after.raw[4] = 0x77;
    assert(wt_verdict(&v) == WT_V_OTHER);
    v.before.raw[4] = 0x87, v.after.raw[4] = 0x86;
    assert(wt_verdict(&v) == WT_V_NO_CHANGE);
    v.before.raw[4] = 0x37, v.after.raw[4] = 0x86;
    assert(wt_verdict(&v) == WT_V_DONE);
    v.before.raw[4] = 0xF6, v.after.raw[4] = 0x66; /* heard driving: working */
    assert(wt_verdict(&v) == WT_V_DONE);
    v.before.raw[4] = 0x86, v.after.raw[4] = 0x66; /* park to drive: both working, nothing fixed */
    assert(wt_verdict(&v) == WT_V_NO_CHANGE);
    v.before.raw[4] = 0x86, v.after.raw[4] = 0xF6;
    assert(wt_verdict(&v) == WT_V_OTHER);
    printf("PASS: exit ship mode: changed, ignored, silent after, already normal, foreign frame, Back, verdicts\n");
}

static void test_monitor(void) {
    Sim sim;
    sim_init(&sim);
    uint32_t pins[WT_PINS] = {0x0F279DDC, 0x0F592011, 0x0F3A6CB8, 0x0F4EA3FA, 0x0F61C9F5}; /* a set and the spare tyre */
    wt_engine_pin(&eng, pins);
    /* frames arrive from inside the pump: drive it by hand */
    assert(p_rx_start(&sim, true));
    eng.prog.phase = WT_PH_MONITOR;
    for(int w = 0; w < 4; w++) {
        sim.now += 1000;
        emit(&sim, pins[w], 0x86, (uint8_t)(0xA7 + w), 0x34, -60 - w);
    }
    assert(eng.nsensors == 4 && eng.sensors[3].id == 0x0F4EA3FA && eng.sensors[3].raw[5] == 0xAA && eng.sensors[3].count == 1);
    sim.now += 1000;
    emit(&sim, pins[0], 0x86, 0xA8, 0x35, -58);
    assert(eng.nsensors == 4 && eng.sensors[0].count == 2 && eng.sensors[0].raw[5] == 0xA8 && eng.sensors[0].last_tick == sim.now);
    /* passing cars fill the rest of the table and then push each other out, never the four wheels */
    for(uint32_t k = 0; k < 20; k++) {
        sim.now += 500;
        emit(&sim, 0x0B000000u + k, 0x87, 0x90, 0x40, -80);
    }
    assert(eng.nsensors == WT_MAX_SENSORS);
    for(int w = 0; w < 4; w++) assert(eng.sensors[w].id == pins[w]);
    /* the spare tyre, heard late, takes a stranger's row and then keeps it */
    sim.now += 500;
    emit(&sim, pins[4], 0x87, 0xC8, 0x40, -75);
    for(uint32_t k = 20; k < 40; k++) {
        sim.now += 500;
        emit(&sim, 0x0B000000u + k, 0x87, 0x90, 0x40, -80);
    }
    bool tyre = false;
    for(uint8_t i = 0; i < eng.nsensors; i++) tyre |= eng.sensors[i].id == pins[4];
    assert(tyre);
    for(int w = 0; w < 4; w++) assert(eng.sensors[w].id == pins[w]);
    bool newest = false;
    for(uint8_t i = 4; i < eng.nsensors; i++) {
        assert(eng.sensors[i].id == pins[4] || eng.sensors[i].id >= 0x0B000024u); /* the three most recent strangers */
        if(eng.sensors[i].id == 0x0B000027u) newest = true;
    }
    assert(newest);
    p_rx_stop(&sim);
    eng.prog.phase = WT_PH_IDLE;

    /* the loop itself: receive only, until Back */
    sim_init(&sim);
    sim.cancel_at = sim.now + 5000;
    sim.foreign_at = sim.now + 1200, sim.foreign_id = 0x0F279DDC;
    assert(wt_engine_monitor(&eng) == WT_R_CANCELLED);
    idle(&sim);
    assert(sim.rx_wide && !sim.prepares && !sim.starts && eng.nsensors == 1 && eng.sensors[0].id == 0x0F279DDC);
    assert(sim.progress_calls >= 20);
    sim_init(&sim);
    sim.rx_fail = true;
    assert(wt_engine_monitor(&eng) == WT_R_FAULT);
    printf("PASS: monitor: receive only, latest frame per sensor, the four wheels are never pushed out of the table\n");
}

static void test_log(void) {
    char line[WT_LOG_LINE_MAX];
    WtReading r = {.id = 0x0F3A6CB8, .raw = {0x0F, 0x3A, 0x6C, 0xB8, 0x86, 0xA7, 0x33, 0x01, 0xF2}, .rssi_dbm = -45,
                   .frames = 3, .answer_ms = 680};
    assert(wt_log_scan(line, sizeof(line), "2026-11-12 23:40:01", WT_R_OK, &r, "S-RL", false));
    assert(!strcmp(line, "2026-11-12 23:40:01," WT_VERSION ",scan,ok,0F3A6CB8,86/01,,,2292,1,-45,680,3,,,,0F3A6CB886A73301F2,S-RL\n"));
    assert(wt_log_scan(line, sizeof(line), "2026-11-12 23:41:00", WT_R_NO_REPLY, &r, "", true));
    assert(!strcmp(line, "2026-11-12 23:41:00," WT_VERSION ",scan_new,no_reply,00000000,,,,,,,,,,,,,\n"));
    WtShip s = {.before = r, .ack = r, .after = r, .have_ack = true, .have_after = true, .cmd_bursts = 1, .cmd_frames = 5,
                .verdict = WT_V_DONE};
    s.before.raw[4] = 0xF6, s.ack.raw[4] = 0x75, s.after.answer_ms = 21140, s.after.frames = 1;
    assert(wt_log_ship(line, sizeof(line), "2026-11-12 23:42:00", WT_R_OK, &s, "TYRE"));
    assert(!strcmp(line, "2026-11-12 23:42:00," WT_VERSION ",ship,ok,0F3A6CB8,F6/01,75/01,86/01,2292,1,-45,21140,1,1,5,changed,"
                         "0F3A6CB886A73301F2,TYRE\n"));
    s.have_ack = s.have_after = false, s.verdict = WT_V_NONE, s.cmd_bursts = 2, s.cmd_frames = 310;
    assert(wt_log_ship(line, sizeof(line), "2026-11-12 23:43:00", WT_R_CANCELLED, &s, ""));
    assert(!strcmp(line, "2026-11-12 23:43:00," WT_VERSION ",ship,cancelled,0F3A6CB8,F6/01,,,,,,,,2,310,,,\n"));
    /* every line has the header's number of columns, and a buffer too small gives nothing */
    int cols = 0, want = 0;
    for(const char* c = WT_LOG_HEADER; *c; c++) want += *c == ',';
    for(const char* c = line; *c; c++) cols += *c == ',';
    assert(cols == want);
    wt_log_scan(line, sizeof(line), "t", WT_R_OK, &r, "SP1", true);
    cols = 0;
    for(const char* c = line; *c; c++) cols += *c == ',';
    assert(cols == want);
    assert(wt_log_ship(line, 40, "2026-11-12 23:43:00", WT_R_OK, &s, "") == 0);
    printf("PASS: log: one line per scan and per exit-ship-mode run, columns as the header names them\n");
}

static WtSensor link_sensor(uint32_t id, uint8_t b4, uint8_t b5, uint8_t b6, int8_t rssi, uint16_t count, uint32_t last) {
    WtSensor s = {.id = id, .rssi_dbm = rssi, .count = count, .last_tick = last};
    uint8_t raw[9] = {(uint8_t)(id >> 24), (uint8_t)(id >> 16), (uint8_t)(id >> 8), (uint8_t)id, b4, b5, b6, 0x01, 0};
    for(int i = 0; i < 8; i++) raw[8] ^= raw[i];
    memcpy(s.raw, raw, 9);
    return s;
}

static void test_link(void) {
    uint8_t r[WT_LINK_REC_LEN];
    /* the sensor record, byte by byte: 176 counts and 0x34 are what the car read as 35 psi and 2 C */
    WtSensor s = link_sensor(0x0F3A6CB8, 0x66, 176, 0x34, -61, 7, 1000);
    wt_link_sensor_rec(r, 0x2A, &s, wt_slot(0, WT_RL), true, false, 12345);
    uint8_t want[WT_LINK_REC_LEN] = {0x01, 0x2A, 0x0F, 0x3A, 0x6C, 0xB8, 0x66, 176, 0x34, 0x01, 0,
                                     0xC3, 2, WT_LINK_F_ON_CAR, 12, 0, 0x70, 0x09, 2, WT_MODE_DRIVE};
    for(int i = 2; i < 10; i++) want[10] ^= want[i]; /* the frame's own checksum is passed on */
    assert(!memcmp(r, want, sizeof(want)));
    assert((r[16] | r[17] << 8) == 2416 && wt_kpa_x10(176) == 2416);
    /* below zero, an age past 18 hours, no place, a repeat */
    s = link_sensor(0x0F61C9F5, 0xF6, 0x02, 0x10, -90, 1, 0);
    wt_link_sensor_rec(r, 0xFF, &s, WT_NO_SLOT, false, true, 100000000u);
    assert((int8_t)r[18] == -34 && r[12] == 0xFF && r[13] == WT_LINK_F_REPEAT && r[14] == 0xFF && r[15] == 0xFF &&
           r[19] == WT_MODE_SHIP && (int8_t)r[11] == -90);
    /* the hello */
    WtConfig cfg;
    wt_config_default(&cfg);
    cfg.on_car = 1, cfg.punit = WT_P_PSI, cfg.tunit = WT_T_F;
    wt_link_hello_rec(r, 3, &cfg, 5, 87, 1);
    unsigned vmaj = 0, vmin = 0, vpat = 0;
    assert(sscanf(WT_VERSION, "%u.%u.%u", &vmaj, &vmin, &vpat) == 3);
    const uint8_t hello[WT_LINK_REC_LEN] = {0x02, 3, 2, (uint8_t)vmaj, (uint8_t)vmin, (uint8_t)vpat, 1, WT_P_PSI, WT_T_F, 5, 87, 1};
    assert(!memcmp(r, hello, sizeof(hello)));

    /* what is due, and when */
    WtLinkTrack t;
    wt_link_track_init(&t);
    WtSensor tab[WT_MAX_SENSORS];
    uint8_t places[WT_MAX_SENSORS], out[WT_MAX_SENSORS + 1][WT_LINK_REC_LEN], rows[WT_MAX_SENSORS + 1];
    wt_config_default(&cfg); /* summer on the car */
    for(uint8_t i = 0; i < 4; i++) {
        tab[i] = link_sensor(0x0F000000u + i, 0x66, (uint8_t)(170 + i), 0x34, -60, 1, 1000);
        places[i] = wt_slot(i < 2 ? 0 : 1, i); /* two summer wheels, two winter */
    }
    tab[4] = link_sensor(0x0B000001, 0x87, 0x90, 0x40, -85, 1, 1500);
    places[4] = WT_NO_SLOT;
    uint8_t n = 5, cap = WT_MAX_SENSORS + 1;
    assert(wt_link_due(&t, false, 2000, tab, n, places, &cfg, out, cap, rows) == 0); /* no phone: nothing */
    uint8_t k = wt_link_due(&t, true, 2000, tab, n, places, &cfg, out, cap, rows);
    assert(k == 6 && out[0][0] == WT_LINK_HELLO && rows[0] == 0xFF && out[0][9] == 5); /* on connecting: a hello, then all */
    for(uint8_t i = 1; i < k; i++) {
        assert(out[i][0] == WT_LINK_SENSOR && rows[i] == i - 1 && out[i][1] == i && !(out[i][13] & WT_LINK_F_REPEAT));
        assert(!!(out[i][13] & WT_LINK_F_ON_CAR) == (i - 1 < 2)); /* the winter wheels are not on the car */
    }
    assert(out[1][14] == 1); /* heard a second ago */
    assert(wt_link_due(&t, true, 2250, tab, n, places, &cfg, out, cap, rows) == 0); /* nothing new */
    tab[2].count++, tab[2].last_tick = 2400, tab[2].raw[5] = 0xB0;
    k = wt_link_due(&t, true, 2500, tab, n, places, &cfg, out, cap, rows);
    assert(k == 1 && rows[0] == 2 && out[0][7] == 0xB0 && out[0][1] == 6 && out[0][14] == 0); /* the new frame only */
    /* the stack refused it: it goes again */
    wt_link_unsent(&t, rows[0]);
    k = wt_link_due(&t, true, 2750, tab, n, places, &cfg, out, cap, rows);
    assert(k == 1 && rows[0] == 2);
    /* a row handed to another sensor counts as new, even with the same count */
    tab[4] = link_sensor(0x0B000002, 0x87, 0x90, 0x40, -85, 1, 2900);
    k = wt_link_due(&t, true, 3000, tab, n, places, &cfg, out, cap, rows);
    assert(k == 1 && rows[0] == 4 && out[0][5] == 0x02);
    /* ten seconds after connecting: the round, everything again, marked as repeats */
    k = wt_link_due(&t, true, 12000, tab, n, places, &cfg, out, cap, rows);
    assert(k == 6 && out[0][0] == WT_LINK_HELLO);
    for(uint8_t i = 1; i < k; i++) assert(out[i][13] & WT_LINK_F_REPEAT);
    assert(out[1][14] == 11);
    /* a hello the stack refused goes again, alone */
    wt_link_unsent(&t, 0xFF);
    k = wt_link_due(&t, true, 12250, tab, n, places, &cfg, out, cap, rows);
    assert(k == 1 && out[0][0] == WT_LINK_HELLO);
    /* a small buffer: what does not fit is sent next time */
    assert(wt_link_due(&t, true, 12500, tab, n, places, &cfg, out, cap, rows) == 0);
    tab[0].count++, tab[1].count++, tab[3].count++;
    k = wt_link_due(&t, true, 12750, tab, n, places, &cfg, out, 2, rows);
    assert(k == 2 && rows[0] == 0 && rows[1] == 1);
    k = wt_link_due(&t, true, 13000, tab, n, places, &cfg, out, 2, rows);
    assert(k == 1 && rows[0] == 3);
    /* the phone goes away and comes back: a hello and everything, at once */
    assert(wt_link_due(&t, false, 14000, tab, n, places, &cfg, out, cap, rows) == 0);
    k = wt_link_due(&t, true, 15000, tab, n, places, &cfg, out, cap, rows);
    assert(k == 6 && out[0][0] == WT_LINK_HELLO);
    /* a round spread over calls of three: new frames first, the round's remainder after */
    k = wt_link_due(&t, true, 25000, tab, n, places, &cfg, out, 3, rows);
    assert(k == 3 && rows[0] == 0xFF && rows[1] == 0 && rows[2] == 1 && (out[1][13] & WT_LINK_F_REPEAT));
    tab[4].count++;
    k = wt_link_due(&t, true, 25250, tab, n, places, &cfg, out, 3, rows);
    assert(k == 3 && rows[0] == 4 && !(out[0][13] & WT_LINK_F_REPEAT) && rows[1] == 2 && rows[2] == 3);
    assert(wt_link_due(&t, true, 25500, tab, n, places, &cfg, out, 3, rows) == 0); /* row 4 was paid by its new frame */
    /* the sequence number runs on across records and wraps */
    t.seq = 0xFF;
    tab[0].count++;
    k = wt_link_due(&t, true, 25750, tab, n, places, &cfg, out, cap, rows);
    assert(k == 1 && out[0][1] == 0xFF && t.seq == 0);
    printf("PASS: phone link: record layout, a hello and every sensor on connecting, new frames at once, a round every 10 s, resend on refusal\n");
}

int main(void) {
    test_core();
    test_link();
    test_log();
    test_scan();
    test_ship();
    test_monitor();
    return 0;
}
