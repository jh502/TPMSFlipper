// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
/* TPMSFlipper — the three operations, on a port. Header-only and SDK-free:
 * the device app gives it the real receiver and LF coil, the host tests a
 * simulated clock, receiver and sensor (tests/tpmsflipper_engine.c).
 *
 *   scan     Wake the sensor at the valve and read it. The stock wake frame in
 *            10 s bursts 2 s apart, up to WT_SCAN_BURSTS of them, stopped at
 *            the first frame heard. A working sensor listens for LF only about
 *            every half minute, so this can take a minute. LF reaches every
 *            sensor near the coil, and the first to answer is the one read;
 *            a scan can be told which sensors to ignore, to get past the
 *            neighbours of the wheel that is wanted.
 *   ship     Take a sensor out of shipping mode. The exit-ship-mode frame in
 *            bursts, stopped at the sensor's first frame; then receive only
 *            until it has been quiet for 5 s; then the stock wake again until
 *            it answers. The verdict compares the mode before and after.
 *   monitor  Receive only. Every sensor heard, with its latest frame.
 *
 * LF timing is the Continental TG1C LF Lab's, unchanged, because that is what
 * was run against real sensors: half-bit 128 us, 2.048 ms carrier lead-in,
 * 8 ms between frames, 10 s bursts, 2 s between bursts (docs/TG1C-LF-LAB.md).
 * The frames come from the lab's tg1c_frame.h: A is the wake, E the frame a
 * workshop tool sends for "disable shipping mode".
 *
 * Nothing here writes to storage, and the carrier is off whenever a function
 * returns.
 */
#include "tg1c_frame.h"
#include "wt_core.h"

#define WT_HALFBIT_US 128u
#define WT_LEAD_US 2048u
#define WT_GAP_MS 8u
#define WT_BURST_MS 10000u
#define WT_LISTEN_MS 2000u
#define WT_FRAME_TIMEOUT_MS 100u /* a frame is 26 ms on air */
#define WT_SCAN_BURSTS 7u /* 84 s: two listening moments of a working sensor */
#define WT_CMD_BURSTS 6u
#define WT_CHECK_BURSTS 7u
#define WT_SETTLE_MS 5000u /* quiet time that ends the wait after the command */
#define WT_SETTLE_CAP_MS 60000u
#define WT_TAIL_QUIET_MS 600u /* a wake reply stops within 0.3 s of the LF stopping */
#define WT_TAIL_CAP_MS 3000u
#define WT_UI_PERIOD_MS 250u
#define WT_RSSI_PERIOD_MS 5u
#define WT_MAX_SENSORS 8u
#define WT_SKIP_MAX 24u /* sensors a scan can be told to ignore */

typedef enum {
    WT_PH_IDLE = 0,
    WT_PH_SCAN,
    WT_PH_SEND,
    WT_PH_SETTLE,
    WT_PH_CHECK,
    WT_PH_MONITOR,
} WtPhase;

typedef enum {
    WT_R_OK = 0,
    WT_R_NO_REPLY,
    WT_R_CANCELLED,
    WT_R_FAULT,
} WtResult;

typedef enum {
    WT_V_NONE = 0,
    WT_V_DONE, /* was not a working mode, is one now */
    WT_V_NO_CHANGE, /* same mode as before */
    WT_V_SILENT, /* no answer to the wake after the command */
    WT_V_OTHER, /* answered, in a mode that is neither */
} WtVerdict;

typedef struct {
    WtReading before, ack, after;
    bool have_ack, have_after;
    uint8_t ack_last_b4; /* status of the last frame of the acknowledgement */
    uint8_t cmd_bursts;
    uint16_t cmd_frames; /* exit-ship-mode frames sent */
    uint8_t verdict; /* WtVerdict */
} WtShip;

typedef struct {
    uint32_t id;
    uint8_t raw[WT_FRAME_LEN];
    int8_t rssi_dbm;
    uint16_t count;
    uint32_t last_tick;
} WtSensor;

