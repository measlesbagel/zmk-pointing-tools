/* SPDX-License-Identifier: MIT */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Shared model used by the transitional monolithic processors. */

enum zpt_axis_policy {
    ZPT_AXIS_POLICY_FREE = 0,
    ZPT_AXIS_POLICY_ADAPTIVE = 1,
    ZPT_AXIS_POLICY_HORIZONTAL = 2,
    ZPT_AXIS_POLICY_VERTICAL = 3,
};

enum zpt_axis_intent {
    ZPT_AXIS_INTENT_UNDECIDED = 0,
    ZPT_AXIS_INTENT_FREE,
    ZPT_AXIS_INTENT_HORIZONTAL,
    ZPT_AXIS_INTENT_VERTICAL,
};

struct zpt_axis_intent_config {
    uint16_t engage_ratio_percent;
    uint16_t release_ratio_percent;
    uint16_t activation_distance;
    uint16_t window_ms;
};

struct zpt_axis_intent_state {
    uint32_t horizontal_energy;
    uint32_t vertical_energy;
    enum zpt_axis_intent intent;
};

void zpt_axis_intent_reset(struct zpt_axis_intent_state *state);

enum zpt_axis_intent zpt_axis_intent_update(struct zpt_axis_intent_state *state,
                                            const struct zpt_axis_intent_config *config,
                                            enum zpt_axis_policy policy, int32_t x, int32_t y,
                                            uint32_t elapsed_ms);
