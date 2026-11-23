// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
/* TPMSFlipper — what a telemetry frame means, in the user's units, and the
 * small settings file. Header-only and SDK-free: the host tests compile it
 * with a plain C compiler.
 *
 * A Continental/VDO TG1C frame is nine bytes:
 *   ID[0..3]  B4 status  B5 pressure  B6 temperature  B7  B8 XOR checksum
 * Pressure is B5 x 1.3725 kPa, gauge (the car's own readings are whole
 * multiples of 1.3725; a loose sensor on a bench reads about 0), and
 * temperature is B6 - 50 C.
 *
 * WHAT THE STATUS BYTE MEANS IS AN OBSERVATION, NOT A SPECIFICATION. The
 * reference is docs/TG1C-STATUS-CODES.md: one car (Jaguar XF X260) and one
 * loose spare, September to November 2026. By the high digit of B4:
 *   8  a working sensor at rest answering a wake (87 warm, 86 cold)
 *   6  a frame the sensor sends on its own while the wheel turns (66)
 *   F  a sensor in shipping mode answering a wake (F7, F6)
 *   7  the block a sensor sends in answer to the exit-ship-mode command
 *      (77, 76, 75)
 * The low digit is not the state: it follows temperature. The names Park,
 * Drive and Ship are this project's and the owner's, not the maker's.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "wt_build.h" /* WT_VERSION, WT_PHONE_LINK */
#define WT_FRAME_LEN 9

/* One sensor as last heard. */
typedef struct {
    uint32_t id;
    uint8_t raw[WT_FRAME_LEN];
    int8_t rssi_dbm;
    uint16_t frames; /* frames heard from it in this operation */
    uint32_t answer_ms; /* from the first LF frame of the operation to its first frame */
} WtReading;

static inline uint8_t wt_b4(const WtReading* r) {
    return r->raw[4];
}
static inline uint8_t wt_b7(const WtReading* r) {
    return r->raw[7];
}

/* ----- mode, from the status byte ------------------------------------------- */
typedef enum {
    WT_MODE_UNKNOWN = 0,
    WT_MODE_PARK, /* 8x: working, at rest, answering a wake */
    WT_MODE_DRIVE, /* 6x: working, sent on its own while the wheel turns */
    WT_MODE_SHIP, /* Fx: shipping mode, answering a wake */
    WT_MODE_BUSY, /* 7x: answering the exit-ship-mode command */
} WtMode;

static inline WtMode wt_mode(uint8_t b4) {
    switch(b4 >> 4) {
    case 0x8u:
        return WT_MODE_PARK;
    case 0x6u:
        return WT_MODE_DRIVE;
    case 0xFu:
        return WT_MODE_SHIP;
    case 0x7u:
        return WT_MODE_BUSY;
    default:
        return WT_MODE_UNKNOWN;
    }
}
/* Park and Drive are the two states of a sensor the car can use. */
static inline bool wt_mode_working(WtMode m) {
    return m == WT_MODE_PARK || m == WT_MODE_DRIVE;
}
/* The mode in words; for a status this app does not know, the byte itself. */
static inline void wt_mode_label(char* out, size_t cap, uint8_t b4) {
    WtMode m = wt_mode(b4);
    if(m == WT_MODE_UNKNOWN)
        snprintf(out, cap, "STATUS %02X", b4);
    else
        snprintf(
            out, cap, "%s",
            m == WT_MODE_PARK ? "PARK MODE" : m == WT_MODE_DRIVE ? "DRIVE" : m == WT_MODE_SHIP ? "SHIP MODE" : "BUSY");
}
/* One word, for the monitor. */
static inline const char* wt_mode_short(WtMode m) {
    return m == WT_MODE_PARK ? "park" : m == WT_MODE_DRIVE ? "drive" : m == WT_MODE_SHIP ? "SHIP" : m == WT_MODE_BUSY ? "busy" : "?";
}

/* Battery flag, as two workshop-tool rules read a wake reply: bit 7 set, and
 * bit 4 set or the high digit 8. OK when both hold; a candidate reading. The
 * rules do not apply to a frame the sensor sent on its own (bit 7 clear in
 * every one seen): for those there is no battery reading. */
