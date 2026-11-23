// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
/* Continental TG1C LF Lab — frame + CRC core (v0.2.0, plan v3 §4a; wake-frame
 * variants added in v0.3.0, the dilution rule in v0.3.1).
 *
 * Header-only and SDK-free: the CONNECT CRC, the packet layout and the slot
 * builders are pure logic, so the host tests compile this file with a plain C
 * compiler. The device app includes the same header. Nothing here allocates or
 * touches hardware.
 *
 * Authority: the NXP TPMS bootloader sources under
 * references/vendor-packages/nxp-and-siemens/TPMS_Bootloader_v1.3/Source_Code/
 * (MKW01_BtLdr_1/Sources/LF_App.c for the LF master, FXTH_BtLdr_1/Sources/
 * BtLdr.c for the sensor side). tests/tg1c_lf_reference.py replays
 * LF_TxStateSeq and frame B must equal it slot for slot.
 *
 * SCOPE. This file is shared with the LF Lab (flipper/dev/lf_lab), which
 * also sends test frames: tg1c_build_wake_variant() and the dilution and
 * scan helpers below. The main app calls none of them. It builds the wake
 * frame and payload E only, and its test script fails if that changes.
 *
 * v0.3.1 adds the dilution test, which sends no new frame: it mixes the stock
 * wake with one of those one-bit variants in a fixed ratio (see the end of
 * this file and tg1c_seq.h).
 *
 * v0.3.2 lets the stock frame of a diluted burst sit at any position of the
 * cycle (the offset test), and adds tg1c_scan_bytes(): two small families of
 * wake-shaped frames that differ from frame A in several bits at once. What
 * such a frame does to a sensor is unknown.
 *
 * v0.3.3 adds payload E, the frame a workshop tool sends for its "DISABLE SHIPPING MODE"
 * function: 7A 86 30 F4 4F 0B in that tool's polarity, 85 79 CF 0B B0 F4 in
 * ours, in frame A's shape. Read from the tool's code, not captured on air
 * (analysis/research-options-20261112/NOTES.md section 8). One owner of the
 * same sensor family reports it moved their sensors from Ship to Park. It is
 * meant to change the sensor's mode: send it only to the spare or the Bad
 * sensor.
 *
 * There is still no builder for the bootloader commands 0x02 (verify), 0x03
 * (read memory), 0x04 (erase), 0x05 (program) or 0x06 (disconnect) in this
 * version: no CONNECT reply has been seen, so they would have nothing to act on.
 */
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* ----- CRC-CCITT (bootloader variant): poly 0x1021, init 0x1D0F, final ~crc.
 * Over {06 01 A0 A1 A2 A3 00} -> 0x2E7F; over {LEN..CRC_LO} the residue is
 * 0xE2F0 (BtLdr.c u8fBLValidateLFCmd). */
static inline uint16_t tg1c_crc_ccitt(const uint8_t* buf, size_t len) {
    uint16_t crc = 0x1D0F;
    for(size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)buf[i] << 8;
        for(int b = 0; b < 8; b++)
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
    }
    return (uint16_t)~crc;
}

/* ----- CONNECT (LF command 0x01, "Connect to Slave"): the nine counted bytes
 * LEN..CRC_LO, equal to cu8BootEntryKey in BtLdr.c. The LF ID is a separate
 * on-air prefix, matched by the receiver hardware and not covered by the CRC. */
#define TG1C_CONNECT_PKT_LEN 9

static inline void tg1c_connect_packet(uint8_t out[TG1C_CONNECT_PKT_LEN]) {
    out[0] = 0x06; /* LEN  */
    out[1] = 0x01; /* CMD  = 0x01 Connect to Slave */
    out[2] = 0xA0; /* DATA */
    out[3] = 0xA1;
    out[4] = 0xA2;
    out[5] = 0xA3;
    out[6] = 0x00; /* CNTR */
    uint16_t crc = tg1c_crc_ccitt(out, 7);
    out[7] = (uint8_t)(crc >> 8);
    out[8] = (uint8_t)(crc & 0xFF);
}

