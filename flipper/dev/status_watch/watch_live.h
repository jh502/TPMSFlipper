// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "watch_state.h"
typedef struct {uint8_t old4,old7,b4,b7;uint32_t tick;} WatchTransition;
typedef struct {
    bool have;
    uint8_t b4,b7,delta4,delta7,history_count;
    uint32_t last,interval,transitions;
    WatchTransition history[3];
} WatchLive;
static inline bool watch_live_update(WatchLive* s,uint8_t b4,uint8_t b7,uint32_t tick) {
    bool change=s->have && (b4!=s->b4 || b7!=s->b7);
    if(s->have)s->interval=tick-s->last;
    if(change) {
        memmove(&s->history[1],&s->history[0],2*sizeof(s->history[0]));
        s->history[0]=(WatchTransition){s->b4,s->b7,b4,b7,tick};
        if(s->history_count<3)s->history_count++;
        s->delta4=s->b4^b4;s->delta7=s->b7^b7;s->transitions++;
    }
    s->have=true;s->b4=b4;s->b7=b7;s->last=tick;return change;
}
static inline const char* watch_live_reference(uint8_t b4,uint8_t b7) {
    if(watch_good(b4,b7))return "Good ref MATCH";
    if(b4==0xf7 && (b7==1||b7==2))return "Known Bad ref pair";
    return "Unclassified pair";
}
static inline const char* watch_live_status(uint8_t b7) {
    if(b7==1)return "01: common in captures";
    if(b7==2)return "02: brief in captures";
    return "Status not mapped";
}
