/* SPDX-License-Identifier: MIT */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <zmk/pointing_tools/axis_intent.h>

struct zpt_scroll_settings {
    uint16_t scale_multiplier;
    uint16_t scale_divisor;
    uint16_t report_interval_ms;
    uint16_t idle_timeout_ms;
    uint16_t suppress_after_keypress_ms;
    bool discard_unclassified;
    struct zpt_axis_intent_config intent;
};

struct zpt_scroll_state {
    struct zpt_axis_intent_state intent;
    enum zpt_axis_policy policy;
    int32_t undecided_x;
    int32_t undecided_y;
    int32_t pending_x;
    int32_t pending_y;
    int32_t remainder_x;
    int32_t remainder_y;
    uint32_t last_frame_ms;
    bool have_last_frame;
};

struct zpt_scroll_decision {
    enum zpt_axis_intent previous_intent;
    enum zpt_axis_intent intent;
    bool reset_for_idle;
    bool suppressed;
};

struct zpt_scroll_flush_decision {
    int32_t undecided_x;
    int32_t undecided_y;
    bool discarded;
    bool clipped_horizontal;
    bool clipped_vertical;
};

void zpt_scroll_reset(struct zpt_scroll_state *state, bool clear_remainder);

struct zpt_scroll_decision zpt_scroll_process(struct zpt_scroll_state *state,
                                              const struct zpt_scroll_settings *settings,
                                              int32_t x, int32_t y,
                                              enum zpt_axis_policy policy, uint32_t now_ms,
                                              bool suppress);

bool zpt_scroll_flush(struct zpt_scroll_state *state,
                      const struct zpt_scroll_settings *settings, int16_t *horizontal,
                      int16_t *vertical, struct zpt_scroll_flush_decision *decision);
