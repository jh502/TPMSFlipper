// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
/* TPMSFlipper — commands from a paired phone, and the records that tell
 * the phone what the Flipper is doing. Header-only and SDK-free; the host
 * tests drive it (tests/tpmsflipper_engine.c, tests/tpmsflipper_ui.c).
 *
 * A command does what the same keys would do on the Flipper: it moves the
 * screen model and may return a WtCmd for the app to carry out, exactly as
 * wt_ui_input does. So the Flipper's screen always shows what the phone
 * started, and Back on the Flipper still cancels it. The format is
 * docs/PHONE-LINK.md (protocol 2).
 *
 * WHAT A PHONE CAN START. A scan (the stock wake frame, as "Scan wheel"
 * sends), the receive-only monitor, and changes to the places and the set on
 * the car. Exit ship mode is answered "unsupported" in this version.
 */
#include "wt_ui.h"

#define WT_LINK_PROTO_REMOTE 2u

/* Command types, byte 0 of a write. Byte 1 is the phone's tag, echoed in the ack. */
enum {
    WT_RC_SCAN = 0x81,
    WT_RC_SCAN_OTHER = 0x82, /* "Not it": ignore the sensor just read and every placed one */
    WT_RC_CANCEL = 0x83, /* stop what runs, back to the home screen */
    WT_RC_MONITOR = 0x84,
    WT_RC_PLACE = 0x85, /* [2..5] ID big-endian, [6] slot or 0xFF for none */
    WT_RC_ON_CAR = 0x86, /* [2] 0 Summer, 1 Winter */
    WT_RC_WHEELS = 0x87, /* send the place list */
    WT_RC_SHIP = 0x88, /* exit ship mode: a later version */
};

typedef enum { WT_ACK_OK = 0, WT_ACK_BUSY, WT_ACK_BAD, WT_ACK_UNSUPPORTED } WtAck;

/* Record types sent to the phone, beyond hello (0x02) and sensor (0x01). */
#define WT_LINK_ACK 0x03u
#define WT_LINK_STATUS 0x04u
#define WT_LINK_SCAN 0x05u
#define WT_LINK_PLACE 0x06u

