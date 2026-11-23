// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
/* Continental TG1C LF Lab — probe sequencer, status rule, telemetry tally and
 * heap preflight (plan v3 §2, §3, §4b, §4d). Header-only, SDK-free.
 *
 * The device worker does nothing but execute the actions tg1c_seq_next() hands
 * out and report what happened, so every transmit decision is made here and is
 * exercised by tests/tg1c_seq.c on the host.
 *
 * RULES (from plan v3 §2; they hold for every selection):
 *   - Only the loose spare 0F61C9F5's telemetry counts as a control.
 *   - In a Sequence, CONNECT is sent only after the spare answered the wake in
 *     the same session; otherwise the run ends NO COUPLING. In a Bit sweep the
 *     same goes for the first variant.
 *   - The first CRC-valid bootloader reply, or any telemetry status outside
 *     {87/01, 87/02} from any sensor, latches tx_locked. No burst is issued
 *     after that: the run goes receive-only and stops, after 120 s for a
 *     bootloader reply and, from v0.3.6, once 10 s pass with no telemetry
 *     frame (120 s at most) for a status.
 *
 * From 11 November 2026 the owner allows write/transmit frames on the spare
 * and on Bad sensor 0F3A6CB8 (see tg1c_frame.h). The rules above still hold for
 * every selection; the Bit sweep (v0.3.0, below), the Dilution test (v0.3.1),
 * the Offset test and the Scan (v0.3.2) add frames, not exceptions: the latch
 * and the receive-only stop apply to them unchanged.
 *
 * v0.3.3 adds the Park command selection and a target choice. With the target
 * set to the Bad sensor the control is 0F3A6CB8 and its normal pairs are F7/01
 * and F7/02; a pair outside a sensor's normal set still latches, and for this
 * selection that latch is the result looked for.
 *
 * CONNECT spacing (owner ruling 2026-11-10): BtLdr.c answers CONNECT once, then
 * listens on a different LF ID and rejects command 01 until its 30-35 s
 * timeout. Back-to-back CONNECT bursts are therefore not independent, so every
 * CONNECT burst is followed by 40 s of receive-only before the next one.
 */
#include "tg1c_frame.h"
#include "tg1c_ring.h"
#include <stdio.h>
#include <string.h>

#define TG1C_SPARE_ID 0x0F61C9F5u
#define TG1C_MAX_BURSTS 3u
#define TG1C_RX_HZ_WAKE 433920000u /* TG1C telemetry */
#define TG1C_RX_HZ_NXP 434000000u /* BtLdr.c: 434.000 MHz +/-35 kHz */
#define TG1C_POST_BURST_MS 2000u
#define TG1C_CONNECT_SPACING_MS 40000u /* burst end to next CONNECT burst */
#define TG1C_QUIET_MS 5000u
#define TG1C_QUIET_CAP_MS 60000u
#define TG1C_OBSERVE_MS 120000u
/* v0.3.6: after an unknown status the observe ends once this long has passed
 * with no telemetry frame, TG1C_OBSERVE_MS being the cap. Every reply seen so
 * far ended within 6 s of the LF stopping. After a bootloader reply the
 * observe is still the full TG1C_OBSERVE_MS. */
#define TG1C_OBSERVE_QUIET_MS 10000u
#define TG1C_HALFBIT_US 128u
#define TG1C_HEAP_RESERVE 16384u

/* Known-normal telemetry status pairs (B4/B7). Anything else is a candidate
 * and stops transmission. */
static inline bool tg1c_status_known(uint8_t b4, uint8_t b7) {
    return b4 == 0x87 && (b7 == 0x01 || b7 == 0x02);
}

/* ----- Target (v0.3.3) -------------------------------------------------------
 * Which sensor a session is aimed at. The spare is the default and leaves every
 * rule above as it was. With the Bad sensor as target, its telemetry is the
 * control and its normal pairs are Fx/01 and Fx/02: F7 in run 13, and F7 then
 * F6 at the wheel on 12 November 2026 (2 C), so from v0.3.5 the low nibble of
 * B4 is not judged for it. Every other sensor is still judged by 87/01 and
 * 87/02. */
typedef enum {
    TG1C_TARGET_SPARE = 0,
    TG1C_TARGET_BAD,
    TG1C_TARGETS,
} TG1cTarget;
#define TG1C_BAD_ID 0x0F3A6CB8u
static inline uint32_t tg1c_target_id(uint8_t target) {
    return target == TG1C_TARGET_BAD ? TG1C_BAD_ID : TG1C_SPARE_ID;
}
static inline const char* tg1c_target_str(uint8_t target) {
    return target == TG1C_TARGET_BAD ? "bad" : "spare";
}
/* Is this frame's status pair a normal one for the sensor that sent it? */
static inline bool tg1c_frame_known(uint8_t target, uint32_t id, uint8_t b4, uint8_t b7) {
    if(target == TG1C_TARGET_BAD && id == TG1C_BAD_ID) return (b4 & 0xF0) == 0xF0 && (b7 == 0x01 || b7 == 0x02);
    return tg1c_status_known(b4, b7);
}

/* ----- Reading a frame for the screen (v0.3.6) --------------------------------
 * Pressure and temperature are the open TG1C decoder's (rtl_433): B5 x 1.38 kPa
 * and B6 - 50 C. The other two are readings of B4, and both are candidates:
 *
 *  Battery: a workshop tool derives an OK flag
 *  from B4 by sensor type. Older Jaguar/Land Rover types: bit 7 set. Hyundai
 *  H-1 and others on the same Continental part family: bit 4 set, or high
 *  nibble 8. We do not know which our sensors fall under, so OK needs both,
 *  LOW neither, and one alone is UNSURE. What the tool shows when its rule
 *  fails (LOW or NONE) was not traced.
 *
 *  Class: our own observation, not from any tool. Top two bits 10 on every
 *  sensor a car accepts (87 Good and spare, B7 Abarth); 11 only on the Bad
 *  sensor (F7, F6), which a workshop tool would call Ship mode; anything else
 *  (77 after frame E, 74 on a cold Abarth frame) is unexplained. */
static inline unsigned tg1c_kpa(uint8_t b5) {
    return ((unsigned)b5 * 138u + 50u) / 100u;
}
static inline int tg1c_temp_c(uint8_t b6) {
    return (int)b6 - 50;
}
typedef enum { TG1C_BATT_OK = 0, TG1C_BATT_UNSURE, TG1C_BATT_LOW } TG1cBatt;
static inline uint8_t tg1c_b4_batt(uint8_t b4) {
    bool older = (b4 & 0x80) != 0;
    bool newer = (b4 & 0x10) != 0 || (b4 & 0xF0) == 0x80;
    return older && newer ? TG1C_BATT_OK : older || newer ? TG1C_BATT_UNSURE : TG1C_BATT_LOW;
}
static inline const char* tg1c_batt_str(uint8_t batt) {
    return batt == TG1C_BATT_OK ? "ok" : batt == TG1C_BATT_LOW ? "lo?" : "?";
}
typedef enum { TG1C_CLASS_NORMAL = 0, TG1C_CLASS_SHIP, TG1C_CLASS_OTHER } TG1cClass;
static inline uint8_t tg1c_b4_class(uint8_t b4) {
    return (b4 & 0xC0) == 0x80 ? TG1C_CLASS_NORMAL : (b4 & 0xC0) == 0xC0 ? TG1C_CLASS_SHIP : TG1C_CLASS_OTHER;
}
static inline const char* tg1c_class_str(uint8_t cls) {
    return cls == TG1C_CLASS_NORMAL ? "norm?" : cls == TG1C_CLASS_SHIP ? "ship?" : "other";
}
/* "231kPa 2C bat ok ship?": at most 25 characters. */
static inline void tg1c_decode_text(char* out, size_t cap, uint8_t b4, uint8_t b5, uint8_t b6) {
    snprintf(out, cap, "%ukPa %dC bat %s %s", tg1c_kpa(b5), tg1c_temp_c(b6), tg1c_batt_str(tg1c_b4_batt(b4)),
             tg1c_class_str(tg1c_b4_class(b4)));
}

