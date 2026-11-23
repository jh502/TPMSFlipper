// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
/* Continental TG1C LF Lab — on-device decode of the NXP bootloader RF reply
 * (plan v3 §4c). Header-only, SDK-free, no allocation: fed one raw interval at
 * a time from the receive session, and compiled unchanged by the host tests.
 *
 * What the sensor would send (FXTH_BtLdr_1/Sources/BtLdr.c):
 *   - NRZ at 19200 bps, 52 us bit cell (PLLCR1 CODE=10; RFCR0=0x19).
 *   - bytes  55 55 02 02 | LEN CMD DATA.. CNTR_HI CNTR_LO | CRC_HI CRC_LO
 *   - CRC-CCITT, poly 0x1021, init 0x1D0F, final ~crc, over LEN..CNTR. Running
 *     the same CRC over LEN..CRC_LO leaves the residue 0xE2F0.
 *
 * Unknowns this unit is written around:
 *   - Which FSK tone the receiver reports as "high": both the pattern and its
 *     complement are matched and CRC-checked.
 *   - Bit order on air: BtLdr.c writes its buffer through a ROM routine named
 *     TPMS_RF_WRITE_DATA_REVERSE, so the order is not settled by the source.
 *     The bit-reversed sync forms are counted separately as "sync-like" hits,
 *     and the bytes after them are assembled LSB first and CRC-checked too
 *     (owner left this open on 2026-11-10; plan v3 had no CRC attempt here).
 *     A CRC-valid reply in either order is a reply, so stop-on-success holds
 *     whichever order the sensor uses.
 *   - Reply length: not taken from LEN. After a sync every byte boundary from 6
 *     to 40 bytes is tested for the residue.
 *   - How the receiver starts: on the run_0013 captures every transmission
 *     begins with a stuck interval and one to three shortened ones before the
 *     cells settle, so the first cells of 55 55 cannot be relied on. The NXP
 *     master's own receiver (MKW01_BtLdr_1 RF_App.c) treats 55 55 as preamble
 *     and matches 02 02 only. From v0.2.4 the sync is the last 22 cells: six
 *     preamble cells and 02 02. Up to v0.2.3 all 32 were required.
 *
 * Timing: the cell count of an interval is (dur + 26) / 52 with no clock
 * recovery, the same rule the shared TG1C telemetry decoder uses. An interval
 * shorter than 26 us is not a bit: it breaks the match (v0.2.2). Up to v0.2.1
 * it counted as one cell, and on the bench on 10 November 2026 runs of 15 to
 * 25 us receiver glitches were read as the alternating bits of 55 55, giving
 * three false sync hits in about ten minutes of listening. A run of k
 * equal bits is resolved while the timing error stays under 26 us / (k x 52 us):
 * +/-5 % for runs up to 9 bits, about +/-2.7 % for the 18-bit run in a CONNECT
 * reply whose counter is 0000.
 *
 * Sync quality (v0.2.4): the 22 sync cells must each come from an interval on
 * the 52 us grid, within 13 us plus 5 % of a whole number of cells (25 us at
 * most). Data cells after the sync are still taken by rounding alone. On 62 s
 * of real receiver data (run_0013 and the bench noise captures) this gives no
 * false sync; without the grid rule the 22-cell sync gives one, and a 16-cell
 * sync (02 02 alone) gives 68.
 */
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>

#define TG1C_BL_BIT_US 52u
#define TG1C_BL_MIN_US (TG1C_BL_BIT_US / 2) /* a shorter interval is not a bit cell */
#define TG1C_BL_MAX_RUN 40u /* a longer run flushes the matcher */
#define TG1C_BL_MIN_BYTES 6u
#define TG1C_BL_MAX_BYTES 40u
#define TG1C_BL_RESIDUE 0xE2F0u

#define TG1C_BL_SYNC_BITS 22u /* cells that must match: six of the preamble, then 02 02 */
#define TG1C_BL_SYNC_MASK ((1u << TG1C_BL_SYNC_BITS) - 1u)
#define TG1C_BL_GRID_US 13u /* plus 5 % of the interval, 25 us at most */

#define TG1C_BL_SYNC 0x55550202u /* 55 55 02 02 */
#define TG1C_BL_SYNC_INV 0xAAAAFDFDu /* complement */
#define TG1C_BL_SYNC_REV 0xAAAA4040u /* each byte bit-reversed */
#define TG1C_BL_SYNC_REV_INV 0x5555BFBFu /* bit-reversed, complemented */

/* Bit cells in one interval. 0 means the run is longer than TG1C_BL_MAX_RUN.
 * An interval under TG1C_BL_MIN_US returns 1 here; tg1c_btldr_rx_feed never
 * asks, because it treats such an interval as a break. */
static inline uint32_t tg1c_nrz_cells(uint32_t dur_us) {
    if(dur_us > TG1C_BL_MAX_RUN * TG1C_BL_BIT_US + TG1C_BL_BIT_US / 2 - 1) return 0;
    uint32_t n = (dur_us + TG1C_BL_BIT_US / 2) / TG1C_BL_BIT_US;
    return n ? n : 1;
}