typedef struct {
    uint8_t phase; /* WtPhase */
    uint8_t burst_no, burst_max;
    bool lf_on;
    bool heard; /* the sensor has answered in this phase */
    uint32_t elapsed_ms, limit_ms; /* of this phase */
    int rssi_dbm; /* receiver level now */
} WtProgress;

struct WtEngine;
typedef struct {
    void* ctx;
    uint32_t (*tick_ms)(void* ctx);
    void (*delay_tick)(void* ctx); /* sleep about 1 ms */
    bool (*cancelled)(void* ctx);
    /* Receiver on 433.92 MHz FSK. rx_pump hands frames to wt_engine_on_frame.
     * wide: the larger interval buffer, for receive-only use. */
    bool (*rx_start)(void* ctx, bool wide);
    void (*rx_pump)(void* ctx, uint32_t timeout_ms);
    int (*rx_rssi)(void* ctx);
    void (*rx_stop)(void* ctx);
    /* LF engine: one prepared frame, sent once per start. */
    bool (*lf_prepare)(void* ctx, const uint8_t* slots, size_t nslots, uint16_t half_us, uint16_t lead_us);
    bool (*lf_start)(void* ctx);
    bool (*lf_done)(void* ctx);
    bool (*lf_error)(void* ctx);
    void (*lf_stop)(void* ctx);
    void (*lf_free)(void* ctx);
    /* Called every WT_UI_PERIOD_MS (four times a second) while an operation runs. */
    void (*progress)(void* ctx, struct WtEngine* e);
} WtPort;

typedef struct WtEngine {
    const WtPort* port;
    uint8_t slots[TG1C_SLOTS_A];
    const char* fault;
    WtProgress prog;
    uint32_t phase_start, shown, rssi_at;
    int rssi_peak; /* strongest level sampled in this phase */
    uint32_t tx_frames; /* LF frames sent in this operation */

    /* The sensor being worked on. want_id 0: the first one heard that is not
     * in the skip list. */
    uint32_t want_id;
    uint32_t skip[WT_SKIP_MAX];
    uint8_t nskip;
    WtReading first, cur; /* its first and its latest frame of this phase */
    uint32_t first_tick, last_tick;

    /* Monitor. */
    WtSensor sensors[WT_MAX_SENSORS];
    uint8_t nsensors;
    uint32_t pinned[WT_PINS]; /* never dropped from the table */
} WtEngine;

static inline void wt_engine_init(WtEngine* e, const WtPort* port) {
    memset(e, 0, sizeof(*e));
    e->port = port;
}
static inline uint32_t wt_now(WtEngine* e) {
    return e->port->tick_ms(e->port->ctx);
}
static inline bool wt_cancelled(WtEngine* e) {
    return e->port->cancelled(e->port->ctx);
}
static inline void wt_engine_pin(WtEngine* e, const uint32_t ids[WT_PINS]) {
    memcpy(e->pinned, ids, sizeof(e->pinned));
}
static inline bool wt_is_pinned(const WtEngine* e, uint32_t id) {
    for(unsigned w = 0; w < WT_PINS; w++)
        if(id && e->pinned[w] == id) return true;
    return false;
}

