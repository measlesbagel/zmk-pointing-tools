/* SPDX-License-Identifier: MIT */

#include <limits.h>
#include <stddef.h>

#include <zmk/pointing_tools/legacy_processor/noise_filter.h>

static int32_t clamp_i64_to_i32(int64_t value) {
    if (value > INT32_MAX) {
        return INT32_MAX;
    }
    if (value < INT32_MIN) {
        return INT32_MIN;
    }
    return value;
}

static struct zpt_coherent_displacement_settings
strategy_settings(const struct zpt_noise_filter_settings *settings) {
    return (struct zpt_coherent_displacement_settings){
        .enabled = settings->enabled,
        .activation_distance = settings->activation_distance,
        .coherence_percent = settings->coherence_percent,
        .qualification_timeout_ms = settings->qualification_timeout_ms,
        .idle_timeout_ms = settings->idle_timeout_ms,
    };
}

void zpt_noise_filter_reset(struct zpt_noise_filter_state *state) {
    if (state != NULL) {
        zpt_coherent_displacement_reset(&state->strategy);
    }
}

struct zpt_noise_filter_result
zpt_noise_filter_update(struct zpt_noise_filter_state *state,
                        const struct zpt_noise_filter_settings *settings, int32_t x, int32_t y,
                        uint32_t now_ms, bool suppress) {
    if (state == NULL || settings == NULL) {
        return (struct zpt_noise_filter_result){0};
    }

    struct zpt_coherent_displacement_settings strategy = strategy_settings(settings);
    struct zpt_coherent_displacement_result result =
        zpt_coherent_displacement_update(&state->strategy, &strategy, x, y, now_ms, suppress);
    return (struct zpt_noise_filter_result){
        .x = clamp_i64_to_i32(result.x),
        .y = clamp_i64_to_i32(result.y),
        .phase = (enum zpt_noise_filter_phase)result.phase,
        .reset_for_idle = result.reset_for_idle,
        .reset_for_timeout = result.reset_for_timeout,
        .suppressed = result.suppressed,
        .qualified = result.qualified,
        .discarded = result.discarded,
    };
}