/* ----- Slot helpers ----------------------------------------------------------
 * A slot is one 128 us half-bit; the envelope engine expands each slot into
 * carrier samples. Slots are 0 (carrier off) or 1 (carrier on).
 *
 * one_first = true : data bit 1 -> (1,0), bit 0 -> (0,1)   [profile-4 wake]
 * one_first = false: data bit 1 -> (0,1), bit 0 -> (1,0)   [NXP LF_Xmit_1/_0]
 * Bytes go MSB first. */
static inline size_t
    tg1c_manchester_byte(uint8_t byte, bool one_first, uint8_t* out, size_t cap) {
    if(cap < 16) return 0;
    for(int i = 7; i >= 0; i--) {
        bool bit = (byte >> i) & 1;
        bool high_first = one_first ? bit : !bit;
        *out++ = high_first ? 1 : 0;
        *out++ = high_first ? 0 : 1;
    }
    return 16;
}

/* The 9T synchronisation pattern (LF_Xmit_9T), 18 slots. */
#define TG1C_SYNC_SLOTS 18
static inline size_t tg1c_sync_9t(uint8_t* out, size_t cap) {
    static const uint8_t sync[TG1C_SYNC_SLOTS] = {1, 1, 1, 0, 0, 0, 1, 0, 1,
                                                  1, 0, 0, 1, 1, 0, 0, 1, 0};
    if(cap < TG1C_SYNC_SLOTS) return 0;
    for(size_t i = 0; i < TG1C_SYNC_SLOTS; i++) out[i] = sync[i];
    return TG1C_SYNC_SLOTS;
}

/* End of message. LF_TxStateSeq case 3 leaves the carrier on for two half-bit
 * slots after the data; its third interrupt turns the carrier on and off again
 * before returning (owner ruling 2026-11-10, replacing plan v3's "three"). The
 * envelope engine forces the very last carrier sample off. */
#define TG1C_EOM_SLOTS 2

/* ----- The four payloads (plan v3 §4a) ---------------------------------------
 * A  stock wake, the control. The unchanged 184-slot profile-4 frame:
 *    35 x (1,0) | 9T | 9E A1 EC 39 93 C6 with 1 -> (1,0). No EOM is appended.
 *    Read with the NXP convention the same slots are 61 5E 13 C6 6C 39.
 * B  CONNECT on the NXP public LF ID 5A42, as the NXP master sends it:
 *    32 x (0,1) | 9T | 5A 42 | packet, all 1 -> (0,1) | EOM.
 * C  CONNECT behind the proven wake preamble and the wake's first two bytes:
 *    35 x (1,0) | 9T | 61 5E with 1 -> (0,1) | packet 1 -> (0,1) | EOM.
 *    HYPOTHESIS: that 61 5E is a Continental 16-bit LF ID.
 * D  As C with the packet encoded 1 -> (1,0).
 *    HYPOTHESIS: as C, plus the sensor using the opposite data polarity. */
typedef enum {
    TG1C_PAYLOAD_A = 0,
    TG1C_PAYLOAD_B,
    TG1C_PAYLOAD_C,
    TG1C_PAYLOAD_D,
    TG1C_PAYLOAD_E, /* v0.3.3: the tool's disable-shipping frame, in frame A's shape */
    TG1C_PAYLOAD_COUNT,
} TG1cPayload;

#define TG1C_SLOTS_A 184
#define TG1C_SLOTS_B (32 * 2 + TG1C_SYNC_SLOTS + (2 + TG1C_CONNECT_PKT_LEN) * 16 + TG1C_EOM_SLOTS)
#define TG1C_SLOTS_C (35 * 2 + TG1C_SYNC_SLOTS + (2 + TG1C_CONNECT_PKT_LEN) * 16 + TG1C_EOM_SLOTS)
#define TG1C_SLOTS_D TG1C_SLOTS_C
#define TG1C_MAX_FRAME_SLOTS TG1C_SLOTS_C