/* ----- Operator options (plan v3 §4b; defaults from bench-timing pass-07) ---- */
#define TG1C_BURST_OPTIONS 3
#define TG1C_BURST_DEFAULT 1
static const uint16_t kTg1cBurstMs[TG1C_BURST_OPTIONS] = {1000, 5000, 10000};
#define TG1C_LEAD_OPTIONS 4
#define TG1C_LEAD_DEFAULT 1
static const uint16_t kTg1cLeadUs[TG1C_LEAD_OPTIONS] = {0, 2048, 6000, 12000};
#define TG1C_GAP_OPTIONS 4
#define TG1C_GAP_DEFAULT 1
static const uint16_t kTg1cGapMs[TG1C_GAP_OPTIONS] = {5, 8, 20, 40};

typedef enum {
    TG1C_SEL_A = 0, /* default */
    TG1C_SEL_B,
    TG1C_SEL_C,
    TG1C_SEL_D,
    TG1C_SEL_SEQUENCE, /* A, then B, C, D if the control answered */
    TG1C_SEL_SWEEP, /* v0.3.0: single-bit variants of A, each on the listening grid */
    TG1C_SEL_DILUTION, /* v0.3.1: A as every Nth frame of a train of rejected variants */
    TG1C_SEL_OFFSET, /* v0.3.2: A as every 16th frame, its place in the cycle moved per trial */
    TG1C_SEL_SCAN, /* v0.3.2: groups of candidate frames, interleaved */
    TG1C_SEL_PARK, /* v0.3.3: wake, the disable-shipping frame E, wake; compare the status */
    TG1C_SEL_COUNT,
} TG1cSelection;
#define TG1C_SEL_DEFAULT TG1C_SEL_A

typedef enum {
    TG1C_RXF_ALTERNATE = 0, /* burst 1 at 434.00, 2 at 433.92, 3 at 434.00 */
    TG1C_RXF_434000,
    TG1C_RXF_433920,
    TG1C_RXF_COUNT,
} TG1cRxFreqSel;
#define TG1C_RXF_DEFAULT TG1C_RXF_ALTERNATE

/* ----- Heap preflight (plan v3 §3) -------------------------------------------
 * One 16-bit carrier sample per 8 us; the envelope is allocated per burst. */
static inline size_t tg1c_envelope_samples(size_t nslots, uint16_t half_us, uint32_t lead_us) {
    return nslots * (half_us / 8u) + lead_us / 8u;
}
static inline size_t tg1c_preflight_need(size_t max_slots, uint16_t half_us, uint32_t lead_us) {
    return tg1c_envelope_samples(max_slots, half_us, lead_us) * sizeof(uint16_t) +
           TG1C_RING_RECORDS * sizeof(TG1cRawRecord) + TG1C_HEAP_RESERVE;
}
static inline bool
    tg1c_preflight_ok(size_t free_heap, size_t max_slots, uint16_t half_us, uint32_t lead_us) {
    return free_heap >= tg1c_preflight_need(max_slots, half_us, lead_us);
}
/* Largest frame a selection can transmit. */
static inline size_t tg1c_selection_max_slots(uint8_t sel) {
    switch(sel) {
    case TG1C_SEL_A:
    case TG1C_SEL_SWEEP:
    case TG1C_SEL_DILUTION:
    case TG1C_SEL_OFFSET:
    case TG1C_SEL_SCAN:
    case TG1C_SEL_PARK: return TG1C_SLOTS_A; /* fillers, candidates and frame E are frame A's length */
    case TG1C_SEL_B: return TG1C_SLOTS_B;
    default: return TG1C_MAX_FRAME_SLOTS;
    }
}

/* ----- Telemetry tally for one log row ---------------------------------------*/
#define TG1C_TALLY_PAIRS 6
typedef struct {
    uint16_t frames; /* checksum-valid TG1C frames, any sensor */
    uint16_t spare_frames; /* of those, from the spare */
    uint16_t other_frames;
    uint32_t last_id;
    uint8_t pairs; /* distinct B4/B7 pairs recorded */
    uint8_t pair_b4[TG1C_TALLY_PAIRS], pair_b7[TG1C_TALLY_PAIRS];
    bool unknown; /* a pair outside the known set was seen */
    uint8_t unknown_b4, unknown_b7; /* the first such pair */
} TG1cTally;

static inline void tg1c_tally_reset(TG1cTally* t) {
    memset(t, 0, sizeof(*t));
}
/* Record one frame of a session aimed at `target`. Returns true if its status
 * pair is not a normal one for its sender. spare_frames counts the target's. */
static inline bool tg1c_tally_add_for(TG1cTally* t, uint8_t target, uint32_t id, uint8_t b4, uint8_t b7) {
    if(t->frames < UINT16_MAX) t->frames++;
    if(id == tg1c_target_id(target)) {
        if(t->spare_frames < UINT16_MAX) t->spare_frames++;
    } else if(t->other_frames < UINT16_MAX) {
        t->other_frames++;
    }
    t->last_id = id;
    bool seen = false;
    for(uint8_t i = 0; i < t->pairs; i++)
        if(t->pair_b4[i] == b4 && t->pair_b7[i] == b7) seen = true;
    if(!seen && t->pairs < TG1C_TALLY_PAIRS) {
        t->pair_b4[t->pairs] = b4;
        t->pair_b7[t->pairs] = b7;
        t->pairs++;
    }
    bool unknown = !tg1c_frame_known(target, id, b4, b7);
    if(unknown && !t->unknown) {
        t->unknown = true;
        t->unknown_b4 = b4;
        t->unknown_b7 = b7;
    }
    return unknown;
}
/* As above with the spare as target (every version before v0.3.3). */
static inline bool tg1c_tally_add(TG1cTally* t, uint32_t id, uint8_t b4, uint8_t b7) {
    return tg1c_tally_add_for(t, TG1C_TARGET_SPARE, id, b4, b7);
}
/* "87/01;87/02" into out; an empty string when no frame was seen. */
static inline void tg1c_tally_pairs(const TG1cTally* t, char* out, size_t cap) {
    size_t n = 0;
    if(cap) out[0] = 0;
    for(uint8_t i = 0; i < t->pairs; i++) {
        if(n + 7 > cap) break; /* ";XX/YY" + NUL */
        n += (size_t)snprintf(
            out + n, cap - n, "%s%02X/%02X", i ? ";" : "", t->pair_b4[i], t->pair_b7[i]);
    }
}

/* ----- Bit sweep (v0.3.0) ----------------------------------------------------
 * One session sends the wake frame with one data bit flipped, for each of 12
 * consecutive bits, and records whether the spare still answers.
 *
 * Timing rule, a CANDIDATE from the bench (analysis/tg1c-bench-20261111 and
 * tg1c-cycle-check-20261111): the spare checks for LF once per about 31.6 s,
 * counted from the end of its last reply, and a burst it does not answer leaves
 * that count alone. Every sweep burst is therefore placed so the predicted
 * check falls inside a 10 s burst:
 *     variant: start = anchor + TG1C_LISTEN_PERIOD_MS - TG1C_SWEEP_BURST_MS / 2
 *     control: start = anchor + 2 * TG1C_LISTEN_PERIOD_MS - TG1C_CONTROL_LEAD_MS
 * where anchor is the tick of the last spare frame of the last answered burst.
 * The variant comes one period after it (26.6 s), the stock control after a
 * silent variant two periods after it. From v0.3.2 the control starts 4 s
 * before its predicted check, not 5 s (59.2 s, not 58.2 s): the bench of
 * 12 November (analysis/tg1c-sweep-rerun-20261112) had replies to the control
 * between 61.9 s and 68.0 s after the anchor, the last ones 0.2 s before the
 * burst ended.
 *
 * Flow:
 *   sync     stock wake bursts as the wake control sends them (up to 3). None
 *            answered: NO COUPLING, nothing else is sent.
 *   variant  one burst on the grid. Answered: the bit is `woke`, and the reply
 *            is the new anchor.
 *   control  after a silent variant, one stock burst at n = 2. Answered: the
 *            bit is `silent`, verified by a wake one period later.
 *   resync   after a silent control, up to 3 stock bursts back to back.
 *            Answered: the bit is `unverified`; it is tried once more and a
 *            second unverified result is recorded as such. Not answered: the
 *            session stops NO WAKE and names the bit, a candidate that the
 *            variant changed the sensor. */
