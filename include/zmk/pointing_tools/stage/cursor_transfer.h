/* SPDX-License-Identifier: MIT */
#pragma once

#include <stdint.h>

#include <zmk/pointing_tools/core/pipeline.h>

/*
 * Cursor transfer stage over normalized motion.
 *
 * Applies a base gain (scale multiplier and divisor) to Q16 millimetre
 * motion, saturating at the fixed-point limits and flagging clipped output
 * so downstream telemetry can report sensor or HID range overflow.
 */

struct zpt_cursor_transfer_config {
    uint16_t scale_multiplier;
    uint16_t scale_divisor;
};

extern const struct zpt_stage_api zpt_cursor_transfer_stage_api;