static inline size_t tg1c_frame_slots(TG1cPayload key) {
    switch(key) {
    case TG1C_PAYLOAD_A: return TG1C_SLOTS_A;
    case TG1C_PAYLOAD_B: return TG1C_SLOTS_B;
    case TG1C_PAYLOAD_C: return TG1C_SLOTS_C;
    case TG1C_PAYLOAD_D: return TG1C_SLOTS_D;
    case TG1C_PAYLOAD_E: return TG1C_SLOTS_A;
    default: return 0;
    }
}

static inline bool tg1c_payload_is_connect(TG1cPayload key) {
    return key == TG1C_PAYLOAD_B || key == TG1C_PAYLOAD_C || key == TG1C_PAYLOAD_D;
}

/* LF ID bytes the frame carries after the sync (0 = none, frame A). */
static inline uint16_t tg1c_payload_lf_id(TG1cPayload key) {
    switch(key) {
    case TG1C_PAYLOAD_B: return 0x5A42;
    case TG1C_PAYLOAD_C:
    case TG1C_PAYLOAD_D: return 0x615E;
    default: return 0;
    }
}

/* Slot pair that encodes a data 1 in the frame's body, for the log. */
static inline const char* tg1c_payload_body_polarity(TG1cPayload key) {
    return (key == TG1C_PAYLOAD_A || key == TG1C_PAYLOAD_D || key == TG1C_PAYLOAD_E) ? "1>10" : "1>01";
}

/* ----- Wake-frame variants (v0.3.0) ------------------------------------------
 * The six data bytes of frame A, and the same frame with one of their 48 bits
 * flipped before Manchester encoding. Bit 0 is the MSB of the first byte (9E),
 * bit 47 the LSB of the last (C6). Preamble, 9T sync, polarity (1 -> 1,0) and
 * the missing EOM are frame A's, so a variant differs from A in exactly the two
 * slots of the flipped bit. flip_bit < 0 gives the unchanged bytes. */
#define TG1C_WAKE_BYTES 6
#define TG1C_WAKE_BITS (TG1C_WAKE_BYTES * 8)

static inline bool tg1c_wake_bytes(int flip_bit, uint8_t out[TG1C_WAKE_BYTES]) {
    static const uint8_t stock_wake[TG1C_WAKE_BYTES] = {0x9E, 0xA1, 0xEC, 0x39, 0x93, 0xC6};
    if(flip_bit >= TG1C_WAKE_BITS) return false;
    for(int i = 0; i < TG1C_WAKE_BYTES; i++) out[i] = stock_wake[i];
    if(flip_bit >= 0) out[flip_bit / 8] ^= (uint8_t)(0x80u >> (flip_bit % 8));
    return true;
}

/* A frame of frame A's shape (preamble, 9T sync, 1 -> (1,0), no EOM) around any
 * six data bytes. Returns TG1C_SLOTS_A, or 0 for a buffer smaller than the frame. */
static inline size_t tg1c_build_wake_bytes(const uint8_t bytes[TG1C_WAKE_BYTES], uint8_t* out, size_t cap) {
    if(!bytes || !out || cap < TG1C_SLOTS_A) return 0;
    size_t n = 0;
    for(int i = 0; i < 35; i++) {
        out[n++] = 1;
        out[n++] = 0;
    }
    n += tg1c_sync_9t(out + n, cap - n);
    for(size_t b = 0; b < TG1C_WAKE_BYTES; b++)
        n += tg1c_manchester_byte(bytes[b], true, out + n, cap - n);
    return n == TG1C_SLOTS_A ? n : 0;
}

/* Frame A with data bit flip_bit (0..47) flipped. Returns TG1C_SLOTS_A, or 0 for a bit
 * outside 0..47 or a buffer smaller than the frame. */