#define TG1C_LISTEN_PERIOD_MS 31600u
#define TG1C_SWEEP_BURST_MS 10000u
#define TG1C_CONTROL_LEAD_MS 4000u /* a control burst starts this long before two periods are up */
#define TG1C_SWEEP_BITS 12u /* bits per session */
#define TG1C_SWEEP_RANGES (TG1C_WAKE_BITS / TG1C_SWEEP_BITS)
#define TG1C_SWEEP_TRIES 2u /* a bit is sent at most twice */
/* Worst case: 3 sync bursts, then per bit two tries of variant + control + 3
 * resync bursts. */
#define TG1C_SWEEP_MAX_BURSTS (TG1C_MAX_BURSTS + TG1C_SWEEP_BITS * TG1C_SWEEP_TRIES * (2u + TG1C_MAX_BURSTS))

/* ----- Dilution test (v0.3.1) ------------------------------------------------
 * Question: how many frames does the spare examine each time it listens? A
 * diluted burst is a 10 s train in which the unaltered wake frame is only every
 * Nth frame (frame index % N == 0, counted from the first frame of the burst)
 * and every other frame is a filler: frame A with one bit flipped, which the
 * bit sweep of 11-12 November showed the spare does not answer. If the spare
 * still answers at ratio N it examined a stretch of the train long enough to
 * contain a stock frame.
 *
 * One session runs the ladder N = 2, 4, 8, 16, 32 with TG1C_DIL_TRIALS trials
 * per ratio, 15 trials in all, on the sweep's machinery: the diluted burst
 * takes the place of the sweep's variant (one period after the last reply),
 * the stock control follows a silent one at two periods, and a silent control
 * leads to a resync. Differences from the sweep: a trial whose control was
 * silent is recorded `unverified` and is not repeated, and the filler bit is
 * an operator choice (bit 3, inside the first 14 bits, or bit 40, after them:
 * analysis/tg1c-sweep-20261112 found the spare's timing differs between the
 * two groups). */
#define TG1C_DIL_RATIOS 5u
static const uint8_t kTg1cDilRatio[TG1C_DIL_RATIOS] = {2, 4, 8, 16, 32};
#define TG1C_DIL_TRIALS 3u
#define TG1C_DIL_ITEMS (TG1C_DIL_RATIOS * TG1C_DIL_TRIALS)
#define TG1C_DIL_FILLS 2
#define TG1C_DIL_FILL_DEFAULT 0
static const uint8_t kTg1cDilFillBit[TG1C_DIL_FILLS] = {3, 40};
/* Worst case: 3 sync bursts, then per trial diluted + control + 3 resync bursts. */
#define TG1C_DIL_MAX_BURSTS (TG1C_MAX_BURSTS + TG1C_DIL_ITEMS * (2u + TG1C_MAX_BURSTS))

/* ----- Offset test (v0.3.2) --------------------------------------------------
 * Question: how many frames does the spare examine each time it listens? The
 * dilution session of 12 November could not say, because the spare listens at
 * the same point of every burst and frame 0 of each burst was stock: what it
 * found there depended on the ratio only by accident of alignment
 * (analysis/tg1c-dilution-20261112). Here the ratio is fixed at 16 and the
 * place of the stock frame in the 16-frame cycle moves by one frame per trial:
 * in trial t (0..15) the stock frames are those with frame index % 16 == t.
 * The number of offsets that get an on-time answer is the number of frames the
 * spare examines. Same machinery and filler choice as the dilution test; one
 * diluted burst per trial, no retry. */
#define TG1C_OFF_RATIO 16u
#define TG1C_OFF_ITEMS 16u
#define TG1C_OFF_MAX_BURSTS (TG1C_MAX_BURSTS + TG1C_OFF_ITEMS * (2u + TG1C_MAX_BURSTS))

/* ----- Scan (v0.3.2) ---------------------------------------------------------
 * Tests the candidate frames of tg1c_scan_bytes() in small groups. A session
 * is up to 16 groups; group g holds the candidates from + g * G .. + G - 1 of
 * the chosen set, cut at the end of the set. A group burst is a 10 s train in
 * which frame i carries candidate i % n of the group (n = the candidates the
 * group has), one period after the last reply. Telemetry from the spare in
 * that burst: the group is `woke`. None: the stock control follows at two
 * periods, and the group is `silent` if that is answered, else `unverified`
 * after a resync. No retry, and no follow-up on a `woke` group: run it again
 * with a smaller group to find the frame. The BYTE group that holds index 39
 * contains frame A itself and should come out `woke`. */
#define TG1C_SCAN_GROUPS 16u /* groups per session */
#define TG1C_SCAN_GROUP_OPTIONS 4
#define TG1C_SCAN_GROUP_DEFAULT 2
static const uint8_t kTg1cScanGroup[TG1C_SCAN_GROUP_OPTIONS] = {1, 2, 4, 8};
#define TG1C_SCAN_GROUP_MAX 8u
#define TG1C_SCAN_FROM_OPTIONS 16 /* start index = choice * 16 */
#define TG1C_SCAN_FROM_STEP 16u
#define TG1C_SCAN_MAX_BURSTS (TG1C_MAX_BURSTS + TG1C_SCAN_GROUPS * (2u + TG1C_MAX_BURSTS))
/* Result slots a grid session needs: 12 sweep bits, 15 dilution trials, 16
 * offsets or 16 scan groups. */
#define TG1C_GRID_ITEMS 16u
_Static_assert(TG1C_GRID_ITEMS >= TG1C_SWEEP_BITS && TG1C_GRID_ITEMS >= TG1C_DIL_ITEMS, "grid result slots");
_Static_assert(TG1C_GRID_ITEMS >= TG1C_OFF_ITEMS && TG1C_GRID_ITEMS >= TG1C_SCAN_GROUPS, "grid result slots");

/* ----- Park command (v0.3.3; reworked in v0.3.5) ------------------------------
 * Sends payload E, the frame a workshop tool sends to take a Continental sensor
 * out of shipping mode (tg1c_frame.h), and reads the target's status before
 * it, in its answer to it, and after it. Not on the listening grid: the spare
 * listens every 31 to 33 s, the Bad sensor at the wheel answered a wake within
 * a second, so the command is simply sent until the target is heard.
 *
 *   before   stock wake bursts, up to TG1C_PARK_WAKE_BURSTS, until the target
 *            answers with a normal pair, which is recorded. None answered: NO
 *            COUPLING, and frame E is not sent. Any other pair latches, as in
 *            every selection.
 *   command  up to TG1C_PARK_CMD_BURSTS bursts of frame E. The first frame
 *            from the target, whatever its status, is the acknowledgement: its
 *            pair is recorded and the burst is cut there. (On the bench the
 *            spare answered frame E with 77/01, five groups over 6 s that ran
 *            on with the LF off.)
 *   settle   after an acknowledgement, receive-only until the target has been
 *            quiet for 5 s.
 *   after    stock wake bursts as before, until the target answers; its pair,
 *            whatever it is, is recorded and the burst is cut. PARK CMD DONE.
 *            Not answered: NO REPLY AFTER CMD, a candidate.
 * From the command stage on, the target's own frames do not latch: a new pair
 * is what the test is for. A frame from any other sensor with a pair that is
 * not normal for it still does. The status text compares the high nibble of B4
 * before and after. Bursts are 10 s whatever the Burst option says. */