typedef enum { WT_BATT_OK = 0, WT_BATT_UNSURE, WT_BATT_NA } WtBatt;
static inline WtBatt wt_batt(uint8_t b4) {
    if(!(b4 & 0x80u)) return WT_BATT_NA;
    return (b4 & 0x10u) != 0 || (b4 & 0xF0u) == 0x80u ? WT_BATT_OK : WT_BATT_UNSURE;
}
static inline const char* wt_batt_name(WtBatt b) {
    return b == WT_BATT_OK ? "OK" : b == WT_BATT_UNSURE ? "?" : "-";
}

/* ----- units ------------------------------------------------------------------ */
typedef enum { WT_P_BAR = 0, WT_P_PSI, WT_P_KPA, WT_P_COUNT } WtPUnit;
typedef enum { WT_T_C = 0, WT_T_F, WT_T_COUNT } WtTUnit;

static inline const char* wt_punit_name(uint8_t u) {
    return u == WT_P_PSI ? "psi" : u == WT_P_KPA ? "kPa" : "bar";
}
static inline const char* wt_tunit_name(uint8_t u) {
    return u == WT_T_F ? "F" : "C";
}
/* Pressure in hundredths and in tenths of a kPa: B5 x 1.3725. */
static inline uint32_t wt_kpa_x100(uint8_t b5) {
    return ((uint32_t)b5 * 13725u + 50u) / 100u;
}
static inline uint32_t wt_kpa_x10(uint8_t b5) {
    return ((uint32_t)b5 * 13725u + 500u) / 1000u;
}
/* The number alone: "2.30" (bar), "33.4" (psi), "230" (kPa). */
static inline void wt_fmt_pressure(char* out, size_t cap, uint8_t b5, uint8_t unit) {
    uint32_t pa100 = wt_kpa_x100(b5); /* hundredths of a kPa */
    if(unit == WT_P_KPA) {
        snprintf(out, cap, "%lu", (unsigned long)((pa100 + 50u) / 100u));
    } else if(unit == WT_P_PSI) {
        uint32_t psi10 = (pa100 * 14504u + 500000u) / 1000000u; /* 1 kPa = 0.145038 psi */
        snprintf(out, cap, "%lu.%lu", (unsigned long)(psi10 / 10u), (unsigned long)(psi10 % 10u));
    } else {
        uint32_t cbar = (pa100 + 50u) / 100u; /* 1 kPa = 0.01 bar */
        snprintf(out, cap, "%lu.%02lu", (unsigned long)(cbar / 100u), (unsigned long)(cbar % 100u));
    }
}
static inline int wt_temp(uint8_t b6, uint8_t unit) {
    int c = (int)b6 - 50;
    if(unit != WT_T_F) return c;
    int f10 = c * 18 + 320; /* tenths of a degree F */
    return (f10 >= 0 ? f10 + 5 : f10 - 5) / 10;
}

/* ----- the sensor's radio band ---------------------------------------------------
 * The same sensor family is fitted with a 433.92 MHz transmitter for Europe
 * and a 315 MHz one for North America. Only what the app listens on changes;
 * the 125 kHz it sends is the same. 315 MHz is untested: no such sensor has
 * been near this app. */
typedef enum { WT_BAND_433 = 0, WT_BAND_315, WT_BAND_COUNT } WtBand;
static inline uint32_t wt_band_hz(uint8_t band) {
    return band == WT_BAND_315 ? 315000000u : 433920000u;
}
/* On the screen and in the settings file's "band=" line: the first three characters. */
static inline const char* wt_band_name(uint8_t band) {
    return band == WT_BAND_315 ? "315 MHz" : "433 MHz";
}

/* Signal bars, 0 to 4. At a valve the reply reads about -45 dBm; from the
 * driver's seat, wheels read -55 to -75. */
static inline uint8_t wt_bars(int rssi_dbm) {
    return rssi_dbm >= -50 ? 4 : rssi_dbm >= -62 ? 3 : rssi_dbm >= -74 ? 2 : rssi_dbm >= -88 ? 1 : 0;
}

