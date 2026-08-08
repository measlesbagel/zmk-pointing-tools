/* SPDX-License-Identifier: MIT */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <zmk/pointing_tools/signal.h>

enum zpt_motion_axis {
    ZPT_MOTION_AXIS_X = 0,
    ZPT_MOTION_AXIS_Y,
};

struct zpt_frame_assembler {
    int64_t x_counts;
    int64_t y_counts;
    uint32_t flags;
    bool saw_axis;
};

void zpt_frame_assembler_reset(struct zpt_frame_assembler *assembler);
void zpt_frame_assembler_add(struct zpt_frame_assembler *assembler, enum zpt_motion_axis axis,
                             int64_t counts);
bool zpt_frame_assembler_take(struct zpt_frame_assembler *assembler, struct zpt_raw_motion *motion,
                              uint32_t *flags);