static inline size_t tg1c_build_wake_variant(int flip_bit, uint8_t* out, size_t cap) {
    uint8_t bytes[TG1C_WAKE_BYTES];
    if(flip_bit < 0 || !tg1c_wake_bytes(flip_bit, bytes)) return 0;
    return tg1c_build_wake_bytes(bytes, out, cap);
}

/* ----- Dilution (v0.3.1; the offset in v0.3.2) -------------------------------
 * A diluted burst sends the stock wake frame as every `ratio`th frame and a
 * filler (a one-bit variant, built by tg1c_build_wake_variant) as all the
 * others. Frames are counted from 0 at the start of the burst; the stock frames
 * are those with frame index % ratio == offset. The dilution test uses offset
 * 0, so its frame 0 is stock; the offset test moves the stock frame through
 * the cycle. An offset of `ratio` or more is taken modulo the ratio. A ratio
 * below 2 means no dilution: every frame is stock. */
static inline bool tg1c_dilution_is_stock(uint32_t frame_index, uint8_t ratio, uint8_t offset) {
    return ratio < 2 || frame_index % ratio == (uint32_t)(offset % ratio);
}
/* Stock frames among the first `frames` frames of such a burst. */
static inline uint32_t tg1c_dilution_stock_count(uint32_t frames, uint8_t ratio, uint8_t offset) {
    if(ratio < 2) return frames;
    uint32_t first = offset % ratio;
    return frames > first ? (frames - first + ratio - 1u) / ratio : 0;
}

/* ----- Scan candidates (v0.3.2) ----------------------------------------------
 * Two families of six-byte frames, each sent in frame A's shape
 * (tg1c_build_wake_bytes). Both come from one observation about frame A,
 * 9E A1 EC 39 93 C6 (analysis/tg1c-dilution-20261112/RESULT.md): its last four
 * bytes read as the one byte 39 written four ways, and seven of their eight
 * nibbles have exactly two bits set. Whether the sensor reads the frame that
 * way is NOT known; these are frames to try.
 *
 * BYTE, index X = 0..255: 9E A1 | (~swap(X)) ^ 0x80 | X | swap(X) | ~X, where
 *   swap exchanges the two nibbles. X = 0x39 is frame A itself.
 * NIBBLE, index 0..34: frame A with one of its nibbles 5..11 (0 = high nibble
 *   of byte 0, so 5 = low nibble of EC .. 11 = low nibble of C6) replaced by
 *   another of the six two-bit nibbles 3 5 6 9 A C. index = (p - 5) * 5 + a,
 *   a counting the five other values in ascending order. */
typedef enum {
    TG1C_SCAN_BYTE = 0,
    TG1C_SCAN_NIBBLE,
    TG1C_SCAN_SETS,
} TG1cScanSet;
#define TG1C_SCAN_BYTE_COUNT 256u
#define TG1C_SCAN_NIBBLE_COUNT 35u
#define TG1C_SCAN_STOCK_BYTE 0x39u /* the BYTE index that is frame A */

static inline uint16_t tg1c_scan_count(uint8_t set) {
    return set == TG1C_SCAN_BYTE   ? (uint16_t)TG1C_SCAN_BYTE_COUNT :
           set == TG1C_SCAN_NIBBLE ? (uint16_t)TG1C_SCAN_NIBBLE_COUNT :
                                     0;
}
static inline const char* tg1c_scan_set_str(uint8_t set) {
    return set == TG1C_SCAN_BYTE ? "byte" : set == TG1C_SCAN_NIBBLE ? "nibble" : "?";
}
/* The six data bytes of candidate `index` of a set. False for an unknown set or
 * an index past its end. */