/* ----- places: two sets of wheels, the spare tyre, loose spare sensors ---------
 * A sensor can be given one place. The places are the four wheels of the
 * Summer set and of the Winter set, the spare tyre, and four loose spare
 * sensors. One set is "on the car": that is the set the live monitor draws. */
typedef enum { WT_FL = 0, WT_FR, WT_RL, WT_RR, WT_WHEELS } WtWheel;
#define WT_NO_WHEEL 0xFFu
#define WT_SETS 2u
#define WT_SPARES 4u
#define WT_SLOT_TYRE (WT_SETS * WT_WHEELS)
#define WT_SLOT_SPARE0 (WT_SLOT_TYRE + 1u)
#define WT_SLOTS (WT_SLOT_SPARE0 + WT_SPARES)
#define WT_NO_SLOT 0xFFu
#define WT_PINS (WT_WHEELS + 1u) /* the set on the car and the spare tyre */

static inline const char* wt_wheel_name(uint8_t w) {
    static const char* const n[WT_WHEELS] = {"FL", "FR", "RL", "RR"};
    return w < WT_WHEELS ? n[w] : "--";
}
static inline const char* wt_wheel_long(uint8_t w) {
    static const char* const n[WT_WHEELS] = {"Front left", "Front right", "Rear left", "Rear right"};
    return w < WT_WHEELS ? n[w] : "Not set";
}
static inline const char* wt_set_name(uint8_t set) {
    return set == 1 ? "Winter" : "Summer";
}
static inline uint8_t wt_slot(uint8_t set, uint8_t wheel) {
    return (uint8_t)(set * WT_WHEELS + wheel);
}
static inline bool wt_slot_is_wheel(uint8_t slot) {
    return slot < WT_SLOT_TYRE;
}
static inline uint8_t wt_slot_set(uint8_t slot) {
    return (uint8_t)(slot / WT_WHEELS);
}
static inline uint8_t wt_slot_wheel(uint8_t slot) {
    return (uint8_t)(slot % WT_WHEELS);
}
/* Four characters at most: "S-RL", "W-FL", "TYRE", "SP1"; "--" for no place. */
static inline void wt_slot_short(char* out, size_t cap, uint8_t slot) {
    if(slot < WT_SLOT_TYRE)
        snprintf(out, cap, "%c-%s", wt_set_name(wt_slot_set(slot))[0], wt_wheel_name(wt_slot_wheel(slot)));
    else if(slot == WT_SLOT_TYRE)
        snprintf(out, cap, "TYRE");
    else if(slot < WT_SLOTS)
        snprintf(out, cap, "SP%u", (unsigned)(slot - WT_SLOT_SPARE0 + 1u));
    else
        snprintf(out, cap, "--");
}
/* "Summer RL", "Spare tyre", "Spare sensor 2". */
static inline void wt_slot_long(char* out, size_t cap, uint8_t slot) {
    if(slot < WT_SLOT_TYRE)
        snprintf(out, cap, "%s %s", wt_set_name(wt_slot_set(slot)), wt_wheel_name(wt_slot_wheel(slot)));
    else if(slot == WT_SLOT_TYRE)
        snprintf(out, cap, "Spare tyre");
    else if(slot < WT_SLOTS)
        snprintf(out, cap, "Spare sensor %u", (unsigned)(slot - WT_SLOT_SPARE0 + 1u));
    else
        snprintf(out, cap, "No place");
}
/* The key in the settings file: "summer.RL", "spare_tyre", "spare2". */
static inline void wt_slot_key(char* out, size_t cap, uint8_t slot) {
    if(slot < WT_SLOT_TYRE)
        snprintf(out, cap, "%s.%s", wt_slot_set(slot) == 1 ? "winter" : "summer", wt_wheel_name(wt_slot_wheel(slot)));
    else if(slot == WT_SLOT_TYRE)
        snprintf(out, cap, "spare_tyre");
    else
        snprintf(out, cap, "spare%u", (unsigned)(slot - WT_SLOT_SPARE0 + 1u));
}

