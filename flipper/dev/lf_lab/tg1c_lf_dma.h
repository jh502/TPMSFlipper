// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
/* Forked from tpms_bridge/tpms_lf_dma with ONE change to what was there: the
 * slot count is an explicit parameter instead of the fixed 184-slot LAB_SLOTS
 * (and, from v0.3.1, one addition: tg1c_lf_dma_reload). The CONNECT
 * frames are 260 and 266 slots (tg1c_frame.h), so the envelope buffer and the
 * heap guard are computed from the real count. Everything else — TIM1 CCR1 via
 * DMA2 ch1 at 8 us resolution, carrier 125 kHz — is identical to the shipped
 * engine. See README for the divergence note. */

/* Largest frame this app transmits: 266 slots (frames C and D). The app asserts
 * this equals TG1C_MAX_FRAME_SLOTS. */
#define TG1C_MAX_SLOTS 266

bool tg1c_lf_dma_prepare(const uint8_t* slots, size_t nslots, uint16_t half_us, uint16_t lead_us);
/* v0.3.1: rewrite the prepared envelope in place with another frame that has
 * the same slot count, half-bit and lead-in. No allocation. Only while no
 * frame is being sent. False (and a failure text) if nothing is prepared, a
 * frame is on the air, or the sizes differ; the old envelope is then intact. */
bool tg1c_lf_dma_reload(const uint8_t* slots, size_t nslots, uint16_t half_us, uint16_t lead_us);
const char* tg1c_lf_dma_failure(void);
bool tg1c_lf_dma_start(void);
bool tg1c_lf_dma_done(void);
bool tg1c_lf_dma_error(void);
void tg1c_lf_dma_stop(void);
void tg1c_lf_dma_free(void);
/* DMA transfer count of the prepared envelope (0 when none); for logging. */
size_t tg1c_lf_dma_count(void);