#define TG1C_PARK_WAKE_BURSTS 6u
#define TG1C_PARK_CMD_BURSTS 6u
#define TG1C_PARK_MAX_BURSTS (2u * TG1C_PARK_WAKE_BURSTS + TG1C_PARK_CMD_BURSTS)
#define TG1C_PARK_BURST_MS 10000u
#define TG1C_PARK_SETTLE_MS 5000u /* quiet time that ends the settle wait */
#define TG1C_PARK_SETTLE_CAP_MS 60000u

typedef enum {
    TG1C_ROLE_NONE = 0, /* every selection but the sweep and the dilution test */
    TG1C_ROLE_SYNC,
    TG1C_ROLE_VARIANT,
    TG1C_ROLE_CONTROL,
    TG1C_ROLE_RESYNC,
    TG1C_ROLE_COUNT,
} TG1cRole;
static inline const char* tg1c_role_str(uint8_t role) {
    static const char* const name[TG1C_ROLE_COUNT] = {"-", "sync", "variant", "control", "resync"};
    return role < TG1C_ROLE_COUNT ? name[role] : "?";
}
/* As tg1c_role_str, but the timed burst of a dilution or offset test is
 * `diluted` and that of a scan is `scan`. */
static inline const char* tg1c_role_name(uint8_t role, uint8_t sel) {
    if(role != TG1C_ROLE_VARIANT) return tg1c_role_str(role);
    return (sel == TG1C_SEL_DILUTION || sel == TG1C_SEL_OFFSET) ? "diluted" :
           sel == TG1C_SEL_SCAN                                 ? "scan" :
                                                                  tg1c_role_str(role);
}

typedef enum {
    TG1C_BIT_NOT_RUN = 0,
    TG1C_BIT_WOKE, /* the spare answered the variant */
    TG1C_BIT_SILENT, /* no answer to the variant; the stock control one period later answered */
    TG1C_BIT_UNVERIFIED, /* no answer to the variant, and none to its control either */
    TG1C_BIT_RESULTS,
} TG1cBitResult;
static inline const char* tg1c_bit_result_str(uint8_t r) {
    static const char* const name[TG1C_BIT_RESULTS] = {"not_run", "woke", "silent", "unverified"};
    return r < TG1C_BIT_RESULTS ? name[r] : "?";
}

/* Start tick of a grid burst timed for the check n periods after the anchor:
 * n = 1 puts the check in the middle of the burst, n = 2 (the control) puts it
 * TG1C_CONTROL_LEAD_MS after the start. Unsigned arithmetic: correct across a
 * tick wrap. */
static inline uint32_t tg1c_sweep_start(uint32_t anchor, uint8_t n) {
    return anchor + (uint32_t)n * TG1C_LISTEN_PERIOD_MS -
           (n >= 2 ? TG1C_CONTROL_LEAD_MS : TG1C_SWEEP_BURST_MS / 2u);
}

/* ----- Sequencer -------------------------------------------------------------*/
typedef enum {
    TG1C_ACT_BURST = 0, /* send payload as a frame train, RX live, then listen wait_ms */
    TG1C_ACT_QUIET_WAIT, /* receive-only until wait_ms pass with no telemetry, cap cap_ms */
    TG1C_ACT_SPACING, /* receive-only for wait_ms between CONNECT bursts */
    TG1C_ACT_OBSERVE, /* receive-only for wait_ms after a latch */
    TG1C_ACT_DONE,
    TG1C_ACT_GRID_WAIT, /* sweep: receive-only until the tick `planned_tick` */
} TG1cActionKind;

typedef enum {
    TG1C_ST_RUNNING = 0,
    TG1C_ST_CONTROL_OK, /* payload A alone: the spare answered */
    TG1C_ST_NO_COUPLING, /* the spare did not answer the wake */
    TG1C_ST_NO_REPLY, /* CONNECT bursts done, no CRC-valid reply (inconclusive) */
    TG1C_ST_BOOTLOADER_REPLY, /* CRC-valid reply: a candidate, not a proof */
    TG1C_ST_UNKNOWN_STATUS, /* unfamiliar B4/B7 pair: a candidate */
    TG1C_ST_CANCELLED,
    TG1C_ST_ERROR,
    TG1C_ST_SWEEP_DONE, /* every bit of the range has a result */
    TG1C_ST_NO_WAKE, /* sweep: stock wakes stopped being answered after a variant: a candidate */
    TG1C_ST_DILUTION_DONE, /* every trial of the ladder has a result */
    TG1C_ST_OFFSET_DONE, /* every offset has a result */
    TG1C_ST_SCAN_DONE, /* every group of the session has a result */
    TG1C_ST_PARK_DONE, /* park command: the target answered afterwards; compare the pairs */
    TG1C_ST_PARK_SILENT, /* park command: the target did not answer afterwards: a candidate */
} TG1cStatus;

static inline const char* tg1c_status_str(uint8_t st) {
    switch(st) {
    case TG1C_ST_CONTROL_OK: return "CONTROL OK";
    case TG1C_ST_NO_COUPLING: return "NO COUPLING - reposition";
    case TG1C_ST_NO_REPLY: return "NO REPLY (inconclusive)";
    case TG1C_ST_BOOTLOADER_REPLY: return "BOOTLOADER REPLY";
    case TG1C_ST_UNKNOWN_STATUS: return "UNKNOWN STATUS - stopped";
    case TG1C_ST_CANCELLED: return "CANCELLED";
    case TG1C_ST_ERROR: return "ERROR";
    case TG1C_ST_SWEEP_DONE: return "SWEEP DONE";
    case TG1C_ST_NO_WAKE: return "NO WAKE after variant";
    case TG1C_ST_DILUTION_DONE: return "DILUTION DONE";
    case TG1C_ST_OFFSET_DONE: return "OFFSET DONE";
    case TG1C_ST_SCAN_DONE: return "SCAN DONE";
    case TG1C_ST_PARK_DONE: return "PARK CMD DONE";
    case TG1C_ST_PARK_SILENT: return "NO REPLY AFTER CMD";
    default: return "RUNNING";
    }
}

typedef struct {
    uint8_t kind; /* TG1cActionKind */
    TG1cPayload payload; /* for BURST */
    uint8_t burst_no, burst_max; /* 1-based, for BURST */
    uint32_t rx_hz; /* receive frequency for this action */
    uint32_t wait_ms;
    uint32_t cap_ms; /* for QUIET_WAIT */
    uint32_t quiet_ms; /* for OBSERVE (v0.3.6): end once this long passes with no telemetry; 0: run all of wait_ms */
    /* Sweep only (v0.3.0); role NONE, bit -1 and timed false everywhere else. */
    uint8_t role; /* TG1cRole of a BURST, or of the burst a GRID_WAIT leads to */
    int8_t bit; /* wake-frame bit flipped in this burst (0..47); -1: the stock frame */
    int8_t sweep_bit; /* the bit the sweep is working on, also for its stock bursts; -1: none */
    bool timed; /* planned_tick is set */
    uint32_t planned_tick; /* GRID_WAIT: wait until this tick; BURST: its planned start */
    /* Dilution only (v0.3.1); ratio 0, trial 0, diluted false and fill_bit -1
     * everywhere else. */
    uint8_t ratio; /* the ratio N the dilution test is working on, also for its stock bursts */
    uint8_t trial; /* 1-based trial of that ratio */
    bool diluted; /* this BURST is the diluted train: stock every `ratio`th frame */
    int8_t fill_bit; /* wake-frame bit flipped in the filler frames of a diluted burst */
    /* Offset test and scan only (v0.3.2); zero and false everywhere else. An
     * offset test also sets ratio (16), trial (offset + 1), diluted and fill_bit. */
    bool off_test; /* the item is a trial of an offset test (also for its stock bursts) */
    uint8_t offset; /* a diluted burst's stock frames are frame index % ratio == offset */
    bool scan; /* this BURST cycles through the candidates of a scan group */
    uint8_t scan_set; /* TG1cScanSet of the scan, also for its stock bursts */
    uint8_t scan_first; /* first candidate index of the group the scan is working on */
    uint8_t scan_n; /* candidates in that group (1..8); 0: not a scan, or a sync burst */
    /* Park command only (v0.3.5); false everywhere else. */
    bool any_status; /* the target's frames are part of the test whatever their pair: no latch on them */
    bool stop_on_target; /* cut this BURST at the first frame from the target */
} TG1cAction;

