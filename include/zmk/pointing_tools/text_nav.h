/* SPDX-License-Identifier: MIT */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <zmk/pointing_tools/axis_intent.h>

enum zpt_text_nav_direction {
    ZPT_TEXT_NAV_NONE = -1,
    ZPT_TEXT_NAV_LEFT = 0,
    ZPT_TEXT_NAV_RIGHT,
    ZPT_TEXT_NAV_UP,
    ZPT_TEXT_NAV_DOWN,
};

struct zpt_text_nav_settings {
    uint16_t horizontal_threshold;
    uint16_t vertical_threshold;
    uint16_t idle_timeout_ms;
    uint16_t activation_distance;
    uint16_t engage_ratio_percent;
};

struct zpt_text_nav_state {
    int32_t accumulated_x;
    int32_t accumulated_y;
    enum zpt_axis_intent intent;
};

void zpt_text_nav_reset(struct zpt_text_nav_state *state);

enum zpt_text_nav_direction zpt_text_nav_update(struct zpt_text_nav_state *state,
                                                const struct zpt_text_nav_settings *settings,
                                                int32_t x, int32_t y, uint32_t elapsed_ms,
                                                bool continuing_gesture);
