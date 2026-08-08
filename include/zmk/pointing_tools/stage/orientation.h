/* SPDX-License-Identifier: MIT */
#pragma once

#include <stdbool.h>

#include <zmk/pointing_tools/core/pipeline.h>

struct zpt_orientation_config {
    /* Swap first; inversions apply to the resulting logical axes. */
    bool swap_xy;
    bool invert_x;
    bool invert_y;
};

/* Apply an exact orthogonal mounting transform atomically. */
int zpt_orientation_apply(const struct zpt_orientation_config *config,
                          const struct zpt_raw_motion *input, struct zpt_raw_motion *output);

extern const struct zpt_stage_api zpt_orthogonal_orientation_stage_api;