/* A checksum-valid frame, from inside rx_pump. */
static inline void wt_engine_on_frame(WtEngine* e, uint32_t id, const uint8_t raw[WT_FRAME_LEN], int rssi_dbm) {
    uint32_t now = wt_now(e);
    int8_t rssi = (int8_t)(rssi_dbm < -128 ? -128 : rssi_dbm > 0 ? 0 : rssi_dbm);
    if(e->prog.phase == WT_PH_MONITOR) {
        WtSensor* s = NULL;
        for(uint8_t i = 0; i < e->nsensors; i++)
            if(e->sensors[i].id == id) s = &e->sensors[i];
        if(!s && e->nsensors < WT_MAX_SENSORS) {
            s = &e->sensors[e->nsensors++];
            memset(s, 0, sizeof(*s));
        }
        if(!s) { /* full: the one not heard for longest that has no wheel */
            for(uint8_t i = 0; i < e->nsensors; i++) {
                WtSensor* c = &e->sensors[i];
                if(wt_is_pinned(e, c->id)) continue;
                if(!s || now - c->last_tick > now - s->last_tick) s = c;
            }
            if(!s) return;
            memset(s, 0, sizeof(*s));
        }
        s->id = id;
        memcpy(s->raw, raw, WT_FRAME_LEN);
        s->rssi_dbm = rssi;
        if(s->count < UINT16_MAX) s->count++;
        s->last_tick = now;
        return;
    }
    if(e->prog.phase == WT_PH_IDLE) return;
    if(!e->want_id) {
        for(uint8_t i = 0; i < e->nskip; i++)
            if(e->skip[i] == id) return; /* a neighbour the user has ruled out */
        e->want_id = id;
    }
    if(id != e->want_id) return;
    e->cur.id = id;
    memcpy(e->cur.raw, raw, WT_FRAME_LEN);
    if(!e->prog.heard || rssi > e->cur.rssi_dbm) e->cur.rssi_dbm = rssi;
    if(!e->prog.heard) {
        e->prog.heard = true;
        e->cur.frames = 0;
        e->first_tick = now;
        e->first = e->cur;
    }
    if(e->cur.frames < UINT16_MAX) e->cur.frames++;
    e->last_tick = now;
}

/* One step of every wait: let the receiver deliver, sleep 1 ms, keep the
 * signal level and the screen up to date. */
static inline void wt_step(WtEngine* e, uint32_t pump_ms) {
    const WtPort* port = e->port;
    port->rx_pump(port->ctx, pump_ms);
    if(!pump_ms) port->delay_tick(port->ctx);
    uint32_t now = wt_now(e);
    if(e->prog.phase != WT_PH_MONITOR && now - e->rssi_at >= WT_RSSI_PERIOD_MS) {
        e->rssi_at = now;
        e->prog.rssi_dbm = port->rx_rssi(port->ctx);
        if(e->prog.rssi_dbm > e->rssi_peak) e->rssi_peak = e->prog.rssi_dbm;
    }
    if(now - e->shown >= WT_UI_PERIOD_MS) {
        e->shown = now;
        e->prog.elapsed_ms = now - e->phase_start;
        port->progress(port->ctx, e);
    }
}
static inline void wt_phase(WtEngine* e, WtPhase phase, uint8_t burst_max, uint32_t limit_ms) {
    memset(&e->prog, 0, sizeof(e->prog));
    e->prog.phase = (uint8_t)phase;
    e->prog.burst_max = burst_max;
    e->prog.limit_ms = limit_ms;
    e->prog.rssi_dbm = -127;
    e->rssi_peak = -127;
    e->phase_start = wt_now(e);
    e->shown = e->phase_start - WT_UI_PERIOD_MS; /* show it at once */
    memset(&e->cur, 0, sizeof(e->cur));
    memset(&e->first, 0, sizeof(e->first));
}
static inline bool wt_stop(WtEngine* e) {
    return e->prog.heard || e->fault || wt_cancelled(e);
}

/* One burst of one frame, cut at the sensor's first frame. Returns with the
 * carrier off and the envelope freed, whatever ended it. */
