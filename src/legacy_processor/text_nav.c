/* SPDX-License-Identifier: MIT */

#include <limits.h>

#include <zmk/pointing_tools/legacy_processor/text_nav.h>

static int32_t clamp_add(int32_t lhs, int32_t rhs) {
    int64_t result = (int64_t)lhs + rhs;
    if (result > INT32_MAX) {
        return INT32_MAX;
    }
    if (result < INT32_MIN) {
        return INT32_MIN;
    }
    return (int32_t)result;
}

static uint32_t magnitude(int32_t value) {
    return value >= 0 ? (uint32_t)value : (uint32_t)(-(value + 1)) + 1U;
}

static bool dominates(uint32_t major, uint32_t minor, uint16_t ratio_percent) {
    return (uint64_t)major * 100U >= (uint64_t)minor * ratio_percent;
}

void zpt_text_nav_reset(struct zpt_text_nav_state *state) {
    state->accumulated_x = state->accumulated_y = 0;
    state->intent = ZPT_AXIS_INTENT_UNDECIDED;
}

enum zpt_text_nav_direction zpt_text_nav_update(struct zpt_text_nav_state *state,
                                                const struct zpt_text_nav_settings *settings,
                                                int32_t x, int32_t y, uint32_t elapsed_ms,
                                                bool continuing_gesture) {
    if (!continuing_gesture || elapsed_ms >= settings->idle_timeout_ms) {
        zpt_text_nav_reset(state);
    }

    if (state->intent == ZPT_AXIS_INTENT_UNDECIDED) {
        state->accumulated_x = clamp_add(state->accumulated_x, x);
        state->accumulated_y = clamp_add(state->accumulated_y, y);

        uint32_t horizontal = magnitude(state->accumulated_x);
        uint32_t vertical = magnitude(state->accumulated_y);
        if (horizontal + vertical < settings->activation_distance) {
            return ZPT_TEXT_NAV_NONE;
        }

        if (dominates(horizontal, vertical, settings->engage_ratio_percent)) {
            state->intent = ZPT_AXIS_INTENT_HORIZONTAL;
            state->accumulated_y = 0;
        } else if (dominates(vertical, horizontal, settings->engage_ratio_percent)) {
            state->intent = ZPT_AXIS_INTENT_VERTICAL;
            state->accumulated_x = 0;
        } else {
            return ZPT_TEXT_NAV_NONE;
        }
    } else if (state->intent == ZPT_AXIS_INTENT_HORIZONTAL) {
        state->accumulated_x = clamp_add(state->accumulated_x, x);
    } else {
        state->accumulated_y = clamp_add(state->accumulated_y, y);
    }

    int32_t *movement;
    uint32_t threshold;
    enum zpt_text_nav_direction negative_direction;
    enum zpt_text_nav_direction positive_direction;

    if (state->intent == ZPT_AXIS_INTENT_HORIZONTAL) {
        movement = &state->accumulated_x;
        threshold = settings->horizontal_threshold;
        negative_direction = ZPT_TEXT_NAV_LEFT;
        positive_direction = ZPT_TEXT_NAV_RIGHT;
    } else {
        movement = &state->accumulated_y;
        threshold = settings->vertical_threshold;
        negative_direction = ZPT_TEXT_NAV_UP;
        positive_direction = ZPT_TEXT_NAV_DOWN;
    }

    if (magnitude(*movement) < threshold) {
        return ZPT_TEXT_NAV_NONE;
    }

    enum zpt_text_nav_direction direction = *movement < 0 ? negative_direction : positive_direction;
    *movement += *movement < 0 ? (int32_t)threshold : -(int32_t)threshold;
    return direction;
}