/* What happened while the last action ran. */
typedef struct {
    bool spare_telemetry; /* a known-status frame from the spare arrived */
    bool unknown_status; /* a frame with an unfamiliar B4/B7 pair arrived */
    bool reply_valid; /* a CRC-valid bootloader reply arrived */
    bool fault; /* LF engine / receiver / memory fault */
    bool cancelled; /* operator pressed Back */
    bool nothing_sent; /* a BURST was issued but not one frame went out */
    /* Tick of the last known-status spare frame of the action; meaningful only
     * with spare_telemetry. The sweep takes its timing from it. */
    uint32_t last_spare_tick;
    /* v0.3.3. tgt_*: the pair of that same frame (with spare_telemetry; "spare"
     * here means the session's target). unk_*: the first pair of the session
     * that was not normal for its sender (with unknown_status). */
    uint8_t tgt_b4, tgt_b7;
    uint8_t unk_b4, unk_b7;
    /* v0.3.5. A frame from the target arrived, whatever its pair, and the pair
     * of the latest such frame. */
    bool tgt_any;
    uint8_t any_b4, any_b7;
} TG1cOutcome;

typedef struct {
    uint8_t sel, rxf;
    uint8_t stage; /* 0 = first payload of the selection */
    uint8_t burst; /* bursts completed for the current payload */
    uint8_t connect_bursts, total_bursts;
    bool control_ok; /* the spare answered payload A in this session */
    bool quiet_done;
    bool spaced; /* 40 s have passed since the last CONNECT burst */
    bool tx_locked; /* latched: no burst may follow */
    bool observed;
    uint8_t latched; /* TG1cStatus to report after the observe phase */
    uint8_t status; /* TG1cStatus; RUNNING until the run ends */
    uint8_t last_kind; /* kind of the action last handed out */
    uint32_t last_rx_hz;
    /* Sweep (v0.3.0) and dilution test (v0.3.1): the grid selections. An item
     * is one bit of a sweep or one trial of a dilution ladder. */
    uint8_t sw_items; /* items in the session: 12 bits, 15 trials, 16 offsets or up to 16 groups */
    uint8_t sw_max_tries; /* times an item may be sent: 2 in a sweep, 1 in the others */
    uint8_t dl_fill; /* dilution and offset test: wake-frame bit flipped in the filler frames */
    uint8_t sc_set, sc_group, sc_from; /* scan: TG1cScanSet, group size 1..8, first candidate index */
    uint8_t sw_first; /* first bit of the range */
    uint8_t sw_idx; /* items that have a result; the item in progress */
    uint8_t sw_role; /* TG1cRole of the burst to send next */
    uint8_t sw_try; /* tries completed for the bit in progress */
    bool sw_waited; /* the grid wait before the next timed burst has run */
    uint32_t sw_anchor; /* tick of the last spare frame of the last answered burst */
    uint8_t sw_strays; /* grid waits in which the spare was heard with no LF on */
    uint8_t sw_result[TG1C_GRID_ITEMS]; /* TG1cBitResult */
    uint8_t sw_tries[TG1C_GRID_ITEMS]; /* variant (or diluted) bursts sent for the item */
    /* v0.3.3: target, and the park command's record. */
    uint8_t target; /* TG1cTarget */
    bool pk_have_before, pk_have_after, pk_have_new;
    uint8_t pk_before_b4, pk_before_b7; /* the target's pair before frame E */
    uint8_t pk_after_b4, pk_after_b7; /* its pair afterwards, whatever it is */
    uint8_t pk_new_b4, pk_new_b7; /* the pair that latched, if one did */
    uint8_t pk_cmd_sent; /* frame E bursts from which at least one frame went out */
    uint8_t pk_cmd_replies; /* of those, bursts during which the target was heard */
    bool pk_have_ack, pk_settled; /* v0.3.5: the target answered frame E; the settle wait has run */
    uint8_t pk_ack_b4, pk_ack_b7; /* the pair of its first frame in answer to frame E */
} TG1cSeq;

/* The selections that run on the listening grid. */
static inline bool tg1c_sel_is_grid(uint8_t sel) {
    return sel == TG1C_SEL_SWEEP || sel == TG1C_SEL_DILUTION || sel == TG1C_SEL_OFFSET ||
           sel == TG1C_SEL_SCAN;
}
/* The selections whose timed burst mixes the stock frame with a filler. */
static inline bool tg1c_sel_is_diluted(uint8_t sel) {
    return sel == TG1C_SEL_DILUTION || sel == TG1C_SEL_OFFSET;
}
/* Groups a scan session has: 16, or fewer when the set ends first. */
static inline uint8_t tg1c_scan_groups(uint8_t set, uint8_t group, uint8_t from) {
    uint16_t count = tg1c_scan_count(set);
    if(!group || from >= count) return 0;
    uint16_t groups = (uint16_t)((count - from + group - 1u) / group);
    return (uint8_t)(groups > TG1C_SCAN_GROUPS ? TG1C_SCAN_GROUPS : groups);
}

static inline void tg1c_seq_init(TG1cSeq* s, uint8_t sel, uint8_t rxf) {
    memset(s, 0, sizeof(*s));
    s->sel = sel < TG1C_SEL_COUNT ? sel : TG1C_SEL_DEFAULT;
    s->rxf = rxf < TG1C_RXF_COUNT ? rxf : TG1C_RXF_DEFAULT;
    s->spaced = true;
    s->last_kind = TG1C_ACT_DONE;
    s->last_rx_hz = TG1C_RX_HZ_WAKE;
    s->sw_role = TG1C_ROLE_SYNC;
    s->sc_set = TG1C_SCAN_BYTE;
    s->sc_group = kTg1cScanGroup[TG1C_SCAN_GROUP_DEFAULT];
    s->sc_from = 0;
    s->sw_items = s->sel == TG1C_SEL_DILUTION ? (uint8_t)TG1C_DIL_ITEMS :
                  s->sel == TG1C_SEL_OFFSET   ? (uint8_t)TG1C_OFF_ITEMS :
                  s->sel == TG1C_SEL_SCAN     ? tg1c_scan_groups(s->sc_set, s->sc_group, s->sc_from) :
                                                (uint8_t)TG1C_SWEEP_BITS;
    s->sw_max_tries = s->sel == TG1C_SEL_SWEEP ? (uint8_t)TG1C_SWEEP_TRIES : 1;
    s->dl_fill = kTg1cDilFillBit[TG1C_DIL_FILL_DEFAULT];
}

/* Choose the session's target (TG1cTarget). Call after tg1c_seq_init. */
static inline void tg1c_seq_set_target(TG1cSeq* s, uint8_t target) {
    s->target = target < TG1C_TARGETS ? target : TG1C_TARGET_SPARE;
}
/* Park command: did the high nibble of B4 change between the wake before frame
 * E and the wake after it? False until both are known. */
static inline bool tg1c_seq_park_changed(const TG1cSeq* s) {
    return s->pk_have_before && s->pk_have_after && ((s->pk_before_b4 ^ s->pk_after_b4) & 0xF0) != 0;
}
/* Park command: the text for a latch, "NEW STATUS 87/01"; false when that is
 * not what happened. */
static inline bool tg1c_seq_park_changed_text(const TG1cSeq* s, char* out, size_t cap) {
    if(s->sel != TG1C_SEL_PARK || s->status != TG1C_ST_UNKNOWN_STATUS || !s->pk_have_new) return false;
    snprintf(out, cap, "NEW STATUS %02X/%02X", s->pk_new_b4, s->pk_new_b7);
    return true;
}

