// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <stdint.h>
#include <stddef.h>
/* One sample per 8 us carrier period. The first duty is loaded by software;
 * DMA samples describe the subsequent periods, ending with an explicit OFF.
 * Returns DMA transfer count, or zero if invalid/capacity insufficient. */
size_t tpms_lf_envelope(const uint8_t* slots, size_t length, uint16_t half_us,
    uint16_t duty, uint16_t* first, uint16_t* output, size_t capacity);
/* Continuous carrier immediately before each unchanged frame; no extra gap.
 * 8 us granularity, bounded to 16 ms. Zero reproduces the original envelope. */
size_t tpms_lf_envelope_lead(const uint8_t* slots, size_t length, uint16_t half_us,
    uint16_t lead_us, uint16_t duty, uint16_t* first, uint16_t* output, size_t capacity);
