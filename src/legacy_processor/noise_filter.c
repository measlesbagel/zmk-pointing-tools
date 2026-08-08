/* SPDX-License-Identifier: MIT */

#include <limits.h>

#include <zmk/pointing_tools/legacy_processor/noise_filter.h>

static int32_t clamp_add(int32_t left, int32_t right) {
    int64_t result = (int64_t)left + right;
    if (result > INT32_MAX) {
        return INT32_MAX;
    }
    if (result < INT32_MIN) {
        return INT32_MIN;
    }
    return (int32_t)result;
}

static uint64_t saturating_add_u64(uint64_t left, uint64_t right) {
    return UINT64_MAX - left < right ? UINT64_MAX : left + right;
}

static uint64_t saturating_multiply_u64(uint64_t left, uint64_t right) {
    return left != 0U && right > UINT64_MAX / left ? UINT64_MAX : left * right;
}

static uint64_t magnitude_squared(int32_t x, int32_t y) {
    int64_t wide_x = x;
    int64_t wide_y = y;
    uint64_t square_x = (uint64_t)(wide_x * wide_x);
    uint64_t square_y = (uint64_t)(wide_y * wide_y);
    return saturating_add_u64(square_x, square_y);
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
    required = saturating_add_u64(required, (remainder * percent_squared + 9999U) / 10000U);
    return net_squared >= required;
}

static bool has_pending(const struct zpt_noise_filter_state *state) {
    return state->sample_count != 0U;
}

static void reset_pending(struct zpt_noise_filter_state *state) {
    state->pending_x = state->pending_y = 0;
    state->squared_energy = 0U;
    state->sample_count = 0U;
    state->pending_started_ms = 0U;
}

void zpt_noise_filter_reset(struct zpt_noise_filter_state *state) {
    reset_pending(state);
    state->last_frame_ms = 0U;
    state->have_last_frame = false;
    state->active = false;
}

struct zpt_noise_filter_result
zpt_noise_filter_update(struct zpt_noise_filter_state *state,
                        const struct zpt_noise_filter_settings *settings, int32_t x, int32_t y,
                        uint32_t now_ms, bool suppress) {
    struct zpt_noise_filter_result result = {0};

    if (!settings->enabled) {
        zpt_noise_filter_reset(state);
        result.x = x;
        result.y = y;
        result.phase = ZPT_NOISE_FILTER_BYPASS;
        return result;
    }

    if (suppress) {
        result.discarded = has_pending(state) || state->active;
        zpt_noise_filter_reset(state);
        result.phase = ZPT_NOISE_FILTER_IDLE;
        result.suppressed = true;
        return result;
    }

    uint32_t elapsed = state->have_last_frame ? now_ms - state->last_frame_ms : 0U;
    if (state->have_last_frame && elapsed >= settings->idle_timeout_ms) {
        result.discarded = has_pending(state);
        zpt_noise_filter_reset(state);
        result.reset_for_idle = true;
    }

    if (!state->active && has_pending(state) &&
        now_ms - state->pending_started_ms >= settings->qualification_timeout_ms) {
        result.discarded = true;
        reset_pending(state);
        result.reset_for_timeout = true;
    }

    state->last_frame_ms = now_ms;
    state->have_last_frame = true;

    if (state->active) {
        result.x = x;
        result.y = y;
        result.phase = ZPT_NOISE_FILTER_ACTIVE;
        return result;
    }

    if (!has_pending(state)) {
        state->pending_started_ms = now_ms;
    }
    state->pending_x = clamp_add(state->pending_x, x);
    state->pending_y = clamp_add(state->pending_y, y);
    state->squared_energy = saturating_add_u64(state->squared_energy, magnitude_squared(x, y));
    if (state->sample_count != UINT32_MAX) {
        state->sample_count++;
    }

    uint64_t net_squared = magnitude_squared(state->pending_x, state->pending_y);
    uint64_t activation_squared =
        (uint64_t)settings->activation_distance * settings->activation_distance;
    if (net_squared >= activation_squared &&
        coherence_meets(net_squared, state->squared_energy, state->sample_count,
                        settings->coherence_percent)) {
        result.x = state->pending_x;
        result.y = state->pending_y;
        result.phase = ZPT_NOISE_FILTER_ACTIVE;
        result.qualified = true;
        state->active = true;
        reset_pending(state);
        return result;
    }

    result.phase = ZPT_NOISE_FILTER_PENDING;
    return result;
}