static inline uint32_t wt_rc_id(const uint8_t* p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

/* An operation is running: a scan, an exit-ship-mode run or the monitor. */
static inline bool wt_rc_running(const WtUi* ui) {
    return ui->screen == WT_SCR_SCAN_RUN || ui->screen == WT_SCR_SHIP_RUN || ui->screen == WT_SCR_MONITOR ||
           ui->screen == WT_SCR_MONITOR_LIST || wt_ui_busy(ui);
}

/* Carry out one command on the screen model. *ack gets the answer; the
 * return value is what the app must start or stop, as from wt_ui_input. */
static inline WtCmd wt_remote(WtUi* ui, const uint8_t c[WT_LINK_REC_LEN], uint8_t* ack) {
    *ack = WT_ACK_OK;
    switch(c[0]) {
    case WT_RC_SCAN:
        if(wt_rc_running(ui)) break;
        ui->ship_flow = false;
        ui->scan_new = false;
        ui->nrejected = 0;
        return wt_ui_begin(ui, WT_SCR_SCAN_RUN, WT_CMD_SCAN);
    case WT_RC_SCAN_OTHER:
        if(wt_rc_running(ui)) break;
        if(!ui->reading.id) {
            *ack = WT_ACK_BAD; /* nothing has been read to ignore */
            return WT_CMD_NONE;
        }
        if(ui->nrejected == WT_REJECT_MAX) {
            memmove(ui->rejected, ui->rejected + 1, (WT_REJECT_MAX - 1u) * sizeof(uint32_t));
            ui->nrejected--;
        }
        if(!ui->nrejected || ui->rejected[ui->nrejected - 1u] != ui->reading.id)
            ui->rejected[ui->nrejected++] = ui->reading.id;
        ui->ship_flow = false;
        ui->scan_new = true;
        return wt_ui_begin(ui, WT_SCR_SCAN_RUN, WT_CMD_SCAN);
    case WT_RC_CANCEL:
        if(ui->screen == WT_SCR_SCAN_RUN || ui->screen == WT_SCR_SHIP_RUN || ui->screen == WT_SCR_MONITOR ||
           ui->screen == WT_SCR_MONITOR_LIST || wt_ui_busy(ui)) {
            bool monitor = ui->screen == WT_SCR_MONITOR || ui->screen == WT_SCR_MONITOR_LIST || wt_ui_busy(ui);
            ui->screen = WT_SCR_HOME;
            ui->op_seq++; /* whatever the worker reports now is stale */
            if(monitor && ui->mon_touched) ui->dirty = true;
            ui->mon_touched = false;
            return WT_CMD_CANCEL;
        }
        ui->screen = WT_SCR_HOME;
        return WT_CMD_NONE;
    case WT_RC_MONITOR:
        if(wt_rc_running(ui)) break;
        ui->nsensors = 0;
        ui->list_sel = 0;
        return wt_ui_begin(ui, WT_SCR_MONITOR, WT_CMD_MONITOR);
    case WT_RC_PLACE: {
        uint32_t id = wt_rc_id(c + 2);
        uint8_t slot = c[6];
        if(!id || (slot >= WT_SLOTS && slot != WT_NO_SLOT)) {
            *ack = WT_ACK_BAD;
            return WT_CMD_NONE;
        }
        if(slot == WT_NO_SLOT)
            wt_config_remove(&ui->cfg, id);
        else
            wt_config_assign(&ui->cfg, slot, id);
        ui->dirty = true;
        /* A "Where is it?" open on the Flipper for this sensor is answered. */
        if((ui->screen == WT_SCR_WHERE || ui->screen == WT_SCR_ASSIGN || ui->screen == WT_SCR_SPARE_PICK) &&
           ui->assign_id == id)
            ui->screen = ui->return_to;
        return WT_CMD_NONE;
    }
    case WT_RC_ON_CAR:
        if(c[2] >= WT_SETS) {
            *ack = WT_ACK_BAD;
            return WT_CMD_NONE;
        }
        ui->cfg.on_car = c[2];
        ui->dirty = true;
        return WT_CMD_NONE;
    case WT_RC_WHEELS:
        return WT_CMD_NONE; /* the app sends the list */
    case WT_RC_SHIP:
        *ack = WT_ACK_UNSUPPORTED;
        return WT_CMD_NONE;
    default:
        *ack = WT_ACK_UNSUPPORTED;
        return WT_CMD_NONE;
    }
    *ack = WT_ACK_BUSY;
    return WT_CMD_NONE;
}

/* ----- records to the phone ------------------------------------------------- */

static inline void wt_rec_ack(uint8_t out[WT_LINK_REC_LEN], uint8_t seq, const uint8_t c[WT_LINK_REC_LEN], uint8_t ack) {
    memset(out, 0, WT_LINK_REC_LEN);
    out[0] = WT_LINK_ACK;
    out[1] = seq;
    out[2] = c[1];
    out[3] = c[0];
    out[4] = ack;
}

/* What the Flipper is doing now. Equal records mean nothing changed. */
static inline void wt_rec_status(uint8_t out[WT_LINK_REC_LEN], uint8_t seq, const WtUi* ui, uint8_t battery_pct) {
    memset(out, 0, WT_LINK_REC_LEN);
    out[0] = WT_LINK_STATUS;
    out[1] = seq;
    out[2] = ui->screen;
    out[3] = ui->prog.phase;
    out[4] = ui->prog.burst_no;
    out[5] = ui->prog.burst_max;
    out[6] = (uint8_t)((ui->prog.lf_on ? 1u : 0u) | (ui->prog.heard ? 2u : 0u) | (ui->scan_new ? 4u : 0u));
    wt_link_u16(out + 7, ui->prog.elapsed_ms / 1000u);
    wt_link_u16(out + 9, ui->prog.limit_ms / 1000u);
    out[11] = (uint8_t)(int8_t)(ui->prog.rssi_dbm < -128 ? -128 : ui->prog.rssi_dbm > 0 ? 0 : ui->prog.rssi_dbm);
    out[12] = battery_pct;
    uint32_t skip[WT_SKIP_MAX];
    out[13] = ui->scan_new ? wt_ui_skip_list(ui, skip) : 0; /* sensors a "Not it" scan ignores */
}

/* A scan's outcome: result is WtResult. The reading is ui->reading. */
static inline void wt_rec_scan(uint8_t out[WT_LINK_REC_LEN], uint8_t seq, const WtUi* ui, uint8_t result) {
    const WtReading* r = &ui->reading;
    memset(out, 0, WT_LINK_REC_LEN);
    out[0] = WT_LINK_SCAN;
    out[1] = seq;
    out[2] = result;
    if(result == WT_R_OK) {
        memcpy(out + 3, r->raw, WT_FRAME_LEN);
        out[12] = (uint8_t)r->rssi_dbm;
        out[13] = wt_config_slot_of(&ui->cfg, r->id);
        wt_link_u16(out + 14, r->answer_ms / 100u);
        out[16] = r->frames > 255 ? 255 : (uint8_t)r->frames;
    } else {
        out[13] = WT_NO_SLOT;
    }
    out[17] = ui->scan_new ? 1 : 0;
}

/* One place of the thirteen, with what its sensor last said. */
static inline void wt_rec_place(uint8_t out[WT_LINK_REC_LEN], uint8_t seq, const WtConfig* cfg, uint8_t slot) {
    const WtSlot* s = &cfg->slot[slot];
    memset(out, 0, WT_LINK_REC_LEN);
    out[0] = WT_LINK_PLACE;
    out[1] = seq;
    out[2] = slot;
    out[3] = WT_SLOTS;
    out[4] = (uint8_t)(s->id >> 24), out[5] = (uint8_t)(s->id >> 16), out[6] = (uint8_t)(s->id >> 8), out[7] = (uint8_t)s->id;
    out[8] = s->seen ? 1 : 0;
    out[9] = s->b4, out[10] = s->b5, out[11] = s->b6;
    out[12] = s->date.year >= 2000 ? (uint8_t)(s->date.year - 2000u) : 0;
    out[13] = s->date.month;
    out[14] = s->date.day;
    out[15] = cfg->on_car;
}