static inline bool tg1c_scan_bytes(uint8_t set, uint16_t index, uint8_t out[TG1C_WAKE_BYTES]) {
    static const uint8_t two_bit[6] = {0x3, 0x5, 0x6, 0x9, 0xA, 0xC};
    if(index >= tg1c_scan_count(set) || !tg1c_wake_bytes(-1, out)) return false;
    if(set == TG1C_SCAN_BYTE) {
        uint8_t x = (uint8_t)index, swapped = (uint8_t)((x << 4) | (x >> 4));
        out[2] = (uint8_t)(~swapped ^ 0x80u);
        out[3] = x;
        out[4] = swapped;
        out[5] = (uint8_t)~x;
        return true;
    }
    uint8_t p = (uint8_t)(5u + index / 5u), a = (uint8_t)(index % 5u);
    uint8_t shift = (p & 1u) ? 0 : 4;
    uint8_t now = (uint8_t)((out[p / 2u] >> shift) & 0xFu);
    for(uint8_t i = 0; i < 6; i++) {
        if(two_bit[i] == now) continue;
        if(a-- == 0) {
            out[p / 2u] = (uint8_t)((out[p / 2u] & ~(0xFu << shift)) | (uint8_t)(two_bit[i] << shift));
            return true;
        }
    }
    return false; /* not reached: nibbles 5..11 of frame A are all two-bit ones */
}

/* ----- Payload E (v0.3.3): the disable-shipping frame ------------------------
 * Six bytes in this repo's polarity (1 -> 1,0). The tool that sends them stores
 * 7A 86 30 F4 4F 0B, every bit inverted, exactly as it stores frame A as
 * 61 5E 13 C6 6C 39. Like frame A the last four bytes are one byte (0B) written
 * four ways; unlike frame A the first two are 85 79. */
static inline void tg1c_park_bytes(uint8_t out[TG1C_WAKE_BYTES]) {
    static const uint8_t park[TG1C_WAKE_BYTES] = {0x85, 0x79, 0xCF, 0x0B, 0xB0, 0xF4};
    for(int i = 0; i < TG1C_WAKE_BYTES; i++) out[i] = park[i];
}

/* Build one frame. Returns the slot count, or 0 for an unknown key or a buffer
 * smaller than the frame. */
static inline size_t tg1c_build_frame(TG1cPayload key, uint8_t* out, size_t cap) {
    static const uint8_t stock_wake[6] = {0x9E, 0xA1, 0xEC, 0x39, 0x93, 0xC6};
    size_t want = tg1c_frame_slots(key);
    if(!want || !out || cap < want) return 0;
    size_t n = 0;

    if(key == TG1C_PAYLOAD_E) {
        uint8_t park[TG1C_WAKE_BYTES];
        tg1c_park_bytes(park);
        return tg1c_build_wake_bytes(park, out, cap);
    }

    if(key == TG1C_PAYLOAD_B) {
        for(int i = 0; i < 32; i++) {
            out[n++] = 0;
            out[n++] = 1;
        }
    } else {
        for(int i = 0; i < 35; i++) {
            out[n++] = 1;
            out[n++] = 0;
        }
    }
    n += tg1c_sync_9t(out + n, cap - n);

    if(key == TG1C_PAYLOAD_A) {
        for(size_t b = 0; b < sizeof(stock_wake); b++)
            n += tg1c_manchester_byte(stock_wake[b], true, out + n, cap - n);
        return n == want ? n : 0;
    }

    uint16_t lf_id = tg1c_payload_lf_id(key);
    n += tg1c_manchester_byte((uint8_t)(lf_id >> 8), false, out + n, cap - n);
    n += tg1c_manchester_byte((uint8_t)(lf_id & 0xFF), false, out + n, cap - n);

    uint8_t pkt[TG1C_CONNECT_PKT_LEN];
    tg1c_connect_packet(pkt);
    bool body_one_first = (key == TG1C_PAYLOAD_D);
    for(size_t b = 0; b < TG1C_CONNECT_PKT_LEN; b++)
        n += tg1c_manchester_byte(pkt[b], body_one_first, out + n, cap - n);
    for(int i = 0; i < TG1C_EOM_SLOTS; i++) out[n++] = 1;
    return n == want ? n : 0;
}
