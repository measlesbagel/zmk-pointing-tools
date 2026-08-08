/* SPDX-License-Identifier: MIT */

#include <assert.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>

#include <zmk/pointing_tools/frame_assembler.h>

static void test_empty_frame_is_ignored(void) {
    struct zpt_frame_assembler assembler = {0};
    struct zpt_raw_motion motion;
    uint32_t flags;

    assert(!zpt_frame_assembler_take(&assembler, &motion, &flags));
}

static void test_repeated_and_partial_axes_are_reconstructed(void) {
    struct zpt_frame_assembler assembler = {0};
    zpt_frame_assembler_add(&assembler, ZPT_MOTION_AXIS_X, 5);
    zpt_frame_assembler_add(&assembler, ZPT_MOTION_AXIS_X, -2);
    zpt_frame_assembler_add(&assembler, ZPT_MOTION_AXIS_Y, -7);

    struct zpt_raw_motion motion;
    uint32_t flags;
    assert(zpt_frame_assembler_take(&assembler, &motion, &flags));
    assert(motion.x_counts == 3 && motion.y_counts == -7);
    assert(flags == 0);
    assert(!zpt_frame_assembler_take(&assembler, &motion, &flags));

    zpt_frame_assembler_add(&assembler, ZPT_MOTION_AXIS_Y, 4);
    assert(zpt_frame_assembler_take(&assembler, &motion, &flags));
    assert(motion.x_counts == 0 && motion.y_counts == 4);
}

static void test_zero_axis_still_completes_a_frame(void) {
    struct zpt_frame_assembler assembler = {0};
    zpt_frame_assembler_add(&assembler, ZPT_MOTION_AXIS_X, 0);

    struct zpt_raw_motion motion;
    uint32_t flags;
    assert(zpt_frame_assembler_take(&assembler, &motion, &flags));
    assert(motion.x_counts == 0 && motion.y_counts == 0);
}

static void test_accumulation_saturates_and_marks_clipping(void) {
    struct zpt_frame_assembler assembler = {0};
    zpt_frame_assembler_add(&assembler, ZPT_MOTION_AXIS_X, INT64_MAX);
    zpt_frame_assembler_add(&assembler, ZPT_MOTION_AXIS_X, 1);
    zpt_frame_assembler_add(&assembler, ZPT_MOTION_AXIS_Y, INT64_MIN);
    zpt_frame_assembler_add(&assembler, ZPT_MOTION_AXIS_Y, -1);

    struct zpt_raw_motion motion;
    uint32_t flags;
    assert(zpt_frame_assembler_take(&assembler, &motion, &flags));
    assert(motion.x_counts == INT64_MAX && motion.y_counts == INT64_MIN);
    assert((flags & ZPT_SIGNAL_FLAG_CLIPPED) != 0);
}

int main(void) {
    test_empty_frame_is_ignored();
    test_repeated_and_partial_axes_are_reconstructed();
    test_zero_axis_still_completes_a_frame();
    test_accumulation_saturates_and_marks_clipping();
    puts("frame assembler tests passed");
    return 0;
}
