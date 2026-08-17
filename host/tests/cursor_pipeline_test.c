/* SPDX-License-Identifier: MIT */

#include <assert.h>
#include <errno.h>
#include <stdio.h>

#include <zmk/pointing_tools/core/pipeline.h>
#include <zmk/pointing_tools/stage/cursor_quantizer.h>
#include <zmk/pointing_tools/stage/cursor_transfer.h>
#include <zmk/pointing_tools/stage/resolution_normalize.h>

struct capture_state {
    struct zpt_signal signal;
    uint32_t outputs;
};

static int capture_emit(struct zpt_sink *sink, const struct zpt_signal *signal) {
    struct capture_state *capture = sink->state;
    capture->signal = *signal;
    capture->outputs++;
    return 0;
}

static const struct zpt_sink_api capture_api = {
    .type_id = "pointer-capture",
    .accepted_kinds = ZPT_SIGNAL_KIND_MASK(ZPT_SIGNAL_NORMALIZED_MOTION) |
                      ZPT_SIGNAL_KIND_MASK(ZPT_SIGNAL_POINTER_DELTA),
    .emit = capture_emit,
};

struct cursor_fixture {
    uint16_t cpi;
    struct zpt_cursor_transfer_config transfer_config;
    struct zpt_cursor_quantizer_state quantizer_state;
    struct zpt_stage transfer_stage;
    struct zpt_stage quantizer_stage;
    struct zpt_stage *stages[2];
    struct capture_state capture;
    struct zpt_sink sink;
    struct zpt_pipeline pipeline;
};

static void cursor_fixture_init(struct cursor_fixture *fixture, uint16_t multiplier,
                                uint16_t divisor, uint16_t cpi) {
    *fixture = (struct cursor_fixture){0};
    fixture->cpi = cpi;
    fixture->transfer_config = (struct zpt_cursor_transfer_config){
        .scale_multiplier = multiplier,
        .scale_divisor = divisor,
    };
    fixture->transfer_stage = (struct zpt_stage){
        .stable_id = "cursor-transfer",
        .api = &zpt_cursor_transfer_stage_api,
        .config = &fixture->transfer_config,
    };
    fixture->quantizer_stage = (struct zpt_stage){
        .stable_id = "cursor-quantizer",
        .api = &zpt_cursor_quantizer_stage_api,
        .state = &fixture->quantizer_state,
    };
    fixture->stages[0] = &fixture->transfer_stage;
    fixture->stages[1] = &fixture->quantizer_stage;
    fixture->sink = (struct zpt_sink){
        .stable_id = "capture",
        .api = &capture_api,
        .state = &fixture->capture,
    };
    fixture->pipeline = (struct zpt_pipeline){
        .stable_id = "cursor-test",
        .input_kind = ZPT_SIGNAL_NORMALIZED_MOTION,
        .stages = fixture->stages,
        .stage_count = 2,
        .sink = &fixture->sink,
        .dispatch_budget = 4,
    };
    assert(zpt_pipeline_validate(&fixture->pipeline) == 0);
    assert(zpt_pipeline_activate(&fixture->pipeline, ZPT_RESET_PIPELINE_ENTERED) == 0);
}

static int push_normalized(struct zpt_pipeline *pipeline, struct zpt_pipeline_result *result,
                           uint32_t timestamp, zpt_fixed_t x, zpt_fixed_t y, uint16_t cpi) {
    struct zpt_signal signal = {
        .kind = ZPT_SIGNAL_NORMALIZED_MOTION,
        .metadata = {.observed_at_ms = timestamp, .resolution_cpi = cpi},
        .data.fixed_vector = {.x = x, .y = y},
    };
    return zpt_pipeline_push(pipeline, &signal, result);
}

static void test_transfer_applies_gain_and_flags_clipping(void) {
    struct cursor_fixture fixture;
    /* CPI 254 derives a 10 units/mm factor, so 1 mm of motion == 10 units. */
    cursor_fixture_init(&fixture, 2, 1, 254);

    struct zpt_pipeline_result result;
    assert(push_normalized(&fixture.pipeline, &result, 0, ZPT_FIXED_ONE + ZPT_FIXED_ONE / 2,
                           -ZPT_FIXED_ONE, fixture.cpi) == 0);
    assert(fixture.capture.outputs == 1);
    /* 1.5 mm doubled is 3 mm; at 10 units/mm that is 30 units (y: -2 mm -> -20). */
    assert(fixture.capture.signal.data.delta.x == 30);
    assert(fixture.capture.signal.data.delta.y == -20);
}

static void test_transfer_saturates_with_clipped_flag(void) {
    struct zpt_cursor_transfer_config config = {.scale_multiplier = 2, .scale_divisor = 1};
    struct zpt_stage stage = {
        .stable_id = "transfer",
        .api = &zpt_cursor_transfer_stage_api,
        .config = &config,
    };
    struct zpt_stage *stages[1] = {&stage};
    struct capture_state capture = {0};
    struct zpt_sink sink = {.stable_id = "capture", .api = &capture_api, .state = &capture};
    struct zpt_pipeline pipeline = {
        .stable_id = "transfer-saturation",
        .input_kind = ZPT_SIGNAL_NORMALIZED_MOTION,
        .stages = stages,
        .stage_count = 1,
        .sink = &sink,
        .dispatch_budget = 2,
    };
    assert(zpt_pipeline_validate(&pipeline) == 0);
    assert(zpt_pipeline_activate(&pipeline, ZPT_RESET_PIPELINE_ENTERED) == 0);

    struct zpt_pipeline_result result;
    struct zpt_signal signal = {
        .kind = ZPT_SIGNAL_NORMALIZED_MOTION,
        .metadata = {.observed_at_ms = 0},
        .data.fixed_vector = {.x = INT64_MAX / 2 + 1, .y = INT64_MIN / 2 - 1},
    };
    assert(zpt_pipeline_push(&pipeline, &signal, &result) == 0);
    assert(capture.signal.metadata.flags & ZPT_SIGNAL_FLAG_CLIPPED);
    assert(capture.signal.data.fixed_vector.x == INT64_MAX);
    assert(capture.signal.data.fixed_vector.y == INT64_MIN);
}

