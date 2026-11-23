// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
/* TPMSFlipper — the lines of log.csv: one per scan and one per
 * exit-ship-mode run, written after the receiver has stopped. Header-only and
 * SDK-free. The file is the record of what was sent and what came back, for
 * anyone who wants to check a result later. */
#include "wt_engine.h"

#define WT_LOG_LINE_MAX 200
#define WT_LOG_HEADER \
    "time,app,op,result,id,before,ack,after,kpa_x10,temp_c,rssi_dbm,answer_ms,frames,cmd_bursts,cmd_frames,verdict,raw,place\n"

static inline const char* wt_result_name(uint8_t r) {
    return r == WT_R_OK ? "ok" : r == WT_R_NO_REPLY ? "no_reply" : r == WT_R_CANCELLED ? "cancelled" : "fault";
}
static inline const char* wt_verdict_name(uint8_t v) {
    return v == WT_V_DONE ? "changed" : v == WT_V_NO_CHANGE ? "no_change" : v == WT_V_SILENT ? "silent" :
           v == WT_V_OTHER ? "other" : "";
}
static inline void wt_log_raw(char out[2 * WT_FRAME_LEN + 1], const WtReading* r) {
    for(int i = 0; i < WT_FRAME_LEN; i++) snprintf(out + 2 * i, 3, "%02X", r->raw[i]);
}
static inline void wt_log_pair(char* out, size_t cap, bool have, const WtReading* r) {
    if(have)
        snprintf(out, cap, "%02X/%02X", wt_b4(r), wt_b7(r));
    else
        out[0] = 0;
}
/* The measured part of a line, from the reading that carries the values. */
static inline int wt_log_tail(char* out, size_t cap, bool have, const WtReading* r) {
    char raw[2 * WT_FRAME_LEN + 1];
    if(!have) return snprintf(out, cap, ",,,,");
    wt_log_raw(raw, r);
    return snprintf(
        out,
        cap,
        "%lu,%d,%d,%lu,%u",
        (unsigned long)wt_kpa_x10(r->raw[5]),
        wt_temp(r->raw[6], WT_T_C),
        r->rssi_dbm,
        (unsigned long)r->answer_ms,
        r->frames);
}
/* place: the sensor's place when the line is written ("S-RL", "TYRE"), or "".
 * fresh: the scan ignored the sensors that already have a place ("scan_new"). */
static inline size_t wt_log_scan(
    char* out, size_t cap, const char* when, uint8_t result, const WtReading* r, const char* place, bool fresh) {
    char pair[8], tail[64], raw[2 * WT_FRAME_LEN + 1] = "";
    bool have = result == WT_R_OK;
    wt_log_pair(pair, sizeof(pair), have, r);
    wt_log_tail(tail, sizeof(tail), have, r);
    if(have) wt_log_raw(raw, r);
    int n = snprintf(
        out, cap, "%s,%s,%s,%s,%08lX,%s,,,%s,,,,%s,%s\n", when, WT_VERSION, fresh ? "scan_new" : "scan",
        wt_result_name(result), (unsigned long)(have ? r->id : 0), pair, tail, raw, place);
    return n > 0 && (size_t)n < cap ? (size_t)n : 0;
}
static inline size_t wt_log_ship(
    char* out, size_t cap, const char* when, uint8_t result, const WtShip* s, const char* place) {
    char before[8], ack[8], after[8], tail[64], raw[2 * WT_FRAME_LEN + 1] = "";
    wt_log_pair(before, sizeof(before), true, &s->before);
    wt_log_pair(ack, sizeof(ack), s->have_ack, &s->ack);
    wt_log_pair(after, sizeof(after), s->have_after, &s->after);
    wt_log_tail(tail, sizeof(tail), s->have_after, &s->after);
    if(s->have_after) wt_log_raw(raw, &s->after);
    int n = snprintf(
        out, cap, "%s,%s,ship,%s,%08lX,%s,%s,%s,%s,%u,%u,%s,%s,%s\n", when, WT_VERSION, wt_result_name(result),
        (unsigned long)s->before.id, before, ack, after, tail, s->cmd_bursts, s->cmd_frames,
        wt_verdict_name(s->verdict), raw, place);
    return n > 0 && (size_t)n < cap ? (size_t)n : 0;
}
