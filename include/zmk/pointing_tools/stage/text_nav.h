/* SPDX-License-Identifier: MIT */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <zmk/pointing_tools/core/pipeline.h>

/*
 * Text-navigation mapper over normalized motion.
 *
 * Consumes motion that already carries a decided axis-intent annotation
 * (produced by the shared axis-intent estimator upstream), accumulates only
 * the engaged axis, and emits ACTION signals when that axis crosses its
 * threshold, subtracting the threshold so sustained motion repeats. The idle
 * timeout resets the accumulator at gesture boundaries. Thresholds are
 * signed Q16 millimetres.
 */

enum zpt_text_nav_direction {
    ZPT_TEXT_NAV_NONE = -1,
    ZPT_TEXT_NAV_LEFT = 0,
    ZPT_TEXT_NAV_RIGHT,
    ZPT_TEXT_NAV_UP,
    ZPT_TEXT_NAV_DOWN,
};

struct zpt_text_nav_config {
    /* Q16 millimetre gesture thresholds. */
    int64_t horizontal_threshold;
    int64_t vertical_threshold;
    uint16_t idle_timeout_ms;
};

struct zpt_text_nav_state {
    int64_t accumulated_x;
    int64_t accumulated_y;
    /* Last emitted direction for telemetry and replay observation. */
    int32_t last_direction;
    uint32_t last_frame_ms;
    bool have_last_frame;
};

extern const struct zpt_stage_api zpt_text_nav_stage_api;
