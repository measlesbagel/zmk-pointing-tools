/* SPDX-License-Identifier: MIT */

#include <limits.h>

#include <zmk/pointing_tools/scroll.h>

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

static void accumulate_filtered(struct zpt_scroll_state *state, enum zpt_axis_intent intent,
                                int32_t x, int32_t y) {
    if (intent != ZPT_AXIS_INTENT_VERTICAL) {
        state->pending_x = clamp_add(state->pending_x, x);
    }
    if (intent != ZPT_AXIS_INTENT_HORIZONTAL) {
        state->pending_y = clamp_add(state->pending_y, y);
    }
}

static int16_t take_scaled(int32_t *pending, int32_t *remainder, uint16_t multiplier,
                           uint16_t divisor) {
    int64_t numerator = (int64_t)*pending * multiplier + *remainder;
    int64_t scaled = numerator / divisor;
    int16_t output =
        scaled > INT16_MAX ? INT16_MAX : (scaled < INT16_MIN ? INT16_MIN : (int16_t)scaled);

    /* Keep both fractional and HID-range overflow for a later report. */
    int64_t remaining = numerator - ((int64_t)output * divisor);
    if (remaining > INT32_MAX) {
        *remainder = INT32_MAX;
    } else if (remaining < INT32_MIN) {
        *remainder = INT32_MIN;
    } else {
        *remainder = (int32_t)remaining;
    }
    *pending = 0;
    return output;
}

void zpt_scroll_reset(struct zpt_scroll_state *state, bool clear_remainder) {
    zpt_axis_intent_reset(&state->intent);
    state->undecided_x = state->undecided_y = 0;
    state->pending_x = state->pending_y = 0;
    if (clear_remainder) {
        state->remainder_x = state->remainder_y = 0;
    }
    state->have_last_frame = false;
}

struct zpt_scroll_decision zpt_scroll_process(struct zpt_scroll_state *state,
                                              const struct zpt_scroll_settings *settings,
                                              int32_t x, int32_t y,
                                              enum zpt_axis_policy policy, uint32_t now_ms,
                                              bool suppress) {
    struct zpt_scroll_decision decision = {
        .previous_intent = state->intent.intent,
        .intent = state->intent.intent,
    };

    if (suppress) {
        zpt_scroll_reset(state, false);
        decision.intent = state->intent.intent;
        decision.suppressed = true;
        return decision;
    }

    uint32_t elapsed = state->have_last_frame ? now_ms - state->last_frame_ms : 0U;
    if (!state->have_last_frame || elapsed >= settings->idle_timeout_ms || policy != state->policy) {
        zpt_axis_intent_reset(&state->intent);
        state->undecided_x = state->undecided_y = 0;
        decision.reset_for_idle = true;
    }

    state->policy = policy;
    state->last_frame_ms = now_ms;
    state->have_last_frame = true;

    decision.previous_intent = state->intent.intent;
    enum zpt_axis_intent intent =
        zpt_axis_intent_update(&state->intent, &settings->intent, policy, x, y, elapsed);
    decision.intent = intent;

    if (intent == ZPT_AXIS_INTENT_UNDECIDED) {
        state->undecided_x = clamp_add(state->undecided_x, x);
        state->undecided_y = clamp_add(state->undecided_y, y);
    } else {
        if (decision.previous_intent == ZPT_AXIS_INTENT_UNDECIDED) {
            state->undecided_x = clamp_add(state->undecided_x, x);
            state->undecided_y = clamp_add(state->undecided_y, y);
            accumulate_filtered(state, intent, state->undecided_x, state->undecided_y);
            state->undecided_x = state->undecided_y = 0;
        } else {
            accumulate_filtered(state, intent, x, y);
        }
    }

    return decision;
}

bool zpt_scroll_flush(struct zpt_scroll_state *state,
                      const struct zpt_scroll_settings *settings, int16_t *horizontal,
                      int16_t *vertical) {
    if (state->undecided_x != 0 || state->undecided_y != 0) {
        if (!settings->discard_unclassified) {
            accumulate_filtered(state, ZPT_AXIS_INTENT_FREE, state->undecided_x,
                                state->undecided_y);
        }
        state->undecided_x = state->undecided_y = 0;
    }

    *horizontal = take_scaled(&state->pending_x, &state->remainder_x,
                              settings->scale_multiplier, settings->scale_divisor);
    *vertical = take_scaled(&state->pending_y, &state->remainder_y, settings->scale_multiplier,
                            settings->scale_divisor);
    return *horizontal != 0 || *vertical != 0;
}
