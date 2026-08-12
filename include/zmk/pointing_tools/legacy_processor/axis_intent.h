/* SPDX-License-Identifier: MIT */
#pragma once

#include <stdint.h>

#include <zmk/pointing_tools/stage/axis_intent.h>

/* Transitional count-domain adapter retained by the current Bridges firmware.
 * The estimator semantics live in the shared strategy; this adapter feeds it
 * sensor counts with an activation distance in counts. */

struct zpt_axis_intent_config {
    uint16_t engage_ratio_percent;
    uint16_t release_ratio_percent;
    uint16_t activation_distance;
    uint16_t window_ms;
};

enum zpt_axis_intent zpt_axis_intent_update(struct zpt_axis_intent_state *state,
                                            const struct zpt_axis_intent_config *config,
                                            enum zpt_axis_policy policy, int32_t x, int32_t y,
                                            uint32_t elapsed_ms);
