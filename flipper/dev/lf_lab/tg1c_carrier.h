// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
/* Continental TG1C LF Lab — a decoder-independent sign that a transmission was
 * on air (v0.2.3). Header-only, SDK-free.
 *
 * TG1C telemetry is Manchester at 52 us per chip, so while a sensor transmits
 * every raw interval is one or two chips long: 52 or 104 us. An idle FSK
 * channel gives irregular intervals instead. A run of consecutive intervals
 * that are all within 13 us of 52 or 104 us therefore marks a carrier with
 * data, whether or not the frame decoder accepts what it carries.
 *
 * Measured on real captures (tests/tg1c_carrier.c): the run_0013 captures of
 * transmitting sensors hold 325 to 502 runs of 40 or more each; idle-channel
 * captures hold none, and their longest run is 7.
 */
#include <stdint.h>
#include <stdbool.h>

#define TG1C_CARRIER_CHIP_US 52u
#define TG1C_CARRIER_TOL_US 13u
#define TG1C_CARRIER_MIN_RUN 40u /* intervals; a frame is 100 or more */

typedef struct {
    uint32_t cur; /* length of the run in progress */
    uint32_t max; /* longest run since init */
    uint32_t runs; /* runs that reached TG1C_CARRIER_MIN_RUN, since init */
} TG1cCarrier;

static inline void tg1c_carrier_init(TG1cCarrier* c) {
    c->cur = c->max = c->runs = 0;
}
static inline bool tg1c_carrier_clean(uint32_t dur_us) {
    uint32_t one = TG1C_CARRIER_CHIP_US, two = 2 * TG1C_CARRIER_CHIP_US, tol = TG1C_CARRIER_TOL_US;
    return (dur_us >= one - tol && dur_us <= one + tol) || (dur_us >= two - tol && dur_us <= two + tol);
}
/* Returns true on the interval that makes the run in progress a full run. */
static inline bool tg1c_carrier_feed(TG1cCarrier* c, uint32_t dur_us) {
    if(!tg1c_carrier_clean(dur_us)) {
        c->cur = 0;
        return false;
    }
    c->cur++;
    if(c->cur > c->max) c->max = c->cur;
    if(c->cur != TG1C_CARRIER_MIN_RUN) return false;
    c->runs++;
    return true;
}
/* Lost intervals or a retune: the run in progress cannot continue. */
static inline void tg1c_carrier_break(TG1cCarrier* c) {
    c->cur = 0;
}