/* Choose which 12 bits a sweep covers: range 0 = bits 0-11 .. 3 = bits 36-47.
 * Call after tg1c_seq_init; no effect on any other selection. */
static inline void tg1c_seq_set_sweep_range(TG1cSeq* s, uint8_t range) {
    if(range >= TG1C_SWEEP_RANGES) range = 0;
    s->sw_first = (uint8_t)(range * TG1C_SWEEP_BITS);
}
/* The bit a sweep is working on, or -1 (not a sweep, or all bits done). */
static inline int8_t tg1c_seq_sweep_bit(const TG1cSeq* s) {
    if(s->sel != TG1C_SEL_SWEEP || s->sw_idx >= TG1C_SWEEP_BITS) return -1;
    return (int8_t)(s->sw_first + s->sw_idx);
}
static inline uint8_t tg1c_seq_sweep_count(const TG1cSeq* s, uint8_t result) {
    uint8_t n = 0;
    for(uint8_t i = 0; i < TG1C_SWEEP_BITS; i++)
        if(s->sw_result[i] == result) n++;
    return n;
}
/* Choose the filler of a dilution or offset test: 0 = bit 3, 1 = bit 40. Call
 * after tg1c_seq_init; no effect on any other selection. */
static inline void tg1c_seq_set_dilution_fill(TG1cSeq* s, uint8_t fill) {
    s->dl_fill = kTg1cDilFillBit[fill < TG1C_DIL_FILLS ? fill : TG1C_DIL_FILL_DEFAULT];
}
/* The ratio a dilution test is working on, and the 1-based trial of it; 0 when
 * this is not a dilution test or every trial is done. */
static inline uint8_t tg1c_seq_dil_ratio(const TG1cSeq* s) {
    if(s->sel != TG1C_SEL_DILUTION || s->sw_idx >= TG1C_DIL_ITEMS) return 0;
    return kTg1cDilRatio[s->sw_idx / TG1C_DIL_TRIALS];
}
static inline uint8_t tg1c_seq_dil_trial(const TG1cSeq* s) {
    if(s->sel != TG1C_SEL_DILUTION || s->sw_idx >= TG1C_DIL_ITEMS) return 0;
    return (uint8_t)(s->sw_idx % TG1C_DIL_TRIALS + 1u);
}
/* Trials of ratio index `ri` (0..4) with the given result; all ratios for ri < 0. */
static inline uint8_t tg1c_seq_dil_count(const TG1cSeq* s, int ri, uint8_t result) {
    uint8_t n = 0;
    for(uint8_t i = 0; i < TG1C_DIL_ITEMS; i++)
        if((ri < 0 || i / TG1C_DIL_TRIALS == (uint8_t)ri) && s->sw_result[i] == result) n++;
    return n;
}
/* The offset an offset test is working on (0..15), or -1 (not an offset test,
 * or every offset done). */
static inline int8_t tg1c_seq_off_offset(const TG1cSeq* s) {
    if(s->sel != TG1C_SEL_OFFSET || s->sw_idx >= TG1C_OFF_ITEMS) return -1;
    return (int8_t)s->sw_idx;
}
/* 16 characters, one per offset: W woke, . silent, ? unverified, - not run. */
static inline void tg1c_seq_off_map(const TG1cSeq* s, char out[TG1C_OFF_ITEMS + 1u]) {
    static const char mark[TG1C_BIT_RESULTS] = {'-', 'W', '.', '?'};
    for(uint8_t i = 0; i < TG1C_OFF_ITEMS; i++)
        out[i] = s->sw_result[i] < TG1C_BIT_RESULTS ? mark[s->sw_result[i]] : '-';
    out[TG1C_OFF_ITEMS] = 0;
}
/* Choose what a scan covers: the set, the group size (choice 0..3 = 1, 2, 4, 8
 * candidates per burst) and the first candidate (choice 0..15 = index 0, 16,
 * .. 240). A start past the end of the set is moved back to the last step
 * inside it. Call after tg1c_seq_init; no effect on any other selection. */
static inline void tg1c_seq_set_scan(TG1cSeq* s, uint8_t set, uint8_t group_choice, uint8_t from_choice) {
    if(s->sel != TG1C_SEL_SCAN) return;
    s->sc_set = set < TG1C_SCAN_SETS ? set : TG1C_SCAN_BYTE;
    s->sc_group = kTg1cScanGroup[group_choice < TG1C_SCAN_GROUP_OPTIONS ? group_choice : TG1C_SCAN_GROUP_DEFAULT];
    uint16_t count = tg1c_scan_count(s->sc_set);
    uint16_t from = (uint16_t)((from_choice < TG1C_SCAN_FROM_OPTIONS ? from_choice : 0) * TG1C_SCAN_FROM_STEP);
    while(from >= count) from = (uint16_t)(from - TG1C_SCAN_FROM_STEP);
    s->sc_from = (uint8_t)from;
    s->sw_items = tg1c_scan_groups(s->sc_set, s->sc_group, s->sc_from);
}
/* First candidate index of scan group g, and how many candidates it has (0:
 * not a scan, or no such group). */
static inline uint8_t tg1c_seq_scan_first(const TG1cSeq* s, uint8_t g) {
    return (uint8_t)(s->sc_from + g * s->sc_group);
}
static inline uint8_t tg1c_seq_scan_n(const TG1cSeq* s, uint8_t g) {
    if(s->sel != TG1C_SEL_SCAN || g >= s->sw_items) return 0;
    uint16_t left = (uint16_t)(tg1c_scan_count(s->sc_set) - tg1c_seq_scan_first(s, g));
    return (uint8_t)(left < s->sc_group ? left : s->sc_group);
}
/* Items of the session (offsets, groups, trials, bits) with the given result. */
static inline uint8_t tg1c_seq_item_count(const TG1cSeq* s, uint8_t result) {
    uint8_t n = 0;
    for(uint8_t i = 0; i < s->sw_items && i < TG1C_GRID_ITEMS; i++)
        if(s->sw_result[i] == result) n++;
    return n;
}
/* "woke: none", or "woke 38 3C": the first index, in hex, of each scan group
 * that woke the spare; a "+" ends the list when it does not fit. */
static inline void tg1c_seq_scan_woke_text(const TG1cSeq* s, char* out, size_t cap) {
    size_t n = (size_t)snprintf(out, cap, "woke");
    bool any = false;
    for(uint8_t g = 0; g < s->sw_items && g < TG1C_GRID_ITEMS; g++) {
        if(s->sw_result[g] != TG1C_BIT_WOKE) continue;
        any = true;
        if(n + 5 > cap) { /* " XX" + "+" + NUL */
            if(n + 2 <= cap) snprintf(out + n, cap - n, "+");
            return;
        }
        n += (size_t)snprintf(out + n, cap - n, " %02X", tg1c_seq_scan_first(s, g));
    }
    if(!any) snprintf(out, cap, "woke: none");
}
/* The status as text; NO WAKE names the bit (or the trial, the offset, the
 * group) whose burst came before it. */
