// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
/* TPMSFlipper — the phone link: what the Live monitor sends over Bluetooth
 * LE, and when. Header-only and SDK-free; the Bluetooth service itself is
 * wt_link_ble.c. The full description for phone-app writers is
 * docs/PHONE-LINK.md.
 *
 * One GATT service, one characteristic (read + notify), no pairing. Every
 * value is a 20-byte record, so one record fits the default ATT payload and
 * no phone has to negotiate a larger MTU. Little-endian.
 *
 *   SENSOR record, type 0x01: one sensor, as last heard
 *     0      type 0x01
 *     1      seq: +1 for every record sent; a gap means a record was lost
 *     2..10  the TG1C frame as received: ID[4] B4 B5 B6 B7 XOR
 *     11     signal, dBm, int8
 *     12     place: 0-3 Summer FL FR RL RR, 4-7 Winter FL FR RL RR, 8 spare
 *            tyre, 9-12 spare sensors 1-4, 0xFF none
 *     13     flags: bit 0 a wheel of the set on the car; bit 1 a repeat:
 *            this frame has been sent on the link before (the 10 s round)
 *     14..15 age: seconds since the frame was heard, uint16, saturating
 *     16..17 pressure, tenths of a kPa, gauge (B5 x 1.3725)
 *     18     temperature, degrees C, int8 (B6 - 50)
 *     19     mode: 0 unknown, 1 park, 2 drive, 3 ship, 4 busy (wt_core.h)
 *
 *   HELLO record, type 0x02: sent first on every connection and in every round
 *     0      type 0x02
 *     1      seq
 *     2      protocol version (WT_LINK_PROTO)
 *     3..5   app version major, minor, patch
 *     6      the set on the car: 0 Summer, 1 Winter
 *     7      pressure unit chosen on the Flipper: 0 bar, 1 psi, 2 kPa
 *     8      temperature unit: 0 C, 1 F
 *     9      sensors in the monitor's table now
 *     10     the Flipper's battery, percent (protocol 2)
 *     11     flags: bit 0 an operation is running (protocol 2)
 *     12..19 zero
 *
 * Protocol 2 adds a command characteristic and four more record types, for
 * a paired phone: wt_remote.h.
 *
 * Pressure and temperature are also in the raw frame; the decoded bytes are
 * there so a phone app needs no knowledge of the TG1C frame.
 */
#include "wt_engine.h"

#define WT_LINK_PROTO 2u
#define WT_LINK_REC_LEN 20u
#define WT_LINK_SENSOR 0x01u
#define WT_LINK_HELLO 0x02u
#define WT_LINK_F_ON_CAR 0x01u
#define WT_LINK_F_REPEAT 0x02u
#define WT_LINK_ROUND_MS 10000u /* every sensor again, and a hello */

/* What the screen shows about the link. */
typedef enum {
    WT_LINK_OFF = 0, /* the setting is off */
    WT_LINK_WAIT, /* advertising, no phone connected */
    WT_LINK_ON, /* a phone is connected */
    WT_LINK_FAIL, /* Bluetooth could not be started, or is off in the Flipper's settings */
} WtLinkState;

/* 128-bit UUIDs, as the Bluetooth stack wants them: least significant byte first.
 *   service         9D6A2D74-91CD-4828-A353-85A41CA80437
 *   characteristic  9D6A2D74-91CD-4828-A353-85A41CA80438  readings: read, notify
 *   characteristic  9D6A2D74-91CD-4828-A353-85A41CA80439  commands: write, paired only */
#define WT_LINK_UUID_BYTES(last) \
    {(last), 0x04, 0xA8, 0x1C, 0xA4, 0x85, 0x53, 0xA3, 0x28, 0x48, 0xCD, 0x91, 0x74, 0x2D, 0x6A, 0x9D}
#define WT_LINK_SVC_UUID WT_LINK_UUID_BYTES(0x37)
#define WT_LINK_CHR_UUID WT_LINK_UUID_BYTES(0x38)
#define WT_LINK_CMD_UUID WT_LINK_UUID_BYTES(0x39)

