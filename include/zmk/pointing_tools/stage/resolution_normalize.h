/* SPDX-License-Identifier: MIT */
#pragma once

#include <zmk/pointing_tools/core/pipeline.h>

/* Convert nominal sensor counts to signed Q16 millimetres, rounded nearest. */
int zpt_counts_to_millimeters(int64_t counts, uint16_t resolution_cpi, zpt_fixed_t *millimeters);

extern const struct zpt_stage_api zpt_resolution_normalize_stage_api;