static inline void tg1c_seq_status_text(const TG1cSeq* s, char* out, size_t cap) {
    if(tg1c_seq_park_changed_text(s, out, cap)) return;
    if(s->status == TG1C_ST_PARK_DONE && tg1c_seq_park_changed(s))
        snprintf(out, cap, "CHANGED %02X>%02X", s->pk_before_b4, s->pk_after_b4);
    else if(s->status == TG1C_ST_PARK_DONE && s->pk_have_after)
        snprintf(out, cap, "NO CHANGE %02X/%02X", s->pk_after_b4, s->pk_after_b7);
    else if(s->status == TG1C_ST_NO_WAKE && tg1c_seq_off_offset(s) >= 0)
        snprintf(out, cap, "NO WAKE at offset %d", tg1c_seq_off_offset(s));
    else if(s->status == TG1C_ST_NO_WAKE && tg1c_seq_scan_n(s, s->sw_idx))
        snprintf(
            out,
            cap,
            "NO WAKE after scan %02X-%02X",
            tg1c_seq_scan_first(s, s->sw_idx),
            tg1c_seq_scan_first(s, s->sw_idx) + tg1c_seq_scan_n(s, s->sw_idx) - 1u);
    else if(s->status == TG1C_ST_NO_WAKE && tg1c_seq_dil_ratio(s))
        snprintf(out, cap, "NO WAKE at ratio %u trial %u", tg1c_seq_dil_ratio(s), tg1c_seq_dil_trial(s));
    else if(s->status == TG1C_ST_NO_WAKE && tg1c_seq_sweep_bit(s) >= 0)
        snprintf(out, cap, "NO WAKE after bit %d", tg1c_seq_sweep_bit(s));
    else if(s->status == TG1C_ST_SWEEP_DONE)
        snprintf(out, cap, "SWEEP DONE bits %u-%u", s->sw_first, s->sw_first + TG1C_SWEEP_BITS - 1u);
    else
        snprintf(out, cap, "%s", tg1c_status_str(s->status));
}

static inline uint8_t tg1c_seq_stages(const TG1cSeq* s) {
    return s->sel == TG1C_SEL_SEQUENCE ? 4 : s->sel == TG1C_SEL_PARK ? 3 : 1;
}
static inline TG1cPayload tg1c_seq_payload(const TG1cSeq* s) {
    if(s->sel == TG1C_SEL_SEQUENCE) return (TG1cPayload)s->stage; /* A, B, C, D */
    if(s->sel == TG1C_SEL_PARK) return s->stage == 1 ? TG1C_PAYLOAD_E : TG1C_PAYLOAD_A; /* wake, E, wake */
    if(tg1c_sel_is_grid(s->sel)) return TG1C_PAYLOAD_A; /* stock, a one-bit variant, or a mix of both */
    return (TG1cPayload)s->sel;
}
static inline uint32_t tg1c_seq_connect_hz(const TG1cSeq* s, uint8_t burst_no) {
    if(s->rxf == TG1C_RXF_434000) return TG1C_RX_HZ_NXP;
    if(s->rxf == TG1C_RXF_433920) return TG1C_RX_HZ_WAKE;
    return (burst_no == 2) ? TG1C_RX_HZ_WAKE : TG1C_RX_HZ_NXP;
}

/* The next thing to do. Does not advance: call tg1c_seq_report() afterwards. */
static inline TG1cAction tg1c_seq_next(TG1cSeq* s) {
    TG1cAction a = {0};
    a.rx_hz = s->last_rx_hz;
    a.bit = -1;
    a.sweep_bit = -1;
    a.fill_bit = -1;
    if(s->status != TG1C_ST_RUNNING) {
        a.kind = TG1C_ACT_DONE;
    } else if(s->tx_locked) {
        /* After a reply stay on the frequency it arrived on; after an unknown
         * status watch the telemetry frequency. */
        a.kind = TG1C_ACT_OBSERVE;
        a.wait_ms = TG1C_OBSERVE_MS;
        if(s->latched == TG1C_ST_UNKNOWN_STATUS) {
            a.rx_hz = TG1C_RX_HZ_WAKE;
            a.quiet_ms = TG1C_OBSERVE_QUIET_MS;
        }
    } else if(tg1c_sel_is_grid(s->sel)) {
        bool timed = s->sw_role == TG1C_ROLE_VARIANT || s->sw_role == TG1C_ROLE_CONTROL;
        a.rx_hz = TG1C_RX_HZ_WAKE;
        a.payload = TG1C_PAYLOAD_A;
        a.role = s->sw_role;
        a.sweep_bit = s->sw_role == TG1C_ROLE_SYNC ? -1 : tg1c_seq_sweep_bit(s);
        if(s->sw_role != TG1C_ROLE_SYNC) {
            a.ratio = tg1c_seq_dil_ratio(s);
            a.trial = tg1c_seq_dil_trial(s);
            if(tg1c_seq_off_offset(s) >= 0) {
                a.off_test = true;
                a.ratio = (uint8_t)TG1C_OFF_RATIO;
                a.offset = (uint8_t)tg1c_seq_off_offset(s);
                a.trial = (uint8_t)(a.offset + 1u);
            }
            a.scan_n = tg1c_seq_scan_n(s, s->sw_idx);
            if(a.scan_n) {
                a.scan_set = s->sc_set;
                a.scan_first = tg1c_seq_scan_first(s, s->sw_idx);
            }
        }
        if(timed) {
            a.timed = true;
            a.planned_tick =
                tg1c_sweep_start(s->sw_anchor, s->sw_role == TG1C_ROLE_VARIANT ? 1 : 2);
        }
        if(timed && !s->sw_waited) {
            a.kind = TG1C_ACT_GRID_WAIT;
        } else {
            a.kind = TG1C_ACT_BURST;
            if(s->sw_role == TG1C_ROLE_VARIANT) a.bit = tg1c_seq_sweep_bit(s);
            if(s->sw_role == TG1C_ROLE_VARIANT && a.ratio) {
                a.diluted = true;
                a.fill_bit = (int8_t)s->dl_fill;
            }
            if(s->sw_role == TG1C_ROLE_VARIANT && a.scan_n) a.scan = true;
            a.burst_no = timed ? 1 : (uint8_t)(s->burst + 1);
            a.burst_max = timed ? 1 : TG1C_MAX_BURSTS;
            a.wait_ms = TG1C_POST_BURST_MS;
        }
    } else {
        TG1cPayload p = tg1c_seq_payload(s);
        bool connect = tg1c_payload_is_connect(p);
        if(connect && s->sel == TG1C_SEL_SEQUENCE && !s->quiet_done) {
            a.kind = TG1C_ACT_QUIET_WAIT;
            a.wait_ms = TG1C_QUIET_MS;
            a.cap_ms = TG1C_QUIET_CAP_MS;
        } else if(s->sel == TG1C_SEL_PARK && s->stage == 2 && s->pk_have_ack && !s->pk_settled) {
            /* Let the answer to frame E finish before the next wake. */
            a.kind = TG1C_ACT_QUIET_WAIT;
            a.wait_ms = TG1C_PARK_SETTLE_MS;
            a.cap_ms = TG1C_PARK_SETTLE_CAP_MS;
            a.rx_hz = TG1C_RX_HZ_WAKE;
            a.any_status = true;
        } else if(connect && !s->spaced) {
            /* A reply follows the LF frame within milliseconds, inside the
             * burst or its post-burst listen. The rest of the spacing is only
             * the sensor's timeout, so spend it on the telemetry frequency
             * where the unknown-status monitor can hear (v0.2.1). */
            a.kind = TG1C_ACT_SPACING;
            a.wait_ms = TG1C_CONNECT_SPACING_MS - TG1C_POST_BURST_MS;
            a.rx_hz = TG1C_RX_HZ_WAKE;
        } else {
            a.kind = TG1C_ACT_BURST;
            a.payload = p;
            a.burst_no = (uint8_t)(s->burst + 1);
            a.burst_max = s->sel != TG1C_SEL_PARK ? TG1C_MAX_BURSTS :
                          s->stage == 1           ? TG1C_PARK_CMD_BURSTS :
                                                    TG1C_PARK_WAKE_BURSTS;
            a.rx_hz = connect ? tg1c_seq_connect_hz(s, a.burst_no) : TG1C_RX_HZ_WAKE;
            a.wait_ms = TG1C_POST_BURST_MS;
            /* The first wake is not cut at the target's first frame: its
             * reply runs on for up to 0.3 s after the LF stops, and a frame
             * of that tail arriving in a frame E burst would be taken for
             * the answer to frame E. The full burst and its 2 s listen keep
             * the two apart. */
            if(s->sel == TG1C_SEL_PARK && s->stage >= 1) a.any_status = a.stop_on_target = true;
        }
    }
    s->last_kind = a.kind;
    s->last_rx_hz = a.rx_hz;
    return a;
}