/* A calendar date; year 0 means none. */
typedef struct {
    uint16_t year;
    uint8_t month, day;
} WtDate;
/* "13 Nov", or nothing for no date. */
static inline void wt_date_text(char* out, size_t cap, WtDate d) {
    static const char* const m[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    if(!d.year || d.month < 1 || d.month > 12)
        out[0] = 0;
    else
        snprintf(out, cap, "%u %s", d.day, m[d.month - 1]);
}

/* A place: the sensor in it, and what that sensor last told this Flipper. */
typedef struct {
    uint32_t id; /* 0 = empty */
    bool seen; /* b4..b6 and date hold a reading */
    uint8_t b4, b5, b6;
    WtDate date;
} WtSlot;

/* Offer to give a sensor a place after a scan. */
typedef enum { WT_ASK_NEW = 0, WT_ASK_ALWAYS, WT_ASK_NEVER, WT_ASK_COUNT } WtAsk;
static inline const char* wt_ask_name(uint8_t a) {
    return a == WT_ASK_ALWAYS ? "always" : a == WT_ASK_NEVER ? "never" : "new";
}

typedef struct {
    uint8_t punit; /* WtPUnit */
    uint8_t tunit; /* WtTUnit */
    uint8_t on_car; /* the set on the car */
    uint8_t ask; /* WtAsk */
    bool phone; /* the Live monitor sends its readings to a phone over Bluetooth */
    uint8_t band; /* WtBand: the radio band the app listens on */
    WtSlot slot[WT_SLOTS];
} WtConfig;

static inline void wt_config_default(WtConfig* c) {
    memset(c, 0, sizeof(*c));
}
static inline uint8_t wt_config_slot_of(const WtConfig* c, uint32_t id) {
    for(uint8_t i = 0; i < WT_SLOTS; i++)
        if(id && c->slot[i].id == id) return i;
    return WT_NO_SLOT;
}
/* A sensor has one place: giving it a new one empties the old, and what it
 * last told us goes with it. Whatever was in the new place loses its place. */
static inline void wt_config_assign(WtConfig* c, uint8_t slot, uint32_t id) {
    if(slot >= WT_SLOTS || !id) return;
    WtSlot moved = {.id = id};
    uint8_t old = wt_config_slot_of(c, id);
    if(old != WT_NO_SLOT) {
        moved = c->slot[old];
        memset(&c->slot[old], 0, sizeof(WtSlot));
    }
    c->slot[slot] = moved;
}
static inline void wt_config_remove(WtConfig* c, uint32_t id) {
    uint8_t old = wt_config_slot_of(c, id);
    if(old != WT_NO_SLOT) memset(&c->slot[old], 0, sizeof(WtSlot));
}
/* Keep a reading against the sensor's place. False if it has none. */
static inline bool wt_config_note(WtConfig* c, uint32_t id, uint8_t b4, uint8_t b5, uint8_t b6, WtDate date) {
    uint8_t i = wt_config_slot_of(c, id);
    if(i == WT_NO_SLOT) return false;
    c->slot[i].seen = true;
    c->slot[i].b4 = b4, c->slot[i].b5 = b5, c->slot[i].b6 = b6;
    c->slot[i].date = date;
    return true;
}
static inline uint8_t wt_config_set_count(const WtConfig* c, uint8_t set) {
    uint8_t n = 0;
    for(uint8_t w = 0; w < WT_WHEELS; w++) n += c->slot[wt_slot(set, w)].id != 0;
    return n;
}
static inline uint8_t wt_config_spare_count(const WtConfig* c) {
    uint8_t n = 0;
    for(uint8_t i = 0; i < WT_SPARES; i++) n += c->slot[WT_SLOT_SPARE0 + i].id != 0;
    return n;
}
/* The sensors the monitor must never drop: the set on the car and the spare tyre. */
static inline void wt_config_car_ids(const WtConfig* c, uint32_t ids[WT_PINS]) {
    for(uint8_t w = 0; w < WT_WHEELS; w++) ids[w] = c->slot[wt_slot(c->on_car < WT_SETS ? c->on_car : 0, w)].id;
    ids[WT_WHEELS] = c->slot[WT_SLOT_TYRE].id;
}

/* ----- the settings file --------------------------------------------------------
 * "key=value" lines. Unknown keys and bad values are skipped, so a file from
 * another version, or one edited by hand, still loads. A place line is the
 * sensor ID, then optionally its last status, pressure and temperature bytes
 * and the date they were read:
 *     summer.RL=0F3A6CB8 86 A7 33 2026-11-13
 * Empty places are left out. A v0.1 file's FL=, FR=, RL=, RR= lines load as
 * the Summer set. */
#define WT_CONFIG_TEXT_MAX 640
static inline size_t wt_config_write(const WtConfig* c, char* out, size_t cap) {
#if WT_PHONE_LINK
    int n = snprintf(
        out, cap, "# " WT_APP_NAME " %s\npressure=%s\ntemperature=%s\non_car=%s\nask=%s\nphone=%s\nband=%.3s\n",
        WT_VERSION, wt_punit_name(c->punit), wt_tunit_name(c->tunit), c->on_car == 1 ? "winter" : "summer",
        wt_ask_name(c->ask), c->phone ? "on" : "off", wt_band_name(c->band));
#else /* the public build has no phone link: no "phone=" line */
    int n = snprintf(
        out, cap, "# " WT_APP_NAME " %s\npressure=%s\ntemperature=%s\non_car=%s\nask=%s\nband=%.3s\n", WT_VERSION,
        wt_punit_name(c->punit), wt_tunit_name(c->tunit), c->on_car == 1 ? "winter" : "summer", wt_ask_name(c->ask),
        wt_band_name(c->band));
#endif
    if(n <= 0 || (size_t)n >= cap) return 0;
    size_t len = (size_t)n;
    for(uint8_t i = 0; i < WT_SLOTS; i++) {
        const WtSlot* s = &c->slot[i];
        char key[12];
        if(!s->id) continue;
        wt_slot_key(key, sizeof(key), i);
        if(s->seen && s->date.year)
            n = snprintf(
                out + len, cap - len, "%s=%08lX %02X %02X %02X %04u-%02u-%02u\n", key, (unsigned long)s->id, s->b4, s->b5,
                s->b6, s->date.year, s->date.month, s->date.day);
        else if(s->seen)
            n = snprintf(out + len, cap - len, "%s=%08lX %02X %02X %02X\n", key, (unsigned long)s->id, s->b4, s->b5, s->b6);
        else
            n = snprintf(out + len, cap - len, "%s=%08lX\n", key, (unsigned long)s->id);
        if(n <= 0 || (size_t)n >= cap - len) return 0;
        len += (size_t)n;
    }
    return len;
}
static inline bool wt_hex32(const char* s, size_t len, uint32_t* out) {
    if(!len || len > 8) return false;
    uint32_t v = 0;
    for(size_t i = 0; i < len; i++) {
        char ch = s[i];
        uint32_t d = ch >= '0' && ch <= '9' ? (uint32_t)(ch - '0') :
                     ch >= 'A' && ch <= 'F' ? (uint32_t)(ch - 'A' + 10) :
                     ch >= 'a' && ch <= 'f' ? (uint32_t)(ch - 'a' + 10) :
                                              16u;
        if(d > 15u) return false;
        v = (v << 4) | d;
    }
    *out = v;
    return true;
}
static inline bool wt_dec(const char* s, size_t len, uint32_t* out) {
    uint32_t v = 0;
    if(!len) return false;
    for(size_t i = 0; i < len; i++) {
        if(s[i] < '0' || s[i] > '9') return false;
        v = v * 10u + (uint32_t)(s[i] - '0');
    }
    *out = v;
    return true;
}
/* "0F3A6CB8", "0F3A6CB8 86 A7 33" or "0F3A6CB8 86 A7 33 2026-11-13". A line
 * that goes wrong part way keeps what was good before that point. */
static inline bool wt_slot_parse(WtSlot* s, const char* v, size_t n) {
    size_t p = 0, start;
    uint32_t id, b[3], y, m, d;
    memset(s, 0, sizeof(*s));
    while(p < n && v[p] != ' ') p++;
    if(!wt_hex32(v, p, &id) || !id) return false;
    s->id = id;
    for(int k = 0; k < 3; k++) {
        while(p < n && v[p] == ' ') p++;
        start = p;
        while(p < n && v[p] != ' ') p++;
        if(p - start != 2 || !wt_hex32(v + start, 2, &b[k])) return true;
    }
    s->seen = true;
    s->b4 = (uint8_t)b[0], s->b5 = (uint8_t)b[1], s->b6 = (uint8_t)b[2];
    while(p < n && v[p] == ' ') p++;
    if(n - p == 10 && v[p + 4] == '-' && v[p + 7] == '-' && wt_dec(v + p, 4, &y) && wt_dec(v + p + 5, 2, &m) &&
       wt_dec(v + p + 8, 2, &d) && y >= 2000 && m >= 1 && m <= 12 && d >= 1 && d <= 31)
        s->date = (WtDate){(uint16_t)y, (uint8_t)m, (uint8_t)d};
    return true;
}
static inline bool wt_key_is(const char* line, size_t eq, const char* key) {
    return eq == strlen(key) && !memcmp(line, key, eq);
}
static inline void wt_config_parse(WtConfig* c, const char* text, size_t len) {
    wt_config_default(c);
    size_t i = 0;
    while(i < len) {
        size_t start = i;
        while(i < len && text[i] != '\n') i++;
        size_t end = i;
        if(i < len) i++;
        while(end > start && (text[end - 1] == '\r' || text[end - 1] == ' ')) end--;
        const char* line = text + start;
        size_t n = end - start, eq = 0;
        while(eq < n && line[eq] != '=') eq++;
        if(eq == 0 || eq >= n || line[0] == '#') continue;
        const char* val = line + eq + 1;
        size_t vn = n - eq - 1;
        if(wt_key_is(line, eq, "pressure")) {
            for(uint8_t u = 0; u < WT_P_COUNT; u++)
                if(vn == strlen(wt_punit_name(u)) && !memcmp(val, wt_punit_name(u), vn)) c->punit = u;
        } else if(wt_key_is(line, eq, "temperature")) {
            for(uint8_t u = 0; u < WT_T_COUNT; u++)
                if(vn == 1 && val[0] == wt_tunit_name(u)[0]) c->tunit = u;
        } else if(wt_key_is(line, eq, "on_car")) {
            if(vn == 6 && !memcmp(val, "winter", 6)) c->on_car = 1;
            if(vn == 6 && !memcmp(val, "summer", 6)) c->on_car = 0;
        } else if(wt_key_is(line, eq, "ask")) {
            for(uint8_t a = 0; a < WT_ASK_COUNT; a++)
                if(vn == strlen(wt_ask_name(a)) && !memcmp(val, wt_ask_name(a), vn)) c->ask = a;
        } else if(wt_key_is(line, eq, "phone")) {
            if(WT_PHONE_LINK && vn == 2 && !memcmp(val, "on", 2)) c->phone = true;
            if(vn == 3 && !memcmp(val, "off", 3)) c->phone = false;
        } else if(wt_key_is(line, eq, "band")) {
            for(uint8_t b = 0; b < WT_BAND_COUNT; b++)
                if(vn == 3 && !memcmp(val, wt_band_name(b), 3)) c->band = b;
        } else {
            WtSlot got;
            uint8_t slot = WT_NO_SLOT;
            char key[12];
            for(uint8_t k = 0; k < WT_SLOTS; k++) {
                wt_slot_key(key, sizeof(key), k);
                if(wt_key_is(line, eq, key)) slot = k;
            }
            for(uint8_t w = 0; w < WT_WHEELS; w++) /* a v0.1 file: the one set it knew */
                if(wt_key_is(line, eq, wt_wheel_name(w))) slot = wt_slot(0, w);
            if(slot != WT_NO_SLOT && wt_slot_parse(&got, val, vn)) {
                wt_config_remove(c, got.id); /* a sensor listed twice keeps its last place */
                c->slot[slot] = got;
            }
        }
    }
}
