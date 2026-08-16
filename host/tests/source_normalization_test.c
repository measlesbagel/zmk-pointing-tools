/* SPDX-License-Identifier: MIT */

#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>

#include <zmk/pointing_tools/core/pipeline.h>
#include <zmk/pointing_tools/source/motion_source.h>
#include <zmk/pointing_tools/stage/orientation.h>
#include <zmk/pointing_tools/stage/resolution_normalize.h>

#define ARRAY_SIZE(values) (sizeof(values) / sizeof((values)[0]))

struct capture_state {
    struct zpt_signal signal;
    uint32_t outputs;
};

static int capture_normalized(struct zpt_sink *sink, const struct zpt_signal *signal) {
    struct capture_state *state = sink->state;
    state->signal = *signal;
    state->outputs++;
    return 0;
}

static const struct zpt_sink_api capture_api = {
    .type_id = "capture-normalized",
    .accepted_kinds = ZPT_SIGNAL_KIND_MASK(ZPT_SIGNAL_NORMALIZED_MOTION),
    .emit = capture_normalized,
};

static void test_local_source_metadata_and_sequence(void) {
    const struct zpt_motion_source_config config = {
        .flags = ZPT_SIGNAL_FLAG_LOCAL,
        .source_id = 7,
        .resolution_cpi = 700,
    };
    struct zpt_motion_source_state source;
    assert(zpt_motion_source_init(&source, &config) == 0);

    zpt_motion_source_add(&source, ZPT_MOTION_AXIS_X, 4);
    zpt_motion_source_add(&source, ZPT_MOTION_AXIS_X, -1);
    zpt_motion_source_add(&source, ZPT_MOTION_AXIS_Y, -2);
    struct zpt_signal signal;
    assert(zpt_motion_source_take(&source, 123, 8000, ZPT_SIGNAL_FLAG_COALESCED, &signal));
    assert(signal.kind == ZPT_SIGNAL_RAW_MOTION);
    assert(signal.data.raw_motion.x_counts == 3 && signal.data.raw_motion.y_counts == -2);
    assert(signal.metadata.observed_at_ms == 123 && signal.metadata.sample_span_us == 8000);
    assert(signal.metadata.source_id == 7 && signal.metadata.sequence == 0);
    assert(signal.metadata.resolution_cpi == 700);
    assert(signal.metadata.flags == (ZPT_SIGNAL_FLAG_LOCAL | ZPT_SIGNAL_FLAG_COALESCED));

    zpt_motion_source_add(&source, ZPT_MOTION_AXIS_Y, 5);
    assert(zpt_motion_source_take(&source, 130, 0, 0, &signal));
    assert(signal.metadata.sequence == 1);
    assert(signal.data.raw_motion.x_counts == 0 && signal.data.raw_motion.y_counts == 5);
}

static void test_transported_source_and_clipping_evidence(void) {
    const struct zpt_motion_source_config config = {
        .flags = ZPT_SIGNAL_FLAG_TRANSPORTED | ZPT_SIGNAL_FLAG_TIMING_ESTIMATED,
        .source_id = 2,
        .resolution_cpi = 800,
    };
    struct zpt_motion_source_state source;
    assert(zpt_motion_source_init(&source, &config) == 0);
    zpt_motion_source_add(&source, ZPT_MOTION_AXIS_X, 12);
    zpt_motion_source_add(&source, ZPT_MOTION_AXIS_Y, -9);

    struct zpt_signal signal;
    assert(zpt_motion_source_take(&source, 40, 16000, 0, &signal));
    assert(signal.data.raw_motion.x_counts == 12 && signal.data.raw_motion.y_counts == -9);
    assert(signal.metadata.sequence == 0);
    assert((signal.metadata.flags & ZPT_SIGNAL_FLAG_TRANSPORTED) != 0);
    assert((signal.metadata.flags & ZPT_SIGNAL_FLAG_TIMING_ESTIMATED) != 0);

    zpt_motion_source_add(&source, ZPT_MOTION_AXIS_X, INT64_MAX);
    zpt_motion_source_add(&source, ZPT_MOTION_AXIS_X, 1);
    assert(zpt_motion_source_take(&source, 56, 16000, ZPT_SIGNAL_FLAG_LOCAL, &signal));
    assert(signal.data.raw_motion.x_counts == INT64_MAX);
    assert((signal.metadata.flags & ZPT_SIGNAL_FLAG_TRANSPORTED) != 0);
    assert((signal.metadata.flags & ZPT_SIGNAL_FLAG_LOCAL) == 0);
    assert((signal.metadata.flags & ZPT_SIGNAL_FLAG_TIMING_ESTIMATED) != 0);
    assert((signal.metadata.flags & ZPT_SIGNAL_FLAG_CLIPPED) != 0);

    const struct zpt_motion_source_config invalid_location = {
        .flags = ZPT_SIGNAL_FLAG_LOCAL | ZPT_SIGNAL_FLAG_TRANSPORTED,
        .resolution_cpi = 700,
    };
    assert(zpt_motion_source_init(&source, &invalid_location) == -EINVAL);
}