/* Report the outcome of the action last returned by tg1c_seq_next(). */
static inline void tg1c_seq_report(TG1cSeq* s, const TG1cOutcome* o) {
    if(s->status != TG1C_ST_RUNNING) return;
    uint8_t kind = s->last_kind;

    /* Count a burst from which at least one frame went out, whatever ended
     * it. A refused start or a fault before the first frame sent nothing, and
     * must not read as a burst (least of all as a CONNECT) on screen. */
    if(kind == TG1C_ACT_BURST && !o->nothing_sent) {
        s->total_bursts++;
        if(tg1c_payload_is_connect(tg1c_seq_payload(s))) {
            s->connect_bursts++;
            s->spaced = false;
        }
        /* A variant that went out is counted for its bit even if the burst
         * then latched, faulted or was cancelled. */
        if(tg1c_sel_is_grid(s->sel) && s->sw_role == TG1C_ROLE_VARIANT &&
           s->sw_idx < s->sw_items && s->sw_tries[s->sw_idx] < UINT8_MAX)
            s->sw_tries[s->sw_idx]++;
    }

    /* Park command: a frame E burst that went out counts, and so does a reply
     * heard during it, even if the burst then latched or was cancelled. */
    if(s->sel == TG1C_SEL_PARK && kind == TG1C_ACT_BURST && s->stage == 1 && !o->nothing_sent) {
        if(s->pk_cmd_sent < UINT8_MAX) s->pk_cmd_sent++;
        if(o->tgt_any && s->pk_cmd_replies < UINT8_MAX) s->pk_cmd_replies++;
    }

    if(o->reply_valid) {
        s->tx_locked = true;
        s->latched = TG1C_ST_BOOTLOADER_REPLY;
    } else if(o->unknown_status && s->latched != TG1C_ST_BOOTLOADER_REPLY) {
        s->tx_locked = true;
        s->latched = TG1C_ST_UNKNOWN_STATUS;
        if(!s->pk_have_new) {
            s->pk_have_new = true;
            s->pk_new_b4 = o->unk_b4;
            s->pk_new_b7 = o->unk_b7;
        }
    }
    if(o->fault) {
        s->tx_locked = true;
        s->status = TG1C_ST_ERROR;
        return;
    }
    if(o->cancelled) {
        bool keep = s->tx_locked && s->latched != TG1C_ST_RUNNING;
        s->tx_locked = true;
        s->status = keep ? s->latched : TG1C_ST_CANCELLED;
        return;
    }
    if(s->tx_locked) {
        if(kind == TG1C_ACT_OBSERVE) {
            s->observed = true;
            s->status = s->latched;
        }
        return; /* otherwise: observe next */
    }

    if(tg1c_sel_is_grid(s->sel)) {
        if(kind == TG1C_ACT_GRID_WAIT) {
            s->sw_waited = true;
            /* Heard with no LF on: recorded, and the grid is left as it was. */
            if(o->spare_telemetry && s->sw_strays < UINT8_MAX) s->sw_strays++;
            return;
        }
        if(kind != TG1C_ACT_BURST || s->sw_idx >= s->sw_items) return;
        bool answered = o->spare_telemetry;
        bool bit_done = false;
        if(answered) s->sw_anchor = o->last_spare_tick;
        s->sw_waited = false;
        switch(s->sw_role) {
        case TG1C_ROLE_SYNC:
            s->burst++;
            if(answered) {
                s->control_ok = true;
                s->sw_role = TG1C_ROLE_VARIANT;
                s->burst = 0;
            } else if(s->burst >= TG1C_MAX_BURSTS) {
                s->status = TG1C_ST_NO_COUPLING; /* and no variant is sent */
            }
            break;
        case TG1C_ROLE_VARIANT:
            if(answered) {
                s->sw_result[s->sw_idx] = TG1C_BIT_WOKE;
                bit_done = true;
            } else {
                s->sw_role = TG1C_ROLE_CONTROL;
            }
            break;
        case TG1C_ROLE_CONTROL:
            if(answered) {
                s->sw_result[s->sw_idx] = TG1C_BIT_SILENT;
                bit_done = true;
            } else {
                s->sw_role = TG1C_ROLE_RESYNC;
                s->burst = 0;
            }
            break;
        case TG1C_ROLE_RESYNC:
            s->burst++;
            if(answered) {
                s->burst = 0;
                s->sw_try++;
                if(s->sw_try >= s->sw_max_tries) {
                    s->sw_result[s->sw_idx] = TG1C_BIT_UNVERIFIED;
                    bit_done = true;
                } else {
                    s->sw_role = TG1C_ROLE_VARIANT; /* the same bit once more (sweep only) */
                }
            } else if(s->burst >= TG1C_MAX_BURSTS) {
                s->status = TG1C_ST_NO_WAKE; /* sw_idx stays on the bit */
            }
            break;
        default:
            break;
        }
        if(bit_done) {
            s->sw_idx++;
            s->sw_try = 0;
            s->burst = 0;
            s->sw_role = TG1C_ROLE_VARIANT;
            if(s->sw_idx >= s->sw_items)
                s->status = s->sel == TG1C_SEL_DILUTION ? TG1C_ST_DILUTION_DONE :
                            s->sel == TG1C_SEL_OFFSET   ? TG1C_ST_OFFSET_DONE :
                            s->sel == TG1C_SEL_SCAN     ? TG1C_ST_SCAN_DONE :
                                                          TG1C_ST_SWEEP_DONE;
        }
        return;
    }

    if(s->sel == TG1C_SEL_PARK) {
        if(kind == TG1C_ACT_QUIET_WAIT) s->pk_settled = true;
        if(kind != TG1C_ACT_BURST) return;
        s->burst++;
        if(s->stage == 0) {
            if(o->spare_telemetry) {
                s->control_ok = true;
                s->pk_have_before = true;
                s->pk_before_b4 = o->tgt_b4;
                s->pk_before_b7 = o->tgt_b7;
                s->stage = 1;
                s->burst = 0;
            } else if(s->burst >= TG1C_PARK_WAKE_BURSTS) {
                s->status = TG1C_ST_NO_COUPLING; /* and frame E is not sent */
            }
        } else if(s->stage == 1) {
            if(o->tgt_any) {
                s->pk_have_ack = true;
                s->pk_ack_b4 = o->any_b4;
                s->pk_ack_b7 = o->any_b7;
            }
            if(o->tgt_any || s->burst >= TG1C_PARK_CMD_BURSTS) {
                s->stage = 2;
                s->burst = 0;
            }
        } else if(o->tgt_any) {
            s->pk_have_after = true;
            s->pk_after_b4 = o->any_b4;
            s->pk_after_b7 = o->any_b7;
            s->status = TG1C_ST_PARK_DONE;
        } else if(s->burst >= TG1C_PARK_WAKE_BURSTS) {
            s->status = TG1C_ST_PARK_SILENT;
        }
        return;
    }

    switch(kind) {
    case TG1C_ACT_QUIET_WAIT:
        s->quiet_done = true;
        break;
    case TG1C_ACT_SPACING:
        s->spaced = true;
        break;
    case TG1C_ACT_BURST: {
        TG1cPayload p = tg1c_seq_payload(s);
        s->burst++;
        if(p == TG1C_PAYLOAD_A) {
            if(o->spare_telemetry) {
                s->control_ok = true;
                if(s->sel == TG1C_SEL_SEQUENCE) {
                    s->stage = 1;
                    s->burst = 0;
                } else {
                    s->status = TG1C_ST_CONTROL_OK;
                }
            } else if(s->burst >= TG1C_MAX_BURSTS) {
                s->status = TG1C_ST_NO_COUPLING; /* and no CONNECT is sent */
            }
        } else if(s->burst >= TG1C_MAX_BURSTS) {
            s->burst = 0;
            s->stage++;
            if(s->stage >= tg1c_seq_stages(s)) s->status = TG1C_ST_NO_REPLY;
        }
        break;
    }
    default:
        break;
    }
}
