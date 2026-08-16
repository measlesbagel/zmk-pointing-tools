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
 * fractional part stays in the remainder. The units-per-millimetre factor
 * maps millimetres to the output device units (sensor counts or display
 * pixels).
 */

struct zpt_cursor_quantizer_config {
    /* Output units per millimetre, as a Q16 fixed-point factor. */
    zpt_fixed_t units_per_millimeter;
};

struct zpt_cursor_quantizer_state {
    int64_t remainder_x;
    int64_t remainder_y;
};

extern const struct zpt_stage_api zpt_cursor_quantizer_stage_api;