static void test_transported_source_can_preserve_codec_sequence(void) {
    const struct zpt_motion_source_config config = {
        .flags = ZPT_SIGNAL_FLAG_TRANSPORTED,
        .source_id = 3,
        .resolution_cpi = 700,
    };
    struct zpt_motion_source_state source;
    assert(zpt_motion_source_init(&source, &config) == 0);

    zpt_motion_source_add(&source, ZPT_MOTION_AXIS_X, -12);
    zpt_motion_source_add(&source, ZPT_MOTION_AXIS_Y, 8);
    struct zpt_signal signal;
    assert(zpt_motion_source_take_at_sequence(&source, 90, 16000, 42, ZPT_SIGNAL_FLAG_SEQUENCE_GAP,
                                              &signal));
    assert(signal.metadata.sequence == 42);
    assert((signal.metadata.flags & ZPT_SIGNAL_FLAG_SEQUENCE_GAP) != 0U);

    zpt_motion_source_add(&source, ZPT_MOTION_AXIS_X, 1);
    assert(zpt_motion_source_take(&source, 106, 16000, 0, &signal));
    assert(signal.metadata.sequence == 43);
}

static void test_orthogonal_orientation(void) {
    const struct zpt_raw_motion input = {.x_counts = 3, .y_counts = -8};
    struct zpt_raw_motion output;
    const struct zpt_orientation_config identity = {0};
    assert(zpt_orientation_apply(&identity, &input, &output) == 0);
    assert(output.x_counts == 3 && output.y_counts == -8);

    const struct zpt_orientation_config oriented = {
        .swap_xy = true,
        .invert_x = true,
        .invert_y = true,
    };
    assert(zpt_orientation_apply(&oriented, &input, &output) == 0);
    assert(output.x_counts == 8 && output.y_counts == -3);

    const struct zpt_raw_motion minimum = {.x_counts = INT64_MIN};
    const struct zpt_orientation_config invert_x = {.invert_x = true};
    output = (struct zpt_raw_motion){.x_counts = 11, .y_counts = 22};
    assert(zpt_orientation_apply(&invert_x, &minimum, &output) == -ERANGE);
    assert(output.x_counts == 11 && output.y_counts == 22);
}

static void test_resolution_conversion(void) {
    zpt_fixed_t distance;
    assert(zpt_counts_to_millimeters(700, 700, &distance) == 0);
    assert(distance == 1664614);
    assert(zpt_counts_to_millimeters(-1200, 1200, &distance) == 0);
    assert(distance == -1664614);
    assert(zpt_counts_to_millimeters(1, 700, &distance) == 0);
    assert(distance == 2378);
    zpt_fixed_t same_distance;
    assert(zpt_counts_to_millimeters(600, 1200, &same_distance) == 0);
    assert(same_distance == 832307);
    assert(zpt_counts_to_millimeters(350, 700, &distance) == 0);
    assert(distance == same_distance);
    assert(zpt_counts_to_millimeters(1, 0, &distance) == -EINVAL);
    assert(zpt_counts_to_millimeters(INT64_MAX, 700, &distance) == -ERANGE);
}

static void test_orient_then_normalize_pipeline(void) {
    const struct zpt_orientation_config orientation = {
        .swap_xy = true,
        .invert_x = true,
    };
    struct zpt_stage stages[] = {
        {.stable_id = "orientation",
         .api = &zpt_orthogonal_orientation_stage_api,
         .config = &orientation},
        {.stable_id = "resolution", .api = &zpt_resolution_normalize_stage_api},
    };
    struct zpt_stage *stage_refs[] = {&stages[0], &stages[1]};
    struct capture_state capture = {0};
    struct zpt_sink sink = {
        .stable_id = "capture",
        .api = &capture_api,
        .state = &capture,
    };
    struct zpt_pipeline pipeline = {
        .stable_id = "normalized-source",
        .input_kind = ZPT_SIGNAL_RAW_MOTION,
        .stages = stage_refs,
        .stage_count = ARRAY_SIZE(stage_refs),
        .sink = &sink,
        .dispatch_budget = 4,
    };
    assert(zpt_pipeline_validate(&pipeline) == 0);
    assert(zpt_pipeline_activate(&pipeline, ZPT_RESET_PIPELINE_ENTERED) == 0);

    struct zpt_signal input = {
        .kind = ZPT_SIGNAL_RAW_MOTION,
        .metadata =
            {
                .observed_at_ms = 55,
                .flags = ZPT_SIGNAL_FLAG_LOCAL,
                .source_id = 4,
                .sequence = 9,
                .resolution_cpi = 700,
            },
        .data.raw_motion = {.x_counts = 350, .y_counts = -700},
    };
    struct zpt_pipeline_result result;
    assert(zpt_pipeline_push(&pipeline, &input, &result) == 0);
    assert(result.outputs == 1 && capture.outputs == 1);
    assert(capture.signal.kind == ZPT_SIGNAL_NORMALIZED_MOTION);
    assert(capture.signal.data.fixed_vector.x == (zpt_fixed_t)25 * ZPT_FIXED_ONE + 26214);
    assert(capture.signal.data.fixed_vector.y == (zpt_fixed_t)12 * ZPT_FIXED_ONE + 45875);
    assert(capture.signal.metadata.observed_at_ms == 55);
    assert(capture.signal.metadata.source_id == 4 && capture.signal.metadata.sequence == 9);
    assert(capture.signal.metadata.resolution_cpi == 700);
}

int main(void) {
    test_local_source_metadata_and_sequence();
    test_transported_source_and_clipping_evidence();
    test_transported_source_can_preserve_codec_sequence();
    test_orthogonal_orientation();
    test_resolution_conversion();
    test_orient_then_normalize_pipeline();
    puts("source normalization tests passed");
    return 0;
}
