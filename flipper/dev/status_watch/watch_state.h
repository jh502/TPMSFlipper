// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#define WATCH_PAIRS 16
typedef struct {
    uint16_t pairs[WATCH_PAIRS];
    uint8_t count;
    bool armed, changed, overflow, good_reached;
    uint32_t good_tick;
    uint8_t trigger4, trigger7;
    uint32_t trigger_tick;
} WatchState;
static inline bool watch_contains(const WatchState* s,uint8_t b4,uint8_t b7) {
    uint16_t pair=((uint16_t)b4<<8)|b7;
    for(unsigned i=0;i<s->count;i++)if(s->pairs[i]==pair)return true;
    return false;
}
static inline uint8_t watch_observe(WatchState* s,uint8_t b4,uint8_t b7,uint32_t tick) {
    uint8_t event=0;
    if(s->armed && !s->good_reached && b4==0x87 && b7==1) {
        s->good_reached=true;s->good_tick=tick;event=2;
    }
    if(watch_contains(s,b4,b7))return event;
    if(!s->armed) {
        if(s->count<WATCH_PAIRS)s->pairs[s->count++]=((uint16_t)b4<<8)|b7;
        else s->overflow=true;
    } else if(!s->changed) {
        s->changed=true;s->trigger4=b4;s->trigger7=b7;s->trigger_tick=tick;event|=1;
    }
    return event;
}
static inline bool watch_arm(WatchState* s) {
    if(s->armed)return true;
    if(!s->count||s->overflow)return false;
    s->armed=true;s->good_reached=watch_contains(s,0x87,1);return true;
}

/* Known-Good 0F279DDC, user-confirmed 2026-09-29; run 0013. */
static inline bool watch_good(uint8_t b4,uint8_t b7) {return b4==0x87 && b7==1;}