static void test_quantizer_accumulates_sub_pixel_remainders(void) {
    struct cursor_fixture fixture;
    /* CPI 254 derives a 10 units/mm factor, so 0.1 mm is one unit. 0.05 mm
     * (ZPT_FIXED_ONE/20) is sub-unit and accumulates across frames. */
    cursor_fixture_init(&fixture, 1, 1, 254);

    struct zpt_pipeline_result result;
    /* Three sub-unit frames accumulate to one output unit. */
    assert(push_normalized(&fixture.pipeline, &result, 0, ZPT_FIXED_ONE / 20, 0, fixture.cpi) == 0);
    assert(fixture.capture.signal.data.delta.x == 0);
    assert(fixture.quantizer_state.remainder_x == 32760);
    assert(push_normalized(&fixture.pipeline, &result, 8, ZPT_FIXED_ONE / 20, 0, fixture.cpi) == 0);
    assert(fixture.capture.signal.data.delta.x == 0);
    assert(fixture.quantizer_state.remainder_x == 65520);
    assert(push_normalized(&fixture.pipeline, &result, 16, ZPT_FIXED_ONE / 20, 0, fixture.cpi) == 0);
    assert(fixture.capture.signal.data.delta.x == 1);
    assert(fixture.quantizer_state.remainder_x == 32744);

    /* Negative sub-pixel motion carries the sign correctly (fresh state). */
    cursor_fixture_init(&fixture, 1, 1, 254);
    assert(push_normalized(&fixture.pipeline, &result, 24, -ZPT_FIXED_ONE / 20, 0, fixture.cpi) == 0);
    assert(fixture.capture.signal.data.delta.x == 0);
    assert(push_normalized(&fixture.pipeline, &result, 32, -ZPT_FIXED_ONE / 20, 0, fixture.cpi) == 0);
    assert(fixture.capture.signal.data.delta.x == 0);
    assert(push_normalized(&fixture.pipeline, &result, 40, -ZPT_FIXED_ONE / 20, 0, fixture.cpi) == 0);
    assert(fixture.capture.signal.data.delta.x == -1);
    assert(fixture.quantizer_state.remainder_x == -32744);
}

static void test_identity_round_trip_through_normalization(void) {
    /* Full composed path: counts -> Q16 mm -> gain -> units, with the
     * quantizer deriving a 1:1 factor from the 700 CPI to restore the count
     * domain (no explicit units-per-meter). */
    struct zpt_stage normalize_stage = {
        .stable_id = "resolution-normalize",
        .api = &zpt_resolution_normalize_stage_api,
    };
    struct zpt_cursor_transfer_config transfer_config = {.scale_multiplier = 1, .scale_divisor = 1};
    struct zpt_cursor_quantizer_state quantizer_state = {0};
    struct zpt_stage transfer_stage = {
        .stable_id = "cursor-transfer",
        .api = &zpt_cursor_transfer_stage_api,
        .config = &transfer_config,
    };
    struct zpt_stage quantizer_stage = {
        .stable_id = "cursor-quantizer",
        .api = &zpt_cursor_quantizer_stage_api,
        .state = &quantizer_state,
    };
    struct zpt_stage *stages[3] = {&normalize_stage, &transfer_stage, &quantizer_stage};
    struct capture_state capture = {0};
    struct zpt_sink sink = {.stable_id = "capture", .api = &capture_api, .state = &capture};
    struct zpt_pipeline pipeline = {
        .stable_id = "cursor-roundtrip",
        .input_kind = ZPT_SIGNAL_RAW_MOTION,
        .stages = stages,
        .stage_count = 3,
        .sink = &sink,
        .dispatch_budget = 8,
    };
    assert(zpt_pipeline_validate(&pipeline) == 0);
    assert(zpt_pipeline_activate(&pipeline, ZPT_RESET_PIPELINE_ENTERED) == 0);

    struct zpt_pipeline_result result;
    for (int frame = 0; frame < 3; frame++) {
        struct zpt_signal signal = {
            .kind = ZPT_SIGNAL_RAW_MOTION,
            .metadata = {.observed_at_ms = (uint32_t)(frame * 8), .resolution_cpi = 700},
            .data.raw_motion = {.x_counts = 1, .y_counts = 1},
        };
        assert(zpt_pipeline_push(&pipeline, &signal, &result) == 0);
    }
    /* Fixed-point conversion is sub-count exact: two single-count frames
     * accumulate to one output unit, and the remainder carries forward. */
    assert(capture.signal.data.delta.x == 1);
    assert(capture.signal.data.delta.y == 1);
    assert(quantizer_state.remainder_x == 65533);
    assert(quantizer_state.remainder_y == 65533);
}

int main(void) {
    test_transfer_applies_gain_and_flags_clipping();
    test_transfer_saturates_with_clipped_flag();
    test_quantizer_accumulates_sub_pixel_remainders();
    test_identity_round_trip_through_normalization();
    puts("cursor pipeline tests passed");
    return 0;
}
