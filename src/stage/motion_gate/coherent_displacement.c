/* SPDX-License-Identifier: MIT */

#include <errno.h>
#include <limits.h>
#include <stddef.h>

#include <zmk/pointing_tools/core/fixed.h>
#include <zmk/pointing_tools/stage/motion_gate/coherent_displacement.h>

static int64_t saturating_add_clipped(int64_t left, int64_t right, bool *clipped) {
    if (right > 0 && left > INT64_MAX - right) {
        *clipped = true;
        return INT64_MAX;
    }
    if (right < 0 && left < INT64_MIN - right) {
        *clipped = true;
        return INT64_MIN;
    }
    return left + right;
}

static uint64_t saturating_multiply_u64(uint64_t left, uint64_t right) {
    return left != 0U && right > UINT64_MAX / left ? UINT64_MAX : left * right;
}

static uint64_t square_saturated(int64_t value) {
    uint64_t magnitude = zpt_fixed_magnitude(value);
    return magnitude > UINT32_MAX ? UINT64_MAX : magnitude * magnitude;
}

static uint64_t magnitude_squared(int64_t x, int64_t y) {
    return zpt_fixed_saturating_add_u64(square_saturated(x), square_saturated(y));
}

static bool coherence_meets(uint64_t net_squared, uint64_t squared_energy, uint32_t sample_count,
                            uint16_t percent) {
    if (percent == 0U || sample_count <= 1U) {
        return true;
    }

    uint64_t denominator = saturating_multiply_u64(sample_count, squared_energy);
    if (denominator == 0U) {
        return false;
    }

    uint64_t percent_squared = (uint64_t)percent * percent;
    uint64_t required = (denominator / 10000U) * percent_squared;
    uint64_t remainder = denominator % 10000U;
    required =
        zpt_fixed_saturating_add_u64(required, (remainder * percent_squared + 9999U) / 10000U);
    return net_squared >= required;
}

static bool has_pending(const struct zpt_coherent_displacement_state *state) {
    return state->sample_count != 0U;
}

static void reset_pending(struct zpt_coherent_displacement_state *state) {
    state->pending_x = 0;
    state->pending_y = 0;
    state->squared_energy = 0U;
    state->sample_count = 0U;
    state->pending_started_ms = 0U;
    state->pending_clipped = false;
}

int zpt_coherent_displacement_validate(const struct zpt_coherent_displacement_settings *settings) {
    if (settings == NULL || settings->activation_distance <= 0 ||
        settings->coherence_percent > 100U || settings->qualification_timeout_ms == 0U ||
        settings->idle_timeout_ms == 0U) {
        return -EINVAL;
    }
    return 0;
}

void zpt_coherent_displacement_reset(struct zpt_coherent_displacement_state *state) {
    if (state != NULL) {
        *state = (struct zpt_coherent_displacement_state){0};
    }
}

static struct zpt_coherent_displacement_result
expire_state(struct zpt_coherent_displacement_state *state,
             const struct zpt_coherent_displacement_settings *settings, uint32_t now_ms) {
    struct zpt_coherent_displacement_result result = {
        .phase = state->active        ? ZPT_MOTION_GATE_ACTIVE
                 : has_pending(state) ? ZPT_MOTION_GATE_PENDING
                                      : ZPT_MOTION_GATE_IDLE,
    };

    if (state->have_last_frame && now_ms - state->last_frame_ms >= settings->idle_timeout_ms) {
        result.discarded = has_pending(state);
        zpt_coherent_displacement_reset(state);
        result.phase = ZPT_MOTION_GATE_IDLE;
        result.reset_for_idle = true;
        return result;
    }

    if (!state->active && has_pending(state) &&
        now_ms - state->pending_started_ms >= settings->qualification_timeout_ms) {
        result.discarded = true;
        reset_pending(state);
        result.phase = ZPT_MOTION_GATE_IDLE;
        result.reset_for_timeout = true;
    }
    return result;
}

struct zpt_coherent_displacement_result
zpt_coherent_displacement_expire(struct zpt_coherent_displacement_state *state,
                                 const struct zpt_coherent_displacement_settings *settings,
                                 uint32_t now_ms) {
    if (state == NULL || zpt_coherent_displacement_validate(settings) < 0 || !settings->enabled) {
        return (struct zpt_coherent_displacement_result){.phase = ZPT_MOTION_GATE_BYPASS};
    }
    return expire_state(state, settings, now_ms);
}

struct zpt_coherent_displacement_result
zpt_coherent_displacement_update(struct zpt_coherent_displacement_state *state,
                                 const struct zpt_coherent_displacement_settings *settings,
                                 int64_t x, int64_t y, uint32_t now_ms, bool suppress) {
    struct zpt_coherent_displacement_result result = {0};
    if (state == NULL || zpt_coherent_displacement_validate(settings) < 0) {
        return result;
    }

    if (!settings->enabled) {
        zpt_coherent_displacement_reset(state);
        result.x = x;
        result.y = y;
        result.phase = ZPT_MOTION_GATE_BYPASS;
        return result;
    }

    if (suppress) {
        result.discarded = has_pending(state) || state->active;
        zpt_coherent_displacement_reset(state);
        result.phase = ZPT_MOTION_GATE_IDLE;
        result.suppressed = true;
        return result;
    }

    result = expire_state(state, settings, now_ms);
    bool reset_for_idle = result.reset_for_idle;
    bool reset_for_timeout = result.reset_for_timeout;
    bool discarded = result.discarded;

    state->last_frame_ms = now_ms;
    state->have_last_frame = true;

    if (state->active) {
        result = (struct zpt_coherent_displacement_result){
            .x = x,
            .y = y,
            .phase = ZPT_MOTION_GATE_ACTIVE,
        };
        return result;
    }

    if (!has_pending(state)) {
        state->pending_started_ms = now_ms;
    }
    bool clipped = false;
    state->pending_x = saturating_add_clipped(state->pending_x, x, &clipped);
    state->pending_y = saturating_add_clipped(state->pending_y, y, &clipped);
    state->pending_clipped |= clipped;
    state->squared_energy =
        zpt_fixed_saturating_add_u64(state->squared_energy, magnitude_squared(x, y));
    if (state->sample_count != UINT32_MAX) {
        state->sample_count++;
    }

    uint64_t net_squared = magnitude_squared(state->pending_x, state->pending_y);
    uint64_t activation_squared = square_saturated(settings->activation_distance);
    if (net_squared >= activation_squared &&
        coherence_meets(net_squared, state->squared_energy, state->sample_count,
                        settings->coherence_percent)) {
        result = (struct zpt_coherent_displacement_result){
            .x = state->pending_x,
            .y = state->pending_y,
            .phase = ZPT_MOTION_GATE_ACTIVE,
            .reset_for_idle = reset_for_idle,
            .reset_for_timeout = reset_for_timeout,
            .qualified = true,
            .discarded = discarded,
            .clipped = state->pending_clipped,
        };
        state->active = true;
        reset_pending(state);
        return result;
    }

    result = (struct zpt_coherent_displacement_result){
        .phase = ZPT_MOTION_GATE_PENDING,
        .reset_for_idle = reset_for_idle,
        .reset_for_timeout = reset_for_timeout,
        .discarded = discarded,
        .clipped = state->pending_clipped,
    };
    return result;
}
