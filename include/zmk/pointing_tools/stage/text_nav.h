/* SPDX-License-Identifier: MIT */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <zmk/pointing_tools/core/pipeline.h>

/*
 * Text-navigation stage over raw motion.
 *
 * Accumulates a gesture and emits ACTION signals when the engaged axis
 * crosses its threshold, subtracting the threshold from the accumulator so
 * sustained motion repeats. Mirrors the legacy text-navigation processor in
 * the count domain.
 */

enum zpt_text_nav_direction {
    ZPT_TEXT_NAV_NONE = -1,
    ZPT_TEXT_NAV_LEFT = 0,
    ZPT_TEXT_NAV_RIGHT,
    ZPT_TEXT_NAV_UP,
    ZPT_TEXT_NAV_DOWN,
};

struct zpt_text_nav_config {
    int64_t horizontal_threshold;
    int64_t vertical_threshold;
    uint16_t idle_timeout_ms;
    int64_t activation_distance;
    uint16_t engage_ratio_percent;
};

struct zpt_text_nav_state {
    int64_t accumulated_x;
    int64_t accumulated_y;
    uint8_t intent;
    /* Last emitted direction for telemetry and replay observation. */
    int32_t last_direction;
    uint32_t last_frame_ms;
    bool have_last_frame;
};

extern const struct zpt_stage_api zpt_text_nav_stage_api;
