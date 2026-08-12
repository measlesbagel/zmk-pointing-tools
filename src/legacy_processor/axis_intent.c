/* SPDX-License-Identifier: MIT */

#include <stddef.h>

#include <zmk/pointing_tools/legacy_processor/axis_intent.h>

enum zpt_axis_intent zpt_axis_intent_update(struct zpt_axis_intent_state *state,
                                            const struct zpt_axis_intent_config *config,
                                            enum zpt_axis_policy policy, int32_t x, int32_t y,
                                            uint32_t elapsed_ms) {
    if (state == NULL || config == NULL) {
        return ZPT_AXIS_INTENT_FREE;
    }

    struct zpt_axis_intent_settings settings = {
        .engage_ratio_percent = config->engage_ratio_percent,
        .release_ratio_percent = config->release_ratio_percent,
        .activation_distance = config->activation_distance,
        .window_ms = config->window_ms,
    };
    return zpt_axis_intent_estimate(state, &settings, policy, x, y, elapsed_ms);
}
