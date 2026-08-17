/* SPDX-License-Identifier: MIT */
#pragma once

#include <stdint.h>

#include <zmk/pointing_tools/core/pipeline.h>

/*
 * Cursor quantizer stage over normalized motion.
 *
 * Converts Q16 millimetre motion into integer-valued Q16 pointer deltas,
 * carrying the fractional remainder between frames so sub-pixel movement
 * accumulates exactly. Each frame clamps to the signed 16-bit HID movement
 * range; whole units beyond the range are dropped, while only the
 * fractional part stays in the remainder. The output factor is derived from
 * the frame's resolution-CPI (ZPT_CPI_TO_FIXED_PER_MILLIMETER), so a cursor
 * travels the same physical distance as the ball at its CPI and the
 * cursor-transfer gain scales on top.
 */

struct zpt_cursor_quantizer_state {
    int64_t remainder_x;
    int64_t remainder_y;
};

extern const struct zpt_stage_api zpt_cursor_quantizer_stage_api;
