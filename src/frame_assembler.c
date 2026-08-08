/* SPDX-License-Identifier: MIT */

#include <limits.h>
#include <stddef.h>

#include <zmk/pointing_tools/frame_assembler.h>

static int64_t saturating_add(int64_t lhs, int64_t rhs, uint32_t *flags) {
    if (rhs > 0 && lhs > INT64_MAX - rhs) {
        *flags |= ZPT_SIGNAL_FLAG_CLIPPED;
        return INT64_MAX;
    }
    if (rhs < 0 && lhs < INT64_MIN - rhs) {
        *flags |= ZPT_SIGNAL_FLAG_CLIPPED;
        return INT64_MIN;
    }
    return lhs + rhs;
}

void zpt_frame_assembler_reset(struct zpt_frame_assembler *assembler) {
    if (assembler != NULL) {
        *assembler = (struct zpt_frame_assembler){0};
    }
}

void zpt_frame_assembler_add(struct zpt_frame_assembler *assembler, enum zpt_motion_axis axis,
                             int64_t counts) {
    if (assembler == NULL) {
        return;
    }

    switch (axis) {
    case ZPT_MOTION_AXIS_X:
        assembler->x_counts = saturating_add(assembler->x_counts, counts, &assembler->flags);
        break;
    case ZPT_MOTION_AXIS_Y:
        assembler->y_counts = saturating_add(assembler->y_counts, counts, &assembler->flags);
        break;
    default:
        return;
    }
    assembler->saw_axis = true;
}

bool zpt_frame_assembler_take(struct zpt_frame_assembler *assembler, struct zpt_raw_motion *motion,
                              uint32_t *flags) {
    if (assembler == NULL || motion == NULL || flags == NULL || !assembler->saw_axis) {
        return false;
    }

    *motion = (struct zpt_raw_motion){
        .x_counts = assembler->x_counts,
        .y_counts = assembler->y_counts,
    };
    *flags = assembler->flags;
    zpt_frame_assembler_reset(assembler);
    return true;
}