static inline void wt_burst(WtEngine* e, TG1cPayload payload) {
    const WtPort* port = e->port;
    size_t nslots = tg1c_build_frame(payload, e->slots, sizeof(e->slots));
    if(nslots != TG1C_SLOTS_A) {
        e->fault = "frame build failed";
        return;
    }
    if(!port->lf_prepare(port->ctx, e->slots, nslots, WT_HALFBIT_US, WT_LEAD_US)) {
        e->fault = "LF coil not available";
        port->lf_free(port->ctx);
        return;
    }
    uint32_t start = wt_now(e);
    e->prog.lf_on = true;
    while(!wt_stop(e) && wt_now(e) - start < WT_BURST_MS) {
        if(!port->lf_start(port->ctx)) {
            e->fault = "LF coil did not start";
            break;
        }
        uint32_t frame_start = wt_now(e);
        while(!port->lf_done(port->ctx) && !port->lf_error(port->ctx) && !wt_stop(e) &&
              wt_now(e) - frame_start < WT_FRAME_TIMEOUT_MS)
            wt_step(e, 0);
        bool complete = port->lf_done(port->ctx) && !port->lf_error(port->ctx);
        port->lf_stop(port->ctx); /* carrier off now, whatever ended the wait */
        if(!complete) {
            if(!wt_stop(e)) e->fault = "LF coil interrupted";
            break;
        }
        e->tx_frames++;
        uint32_t gap = wt_now(e);
        do {
            wt_step(e, 0);
        } while(!wt_stop(e) && wt_now(e) - gap < WT_GAP_MS);
    }
    port->lf_stop(port->ctx);
    port->lf_free(port->ctx);
    e->prog.lf_on = false;
}
/* Receive only for ms, or until the sensor answers. */
static inline void wt_listen(WtEngine* e, uint32_t ms) {
    uint32_t start = wt_now(e);
    while(!wt_stop(e) && wt_now(e) - start < ms) wt_step(e, 0);
}
/* Receive only until quiet_ms pass with no frame from the sensor, cap_ms at most. */
static inline void wt_quiet(WtEngine* e, uint32_t quiet_ms, uint32_t cap_ms) {
    uint32_t start = wt_now(e);
    while(!e->fault && !wt_cancelled(e)) {
        uint32_t now = wt_now(e);
        uint32_t since = e->prog.heard && now - e->last_tick < now - start ? now - e->last_tick : now - start;
        if(since >= quiet_ms || now - start >= cap_ms) break;
        wt_step(e, 0);
    }
}
/* Bursts of one frame until the sensor answers. */
static inline void wt_train(WtEngine* e, TG1cPayload payload, uint8_t bursts) {
    for(uint8_t b = 1; b <= bursts && !wt_stop(e); b++) {
        e->prog.burst_no = b;
        wt_burst(e, payload);
        if(wt_stop(e)) break;
        wt_listen(e, WT_LISTEN_MS);
    }
}
static inline void wt_take(WtEngine* e, WtReading* out, const WtReading* frame, uint32_t op_start) {
    *out = *frame;
    out->frames = e->cur.frames;
    out->rssi_dbm = (int8_t)(e->rssi_peak > e->cur.rssi_dbm ? e->rssi_peak : e->cur.rssi_dbm);
    out->answer_ms = e->first_tick - op_start;
}
static inline WtResult wt_finish(WtEngine* e, bool ok) {
    const WtPort* port = e->port;
    port->lf_stop(port->ctx);
    port->lf_free(port->ctx);
    port->rx_stop(port->ctx);
    e->prog.phase = WT_PH_IDLE;
    e->prog.lf_on = false;
    return e->fault ? WT_R_FAULT : wt_cancelled(e) ? WT_R_CANCELLED : ok ? WT_R_OK : WT_R_NO_REPLY;
}

/* Wake and read the sensor at the coil. want_id 0 takes the first sensor
 * heard that is not one of the nskip IDs in skip; otherwise only want_id
 * counts. A skipped sensor's answer does not stop the wake, so the scan goes
 * on until another sensor answers or the bursts run out. */
static inline WtResult wt_engine_scan(
    WtEngine* e, uint32_t want_id, const uint32_t* skip, uint8_t nskip, WtReading* out) {
    const WtPort* port = e->port;
    e->fault = NULL;
    e->tx_frames = 0;
    e->want_id = want_id;
    e->nskip = nskip > WT_SKIP_MAX ? WT_SKIP_MAX : nskip;
    if(e->nskip) memcpy(e->skip, skip, e->nskip * sizeof(uint32_t));
    memset(out, 0, sizeof(*out));
    if(!port->rx_start(port->ctx, false)) {
        e->fault = "radio did not start";
        return wt_finish(e, false);
    }
    uint32_t start = wt_now(e);
    wt_phase(e, WT_PH_SCAN, WT_SCAN_BURSTS, WT_SCAN_BURSTS * (WT_BURST_MS + WT_LISTEN_MS));
    wt_train(e, TG1C_PAYLOAD_A, WT_SCAN_BURSTS);
    bool heard = e->prog.heard;
    if(heard) {
        wt_quiet(e, WT_TAIL_QUIET_MS, WT_TAIL_CAP_MS); /* its last frames, and the reply ended */
        wt_take(e, out, &e->cur, start);
    }
    return wt_finish(e, heard);
}

