/* SPDX-License-Identifier: MIT */

#include <limits.h>
#include <stddef.h>

#include <zmk/pointing_tools/axis_intent.h>

static uint32_t magnitude(int32_t value) {
    if (value >= 0) {
        return (uint32_t)value;
    }

    /* Avoid overflowing when negating INT32_MIN. */
    return (uint32_t)(-(value + 1)) + 1U;
}

static uint32_t decay(uint32_t value, uint32_t elapsed_ms, uint32_t window_ms) {
    if (elapsed_ms >= window_ms || window_ms == 0U) {
        return 0U;
    }

    return (uint32_t)(((uint64_t)value * (window_ms - elapsed_ms)) / window_ms);
}

static bool dominates(uint32_t major, uint32_t minor, uint16_t ratio_percent) {
    return (uint64_t)major * 100U >= (uint64_t)minor * ratio_percent;
}

void zpt_axis_intent_reset(struct zpt_axis_intent_state *state) {
    state->horizontal_energy = 0U;
    state->vertical_energy = 0U;
    state->intent = ZPT_AXIS_INTENT_UNDECIDED;
}

enum zpt_axis_intent zpt_axis_intent_update(struct zpt_axis_intent_state *state,
                                            const struct zpt_axis_intent_config *config,
                                            enum zpt_axis_policy policy, int32_t x, int32_t y,
                                            uint32_t elapsed_ms) {
    switch (policy) {
    case ZPT_AXIS_POLICY_FREE:
        state->intent = ZPT_AXIS_INTENT_FREE;
        return state->intent;
    case ZPT_AXIS_POLICY_HORIZONTAL:
        state->intent = ZPT_AXIS_INTENT_HORIZONTAL;
        return state->intent;
    case ZPT_AXIS_POLICY_VERTICAL:
        state->intent = ZPT_AXIS_INTENT_VERTICAL;
        return state->intent;
    case ZPT_AXIS_POLICY_ADAPTIVE:
        break;
    default:
        state->intent = ZPT_AXIS_INTENT_FREE;
        return state->intent;
    }

    state->horizontal_energy =
        decay(state->horizontal_energy, elapsed_ms, config->window_ms) + magnitude(x);
    state->vertical_energy =
        decay(state->vertical_energy, elapsed_ms, config->window_ms) + magnitude(y);

    uint32_t total = state->horizontal_energy + state->vertical_energy;
    if (total < config->activation_distance) {
        return state->intent;
    }

    bool horizontal_engaged =
        dominates(state->horizontal_energy, state->vertical_energy, config->engage_ratio_percent);
    bool vertical_engaged =
        dominates(state->vertical_energy, state->horizontal_energy, config->engage_ratio_percent);

    switch (state->intent) {
    case ZPT_AXIS_INTENT_HORIZONTAL:
        if (dominates(state->horizontal_energy, state->vertical_energy,
                      config->release_ratio_percent)) {
            break;
        }
        state->intent = vertical_engaged ? ZPT_AXIS_INTENT_VERTICAL : ZPT_AXIS_INTENT_FREE;
        break;
    case ZPT_AXIS_INTENT_VERTICAL:
        if (dominates(state->vertical_energy, state->horizontal_energy,
                      config->release_ratio_percent)) {
            break;
        }
        state->intent = horizontal_engaged ? ZPT_AXIS_INTENT_HORIZONTAL : ZPT_AXIS_INTENT_FREE;
        break;
    case ZPT_AXIS_INTENT_UNDECIDED:
    case ZPT_AXIS_INTENT_FREE:
        if (horizontal_engaged) {
            state->intent = ZPT_AXIS_INTENT_HORIZONTAL;
        } else if (vertical_engaged) {
            state->intent = ZPT_AXIS_INTENT_VERTICAL;
        } else {
            state->intent = ZPT_AXIS_INTENT_FREE;
        }
        break;
    }

    return state->intent;
}
