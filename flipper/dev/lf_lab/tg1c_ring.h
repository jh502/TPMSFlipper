// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
/* Continental TG1C LF Lab — raw-interval evidence ring (plan v3 §3, §4c).
 *
 * Header-only, SDK-free. The caller owns the storage (256 records = 4 KiB).
 * The ring overwrites its oldest record until the first sync or sync-like hit;
 * it then takes 192 more records and freezes, leaving 64 records up to the hit
 * and 192 after it. A frozen ring ignores later records and later hits, so one
 * session yields at most one raw file. With no hit there is nothing to save.
 */
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* Same layout as TpmsCaptureRecord (tpms_session.h) and a TPRAW02 record. */
typedef struct {
    uint32_t tick, sequence, rx_lost;
    int32_t pulse;
} TG1cRawRecord;

#define TG1C_RING_RECORDS 256u
#define TG1C_RING_POST 192u
/* The reply ring (v0.2.4) is armed when a reply has already passed its CRC, so
 * the whole reply is behind the trigger: keep nearly all of the ring as
 * history and only a short tail after it. */
#define TG1C_RING_REPLY_POST 32u

typedef struct {
    TG1cRawRecord* rec;
    uint16_t cap;
    uint16_t head; /* next write position */
    uint16_t count;
    uint16_t post_left;
    bool triggered, frozen;
    uint8_t variant; /* TG1cBtldrVariant of the hit that armed the capture */
    bool sync_like; /* the hit was a bit-reversed sync form */
    bool crc_valid; /* a CRC-valid reply followed the hit */
    uint32_t rx_hz; /* receive frequency at the hit, for the file stamp */
} TG1cRing;

static inline void tg1c_ring_init(TG1cRing* r, TG1cRawRecord* storage, uint16_t cap) {
    r->rec = storage;
    r->cap = cap;
    r->head = r->count = r->post_left = 0;
    r->triggered = r->frozen = r->sync_like = r->crc_valid = false;
    r->variant = 0;
    r->rx_hz = 0;
}

static inline void tg1c_ring_push(TG1cRing* r, const TG1cRawRecord* rec) {
    if(r->frozen || !r->rec || !r->cap) return;
    r->rec[r->head] = *rec;
    r->head = (uint16_t)((r->head + 1) % r->cap);
    if(r->count < r->cap) r->count++;
    if(r->triggered && r->post_left && --r->post_left == 0) r->frozen = true;
}

/* Arm the capture on the first hit, keeping `post` further records. Returns
 * false if a capture is already armed or frozen. */
static inline bool tg1c_ring_trigger_post(TG1cRing* r, uint8_t variant, bool sync_like, uint16_t post) {
    if(r->triggered || !r->rec) return false;
    r->triggered = true;
    r->variant = variant;
    r->sync_like = sync_like;
    r->post_left = post;
    if(!post) r->frozen = true;
    return true;
}
static inline bool tg1c_ring_trigger(TG1cRing* r, uint8_t variant, bool sync_like) {
    return tg1c_ring_trigger_post(r, variant, sync_like, (uint16_t)TG1C_RING_POST);
}

static inline void tg1c_ring_mark_crc_valid(TG1cRing* r) {
    if(r->triggered) r->crc_valid = true;
}

static inline bool tg1c_ring_triggered(const TG1cRing* r) {
    return r->triggered;
}
static inline bool tg1c_ring_frozen(const TG1cRing* r) {
    return r->frozen;
}
static inline uint16_t tg1c_ring_count(const TG1cRing* r) {
    return r->count;
}
/* i-th record, oldest first. */
static inline const TG1cRawRecord* tg1c_ring_at(const TG1cRing* r, uint16_t i) {
    uint16_t start = (uint16_t)((r->head + r->cap - r->count) % r->cap);
    return &r->rec[(start + i) % r->cap];
}