static inline WtVerdict wt_verdict(const WtShip* s) {
    if(!s->have_after) return WT_V_SILENT;
    WtMode was = wt_mode(wt_b4(&s->before)), is = wt_mode(wt_b4(&s->after));
    if(wt_mode_working(is) && !wt_mode_working(was)) return WT_V_DONE;
    if(is == was || (wt_mode_working(is) && wt_mode_working(was))) return WT_V_NO_CHANGE;
    return WT_V_OTHER;
}

/* Send the exit-ship-mode frame to the sensor read in `before`, then read it
 * again. WT_R_OK means the sequence ran to a verdict, whatever the verdict. */
static inline WtResult wt_engine_ship(WtEngine* e, const WtReading* before, WtShip* out) {
    const WtPort* port = e->port;
    e->fault = NULL;
    e->tx_frames = 0;
    e->want_id = before->id;
    e->nskip = 0;
    memset(out, 0, sizeof(*out));
    out->before = *before;
    if(!before->id) {
        e->fault = "no sensor to send to";
        return WT_R_FAULT;
    }
    if(!port->rx_start(port->ctx, false)) {
        e->fault = "radio did not start";
        return wt_finish(e, false);
    }
    uint32_t start = wt_now(e);
    wt_phase(e, WT_PH_SEND, WT_CMD_BURSTS, WT_CMD_BURSTS * (WT_BURST_MS + WT_LISTEN_MS));
    for(uint8_t b = 1; b <= WT_CMD_BURSTS && !wt_stop(e); b++) {
        e->prog.burst_no = out->cmd_bursts = b;
        wt_burst(e, TG1C_PAYLOAD_E);
        out->cmd_frames = (uint16_t)e->tx_frames;
        if(wt_stop(e)) break;
        wt_listen(e, WT_LISTEN_MS);
    }
    if(e->prog.heard) {
        /* Its answer goes on with the LF off: wait until it has finished. */
        e->prog.phase = WT_PH_SETTLE;
        e->prog.limit_ms = WT_SETTLE_CAP_MS;
        e->prog.burst_no = e->prog.burst_max = 0;
        e->phase_start = wt_now(e);
        wt_quiet(e, WT_SETTLE_MS, WT_SETTLE_CAP_MS);
        out->have_ack = true;
        wt_take(e, &out->ack, &e->first, start);
        out->ack_last_b4 = wt_b4(&e->cur);
    }
    if(!e->fault && !wt_cancelled(e)) {
        uint32_t check = wt_now(e);
        wt_phase(e, WT_PH_CHECK, WT_CHECK_BURSTS, WT_CHECK_BURSTS * (WT_BURST_MS + WT_LISTEN_MS));
        wt_train(e, TG1C_PAYLOAD_A, WT_CHECK_BURSTS);
        if(e->prog.heard) {
            wt_quiet(e, WT_TAIL_QUIET_MS, WT_TAIL_CAP_MS);
            out->have_after = true;
            wt_take(e, &out->after, &e->cur, check);
        }
    }
    bool ran = !e->fault && !wt_cancelled(e);
    if(ran) out->verdict = (uint8_t)wt_verdict(out);
    return wt_finish(e, true);
}

/* Receive until cancelled. The table is e->sensors. */
static inline WtResult wt_engine_monitor(WtEngine* e) {
    const WtPort* port = e->port;
    e->fault = NULL;
    e->nsensors = 0;
    if(!port->rx_start(port->ctx, true)) {
        e->fault = "radio did not start";
        return wt_finish(e, false);
    }
    wt_phase(e, WT_PH_MONITOR, 0, 0);
    while(!wt_cancelled(e)) wt_step(e, 10);
    return wt_finish(e, true);
}
