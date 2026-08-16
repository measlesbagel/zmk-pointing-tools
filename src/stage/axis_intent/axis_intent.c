/* SPDX-License-Identifier: MIT */

#include <errno.h>
#include <limits.h>
#include <stddef.h>

#include <zmk/pointing_tools/stage/axis_intent.h>

static uint64_t magnitude(int64_t value) {
    if (value >= 0) {
        return (uint64_t)value;
    }
    /* Avoid overflowing when negating INT64_MIN. */
    return (uint64_t)(-(value + 1)) + 1U;
}

static uint64_t saturating_add_u64(uint64_t left, uint64_t right) {
    return UINT64_MAX - left < right ? UINT64_MAX : left + right;
}

static uint64_t decay(uint64_t value, uint32_t elapsed_ms, uint32_t window_ms) {
    if (elapsed_ms >= window_ms || window_ms == 0U) {
        return 0U;
    }
    uint32_t remaining_ms = window_ms - elapsed_ms;
    if (value > UINT64_MAX / remaining_ms) {
        return UINT64_MAX;
    }
    return (value * remaining_ms) / window_ms;
}

static bool dominates(uint64_t major, uint64_t minor, uint16_t ratio_percent) {
    /* major * 100 >= minor * ratio, evaluated without overflow. */
    if (minor != 0U && ratio_percent != 0U && minor > UINT64_MAX / ratio_percent) {
        return true;
    }
    uint64_t product = minor * ratio_percent;
    uint64_t required = product / 100U + (product % 100U != 0U ? 1U : 0U);
    return major >= required;
}

int zpt_axis_intent_validate(const struct zpt_axis_intent_settings *settings) {
    if (settings == NULL || settings->engage_ratio_percent == 0U ||
        settings->release_ratio_percent == 0U || settings->activation_distance <= 0 ||
        settings->window_ms == 0U) {
        return -EINVAL;
    }
    return 0;
}

void zpt_axis_intent_reset(struct zpt_axis_intent_state *state) {
    if (state != NULL) {
        *state = (struct zpt_axis_intent_state){0};
    }
}

enum zpt_axis_intent zpt_axis_intent_estimate(struct zpt_axis_intent_state *state,
                                              const struct zpt_axis_intent_settings *settings,
                                              enum zpt_axis_policy policy, int64_t x, int64_t y,
                                              uint32_t elapsed_ms) {
    if (state == NULL || zpt_axis_intent_validate(settings) < 0) {
        return ZPT_AXIS_INTENT_FREE;
    }

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

    state->horizontal_energy = saturating_add_u64(
        decay(state->horizontal_energy, elapsed_ms, settings->window_ms), magnitude(x));
    state->vertical_energy = saturating_add_u64(
        decay(state->vertical_energy, elapsed_ms, settings->window_ms), magnitude(y));

    uint64_t total = saturating_add_u64(state->horizontal_energy, state->vertical_energy);
    if (total < (uint64_t)settings->activation_distance) {
        return state->intent;
    }

    bool horizontal_engaged =
        dominates(state->horizontal_energy, state->vertical_energy, settings->engage_ratio_percent);
    bool vertical_engaged =
        dominates(state->vertical_energy, state->horizontal_energy, settings->engage_ratio_percent);

    switch (state->intent) {
    case ZPT_AXIS_INTENT_HORIZONTAL:
        if (dominates(state->horizontal_energy, state->vertical_energy,
                      settings->release_ratio_percent)) {
            break;
        }
        state->intent = vertical_engaged ? ZPT_AXIS_INTENT_VERTICAL : ZPT_AXIS_INTENT_FREE;
        break;
    case ZPT_AXIS_INTENT_VERTICAL:
        if (dominates(state->vertical_energy, state->horizontal_energy,
                      settings->release_ratio_percent)) {
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

uint16_t zpt_axis_intent_confidence(const struct zpt_axis_intent_state *state) {
    if (state == NULL || state->intent == ZPT_AXIS_INTENT_UNDECIDED ||
        state->intent == ZPT_AXIS_INTENT_FREE) {
        return 0U;
    }
    uint64_t major = state->intent == ZPT_AXIS_INTENT_HORIZONTAL ? state->horizontal_energy
                                                                 : state->vertical_energy;
    uint64_t total = saturating_add_u64(state->horizontal_energy, state->vertical_energy);
    if (total == 0U) {
        return 0U;
    }
    if (major > UINT64_MAX / 100U) {
        return 100U;
    }
    return (uint16_t)((major * 100U) / total);
}