/* True when an interval is a whole number of bit cells (1 to 40) to within
 * 13 us plus 5 %, 25 us at most. A sync is made only of such intervals. */
static inline bool tg1c_nrz_on_grid(uint32_t dur_us) {
    if(dur_us < TG1C_BL_MIN_US || dur_us > TG1C_BL_MAX_RUN * TG1C_BL_BIT_US + TG1C_BL_BIT_US / 2 - 1)
        return false;
    uint32_t ideal = ((dur_us + TG1C_BL_BIT_US / 2) / TG1C_BL_BIT_US) * TG1C_BL_BIT_US;
    uint32_t off = dur_us > ideal ? dur_us - ideal : ideal - dur_us;
    uint32_t tol = TG1C_BL_GRID_US + ideal / 20;
    if(tol > TG1C_BL_BIT_US / 2 - 1) tol = TG1C_BL_BIT_US / 2 - 1;
    return off <= tol;
}

typedef enum {
    TG1C_BL_VAR_NONE = 0,
    TG1C_BL_VAR_NORMAL, /* 55 55 02 02 */
    TG1C_BL_VAR_INVERTED, /* AA AA FD FD */
    TG1C_BL_VAR_REVERSED, /* AA AA 40 40 (sync-like) */
    TG1C_BL_VAR_REVERSED_INVERTED, /* 55 55 BF BF (sync-like) */
} TG1cBtldrVariant;

static inline const char* tg1c_btldr_variant_str(uint8_t v) {
    switch(v) {
    case TG1C_BL_VAR_NORMAL: return "55550202";
    case TG1C_BL_VAR_INVERTED: return "AAAAFDFD";
    case TG1C_BL_VAR_REVERSED: return "AAAA4040";
    case TG1C_BL_VAR_REVERSED_INVERTED: return "5555BFBF";
    default: return "none";
    }
}

/* Events returned by tg1c_btldr_rx_feed (bit mask). */
#define TG1C_BL_EV_SYNC 1u /* 55550202 or its complement was matched */
#define TG1C_BL_EV_SYNC_LIKE 2u /* a bit-reversed sync form was matched */
#define TG1C_BL_EV_REPLY 4u /* a CRC-valid reply completed (either bit order) */

typedef struct {
    uint32_t reg; /* last 32 bit cells, newest in bit 0 */
    uint8_t reg_bits; /* how many of them are real, saturating at 32 */
    uint8_t clean_bits; /* how many of the newest cells came from on-grid intervals */
    bool cell_clean; /* the interval being fed is on the grid */

    bool collecting; /* gathering bytes after a sync */
    bool invert; /* the matched sync was a complemented form */
    bool reverse; /* the matched sync was a bit-reversed form: bytes LSB first */
    uint8_t acc, acc_bits; /* byte being assembled */
    uint8_t buf[TG1C_BL_MAX_BYTES];
    uint8_t len;
    uint16_t crc; /* running CRC before the final inversion */

    uint32_t sync_hits; /* 55550202 or AAAAFDFD */
    uint32_t sync_like_hits; /* AAAA4040 or 5555BFBF */
    uint8_t last_variant; /* variant of the most recent hit of either kind */

    bool reply_valid; /* the first CRC-valid reply; later ones do not replace it */
    uint8_t reply_variant;
    uint8_t reply_len;
    uint8_t reply[TG1C_BL_MAX_BYTES];
} TG1cBtldrRx;

static inline void tg1c_btldr_rx_init(TG1cBtldrRx* rx) {
    memset(rx, 0, sizeof(*rx));
}

/* The interval stream was interrupted (lost intervals, retune, new burst):
 * forget the partial match. Counters and a stored reply are kept. */
static inline void tg1c_btldr_rx_break(TG1cBtldrRx* rx) {
    rx->reg = 0;
    rx->reg_bits = 0;
    rx->clean_bits = 0;
    rx->collecting = false;
    rx->acc = rx->acc_bits = 0;
    rx->len = 0;
}

/* CMD byte and the first four data bytes of the stored reply. For a CONNECT
 * reply (CMD 01) the data bytes are the sensor's 32-bit ID. */
static inline uint8_t tg1c_btldr_reply_cmd(const TG1cBtldrRx* rx) {
    return (rx->reply_valid && rx->reply_len >= 2) ? rx->reply[1] : 0;
}
static inline uint32_t tg1c_btldr_reply_data4(const TG1cBtldrRx* rx) {
    if(!rx->reply_valid || rx->reply_len < 6) return 0;
    return ((uint32_t)rx->reply[2] << 24) | ((uint32_t)rx->reply[3] << 16) |
           ((uint32_t)rx->reply[4] << 8) | rx->reply[5];
}