static inline void wt_link_u16(uint8_t* p, uint32_t v) {
    if(v > 0xFFFFu) v = 0xFFFFu;
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static inline void wt_link_sensor_rec(
    uint8_t out[WT_LINK_REC_LEN], uint8_t seq, const WtSensor* s, uint8_t place, bool on_car, bool repeat,
    uint32_t age_ms) {
    memset(out, 0, WT_LINK_REC_LEN);
    out[0] = WT_LINK_SENSOR;
    out[1] = seq;
    memcpy(out + 2, s->raw, WT_FRAME_LEN);
    out[11] = (uint8_t)s->rssi_dbm;
    out[12] = place;
    out[13] = (uint8_t)((on_car ? WT_LINK_F_ON_CAR : 0u) | (repeat ? WT_LINK_F_REPEAT : 0u));
    wt_link_u16(out + 14, age_ms / 1000u);
    wt_link_u16(out + 16, wt_kpa_x10(s->raw[5]));
    out[18] = (uint8_t)(int8_t)wt_temp(s->raw[6], WT_T_C);
    out[19] = (uint8_t)wt_mode(s->raw[4]);
}

static inline void wt_link_hello_rec(
    uint8_t out[WT_LINK_REC_LEN], uint8_t seq, const WtConfig* cfg, uint8_t nsensors, uint8_t battery, uint8_t flags) {
    memset(out, 0, WT_LINK_REC_LEN);
    out[0] = WT_LINK_HELLO;
    out[1] = seq;
    out[2] = WT_LINK_PROTO;
    uint8_t part = 3;
    for(const char* v = WT_VERSION; *v && part <= 5; v++) /* "0.3.0" */
        if(*v == '.')
            part++;
        else
            out[part] = (uint8_t)(out[part] * 10u + (uint8_t)(*v - '0'));
    out[6] = cfg->on_car;
    out[7] = cfg->punit;
    out[8] = cfg->tunit;
    out[9] = nsensors;
    out[10] = battery;
    out[11] = flags;
}

/* Which records are due. The table row a sensor sits in can be handed to
 * another sensor (wt_engine_on_frame), so a row counts as sent only for the
 * ID and the frame count it was sent with. */
typedef struct {
    bool connected;
    bool hello_due;
    uint8_t seq;
    uint8_t repeat_due; /* one bit per row: still to be sent in this round */
    uint8_t battery, flags; /* for the hello: set by the caller before each call */
    uint32_t round_at; /* tick the last round began */
    uint32_t sent_id[WT_MAX_SENSORS];
    uint16_t sent_count[WT_MAX_SENSORS];
} WtLinkTrack;

static inline void wt_link_track_init(WtLinkTrack* t) {
    memset(t, 0, sizeof(*t));
}

/* The phone's records for one call, at most cap of them: a hello when one is
 * due, then every row with a frame not sent yet, then the rows still owed in
 * this round. A round begins on connecting and every WT_LINK_ROUND_MS after;
 * what does not fit in one call goes in the next. Nothing is due while no
 * phone is connected. rows[] gets each record's table row, 0xFF for the
 * hello; hand back any the caller could not send with wt_link_unsent. */
static inline uint8_t wt_link_due(
    WtLinkTrack* t, bool connected, uint32_t now, const WtSensor* sensors, uint8_t n, const uint8_t* places,
    const WtConfig* cfg, uint8_t (*out)[WT_LINK_REC_LEN], uint8_t cap, uint8_t* rows) {
    if(!connected) {
        t->connected = false;
        return 0;
    }
    if(n > WT_MAX_SENSORS) n = WT_MAX_SENSORS;
    if(!t->connected || now - t->round_at >= WT_LINK_ROUND_MS) {
        t->round_at = now;
        t->hello_due = true;
        t->repeat_due = (uint8_t)((1u << n) - 1u);
    }
    t->connected = true;
    uint8_t k = 0;
    if(t->hello_due && k < cap) {
        t->hello_due = false;
        rows[k] = 0xFFu;
        wt_link_hello_rec(out[k++], t->seq++, cfg, n, t->battery, t->flags);
    }
    uint8_t on_car = cfg->on_car < WT_SETS ? cfg->on_car : 0;
    for(uint8_t pass = 0; pass < 2; pass++) { /* new frames first, then the round */
        for(uint8_t i = 0; i < n && k < cap; i++) {
            const WtSensor* s = &sensors[i];
            bool fresh = t->sent_id[i] != s->id || t->sent_count[i] != s->count;
            bool owed = (t->repeat_due >> i) & 1u;
            if(pass == 0 ? !fresh : (fresh || !owed)) continue;
            uint8_t place = places[i];
            bool car = place < WT_SLOT_TYRE && wt_slot_set(place) == on_car;
            rows[k] = i;
            wt_link_sensor_rec(out[k++], t->seq++, s, place, car, !fresh, now - s->last_tick);
            t->sent_id[i] = s->id;
            t->sent_count[i] = s->count;
            t->repeat_due &= (uint8_t)~(1u << i);
        }
    }
    return k;
}
/* The record for row could not be sent: it is due again. */
static inline void wt_link_unsent(WtLinkTrack* t, uint8_t row) {
    if(row == 0xFFu)
        t->hello_due = true;
    else if(row < WT_MAX_SENSORS)
        t->sent_id[row] = 0, t->sent_count[row] = 0;
}