static inline unsigned tg1c_btldr_rx_bit(TG1cBtldrRx* rx, bool bit) {
    if(rx->collecting) {
        uint8_t v = (bit != rx->invert) ? 1 : 0;
        rx->acc = rx->reverse ? (uint8_t)((rx->acc >> 1) | (v << 7)) :
                                (uint8_t)((rx->acc << 1) | v);
        if(++rx->acc_bits < 8) return 0;
        uint8_t byte = rx->acc;
        rx->acc = rx->acc_bits = 0;
        rx->buf[rx->len++] = byte;
        rx->crc ^= (uint16_t)byte << 8;
        for(int b = 0; b < 8; b++)
            rx->crc = (rx->crc & 0x8000) ? (uint16_t)((rx->crc << 1) ^ 0x1021) :
                                           (uint16_t)(rx->crc << 1);
        unsigned ev = 0;
        if(rx->len >= TG1C_BL_MIN_BYTES && (uint16_t)~rx->crc == TG1C_BL_RESIDUE) {
            if(!rx->reply_valid) {
                rx->reply_valid = true;
                rx->reply_variant = rx->last_variant;
                rx->reply_len = rx->len;
                memcpy(rx->reply, rx->buf, rx->len);
            }
            ev = TG1C_BL_EV_REPLY;
            tg1c_btldr_rx_break(rx);
        } else if(rx->len >= TG1C_BL_MAX_BYTES) {
            tg1c_btldr_rx_break(rx); /* sync without a valid CRC */
        }
        return ev;
    }

    rx->reg = (rx->reg << 1) | (bit ? 1u : 0u);
    if(rx->reg_bits < 32) rx->reg_bits++;
    if(!rx->cell_clean)
        rx->clean_bits = 0;
    else if(rx->clean_bits < 32)
        rx->clean_bits++;
    if(rx->reg_bits < TG1C_BL_SYNC_BITS || rx->clean_bits < TG1C_BL_SYNC_BITS) return 0;
    unsigned ev;
    uint32_t tail = rx->reg & TG1C_BL_SYNC_MASK;
    if(tail == (TG1C_BL_SYNC & TG1C_BL_SYNC_MASK) || tail == (TG1C_BL_SYNC_INV & TG1C_BL_SYNC_MASK)) {
        rx->invert = (tail == (TG1C_BL_SYNC_INV & TG1C_BL_SYNC_MASK));
        rx->reverse = false;
        rx->last_variant = rx->invert ? TG1C_BL_VAR_INVERTED : TG1C_BL_VAR_NORMAL;
        rx->sync_hits++;
        ev = TG1C_BL_EV_SYNC;
    } else if(tail == (TG1C_BL_SYNC_REV & TG1C_BL_SYNC_MASK) ||
              tail == (TG1C_BL_SYNC_REV_INV & TG1C_BL_SYNC_MASK)) {
        rx->invert = (tail == (TG1C_BL_SYNC_REV_INV & TG1C_BL_SYNC_MASK));
        rx->reverse = true;
        rx->last_variant = rx->invert ? TG1C_BL_VAR_REVERSED_INVERTED : TG1C_BL_VAR_REVERSED;
        rx->sync_like_hits++;
        ev = TG1C_BL_EV_SYNC_LIKE;
    } else {
        return 0;
    }
    rx->collecting = true;
    rx->acc = rx->acc_bits = 0;
    rx->len = 0;
    rx->crc = 0x1D0F;
    return ev;
}

/* Feed one raw interval: level and duration in microseconds. Returns the
 * events it produced. */
static inline unsigned tg1c_btldr_rx_feed(TG1cBtldrRx* rx, bool level, uint32_t dur_us) {
    if(dur_us < TG1C_BL_MIN_US) {
        /* Noise, not a bit cell. A reply it interrupts could not pass its CRC
         * with a bit inserted either, so nothing valid is lost. */
        tg1c_btldr_rx_break(rx);
        return 0;
    }
    uint32_t n = tg1c_nrz_cells(dur_us);
    unsigned ev = 0;
    if(n == 0) {
        /* The line stayed at one level for more than 40 cells: the signal has
         * ended. If a byte is part-collected its remaining bits are this
         * level, so finish that one byte (a reply whose last bits match the
         * idle level is otherwise lost), then flush. The register is left
         * holding 32 cells of this level, which matches no sync form. */
        if(rx->collecting && rx->acc_bits) {
            while(rx->collecting && rx->acc_bits) ev |= tg1c_btldr_rx_bit(rx, level);
        }
        rx->collecting = false;
        rx->acc = rx->acc_bits = 0;
        rx->len = 0;
        rx->reg = level ? 0xFFFFFFFFu : 0u;
        rx->reg_bits = 32;
        rx->clean_bits = 0; /* an idle run is not part of a sync */
        return ev;
    }
    rx->cell_clean = tg1c_nrz_on_grid(dur_us);
    while(n--) ev |= tg1c_btldr_rx_bit(rx, level);
    return ev;
}
