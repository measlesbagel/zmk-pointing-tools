/* SPDX-License-Identifier: MIT */

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <zmk/pointing_tools/core/pipeline.h>
#include <zmk/pointing_tools/stage/pointer_identity.h>

#define ARRAY_SIZE(values) (sizeof(values) / sizeof((values)[0]))
#define TEST_OUTPUT_CAPACITY 16U

struct capture_sink_state {
    struct zpt_signal outputs[TEST_OUTPUT_CAPACITY];
    size_t output_count;
    uint32_t activates;
    uint32_t deactivates;
    uint32_t resets;
    enum zpt_reset_reason last_reset;
};

static int capture_emit(struct zpt_sink *sink, const struct zpt_signal *signal) {
    struct capture_sink_state *state = sink->state;
    if (state->output_count >= ARRAY_SIZE(state->outputs)) {
        return -ENOSPC;
    }
    state->outputs[state->output_count++] = *signal;
    return 0;
}

static int capture_activate(struct zpt_sink *sink, enum zpt_reset_reason reason) {
    (void)reason;
    struct capture_sink_state *state = sink->state;
    state->activates++;
    return 0;
}

static int capture_deactivate(struct zpt_sink *sink, enum zpt_reset_reason reason) {
    (void)reason;
    struct capture_sink_state *state = sink->state;
    state->deactivates++;
    return 0;
}

static void capture_reset(struct zpt_sink *sink, enum zpt_reset_reason reason) {
    struct capture_sink_state *state = sink->state;
    state->resets++;
    state->last_reset = reason;
}

static const struct zpt_sink_api pointer_sink_api = {
    .type_id = "capture-pointer",
    .accepted_kinds = ZPT_SIGNAL_KIND_MASK(ZPT_SIGNAL_POINTER_DELTA),
    .emit = capture_emit,
    .activate = capture_activate,
    .deactivate = capture_deactivate,
    .reset = capture_reset,
};

static const struct zpt_sink_api normalized_sink_api = {
    .type_id = "capture-normalized",
    .accepted_kinds = ZPT_SIGNAL_KIND_MASK(ZPT_SIGNAL_NORMALIZED_MOTION),
    .emit = capture_emit,
    .activate = capture_activate,
    .deactivate = capture_deactivate,
    .reset = capture_reset,
};

struct transform_config {
    int32_t numerator;
    int32_t denominator;
};

static int normalize_process(struct zpt_stage *stage, const struct zpt_signal *signal,
                             struct zpt_stage_context *context) {
    const struct transform_config *config = stage->config;
    struct zpt_signal output = *signal;
    output.kind = ZPT_SIGNAL_NORMALIZED_MOTION;
    output.data.fixed_vector.x =
        signal->data.raw_motion.x_counts * ZPT_FIXED_ONE * config->numerator / config->denominator;
    output.data.fixed_vector.y =
        signal->data.raw_motion.y_counts * ZPT_FIXED_ONE * config->numerator / config->denominator;
    return zpt_stage_emit(context, &output);
}

static const struct zpt_stage_api normalize_api = {
    .strategy_id = "test-normalize",
    .accepted_kinds = ZPT_SIGNAL_KIND_MASK(ZPT_SIGNAL_RAW_MOTION),
    .output_kind = ZPT_SIGNAL_NORMALIZED_MOTION,
    .process = normalize_process,
};

static int pointer_map_process(struct zpt_stage *stage, const struct zpt_signal *signal,
                               struct zpt_stage_context *context) {
    (void)stage;
    struct zpt_signal output = *signal;
    output.kind = ZPT_SIGNAL_POINTER_DELTA;
    return zpt_stage_emit(context, &output);
}

static const struct zpt_stage_api pointer_map_api = {
    .strategy_id = "test-pointer-map",
    .accepted_kinds = ZPT_SIGNAL_KIND_MASK(ZPT_SIGNAL_NORMALIZED_MOTION),
    .output_kind = ZPT_SIGNAL_POINTER_DELTA,
    .process = pointer_map_process,
};

static int duplicate_process(struct zpt_stage *stage, const struct zpt_signal *signal,
                             struct zpt_stage_context *context) {
    (void)stage;
    int ret = zpt_stage_emit(context, signal);
    return ret < 0 ? ret : zpt_stage_emit(context, signal);
}

static const struct zpt_stage_api duplicate_api = {
    .strategy_id = "test-duplicate",
    .accepted_kinds = ZPT_SIGNAL_KIND_MASK(ZPT_SIGNAL_NORMALIZED_MOTION),
    .output_kind = ZPT_SIGNAL_NORMALIZED_MOTION,
    .process = duplicate_process,
};

static int wrong_kind_process(struct zpt_stage *stage, const struct zpt_signal *signal,
                              struct zpt_stage_context *context) {
    (void)stage;
    struct zpt_signal wrong = *signal;
    wrong.kind = ZPT_SIGNAL_ACTION;
    return zpt_stage_emit(context, &wrong);
}

static const struct zpt_stage_api wrong_kind_api = {
    .strategy_id = "test-wrong-kind",
    .accepted_kinds = ZPT_SIGNAL_KIND_MASK(ZPT_SIGNAL_RAW_MOTION),
    .output_kind = ZPT_SIGNAL_NORMALIZED_MOTION,
    .process = wrong_kind_process,
};

struct buffered_stage_state {
    struct zpt_signal pending;
    bool have_pending;
    bool emit_on_deactivate;
    uint32_t delay_ms;
    uint32_t activates;
    uint32_t deactivates;
    uint32_t resets;
    enum zpt_reset_reason last_reset;
};

static int buffered_process(struct zpt_stage *stage, const struct zpt_signal *signal,
                            struct zpt_stage_context *context) {
    struct buffered_stage_state *state = stage->state;
    state->pending = *signal;
    state->have_pending = true;
    return zpt_stage_schedule_flush(context, zpt_stage_now_ms(context) + state->delay_ms);
}

static int buffered_flush(struct zpt_stage *stage, uint32_t now_ms,
                          struct zpt_stage_context *context) {
    (void)now_ms;
    struct buffered_stage_state *state = stage->state;
    if (!state->have_pending) {
        return 0;
    }
    struct zpt_signal output = state->pending;
    state->have_pending = false;
    return zpt_stage_emit(context, &output);
}

static int buffered_activate(struct zpt_stage *stage, enum zpt_reset_reason reason) {
    (void)reason;
    struct buffered_stage_state *state = stage->state;
    state->activates++;
    return 0;
}

static int buffered_deactivate(struct zpt_stage *stage, uint32_t now_ms,
                               enum zpt_reset_reason reason, struct zpt_stage_context *context) {
    (void)now_ms;
    (void)reason;
    struct buffered_stage_state *state = stage->state;
    state->deactivates++;
    if (!state->emit_on_deactivate || !state->have_pending) {
        return 0;
    }
    struct zpt_signal output = state->pending;
    state->have_pending = false;
    return zpt_stage_emit(context, &output);
}

static void buffered_reset(struct zpt_stage *stage, enum zpt_reset_reason reason) {
    struct buffered_stage_state *state = stage->state;
    state->have_pending = false;
    state->resets++;
    state->last_reset = reason;
}

static const struct zpt_stage_api buffered_api = {
    .strategy_id = "test-buffer",
    .accepted_kinds = ZPT_SIGNAL_KIND_MASK(ZPT_SIGNAL_NORMALIZED_MOTION),
    .output_kind = ZPT_SIGNAL_NORMALIZED_MOTION,
    .flags = ZPT_STAGE_STATEFUL,
    .process = buffered_process,
    .flush = buffered_flush,
    .activate = buffered_activate,
    .deactivate = buffered_deactivate,
    .reset = buffered_reset,
};

static struct zpt_signal raw_signal(uint32_t timestamp_ms, int64_t x, int64_t y) {
    return (struct zpt_signal){
        .kind = ZPT_SIGNAL_RAW_MOTION,
        .metadata =
            {
                .observed_at_ms = timestamp_ms,
                .sample_span_us = 8000,
                .flags = ZPT_SIGNAL_FLAG_LOCAL,
                .source_id = 7,
                .sequence = 42,
                .resolution_cpi = 700,
            },
        .annotations =
            {
                .axis_intent = ZPT_SIGNAL_AXIS_UNDECIDED,
            },
        .data.raw_motion =
            {
                .x_counts = x,
                .y_counts = y,
            },
    };
}

static struct zpt_signal normalized_signal(uint32_t timestamp_ms, int32_t x, int32_t y) {
    struct zpt_signal signal = {
        .kind = ZPT_SIGNAL_NORMALIZED_MOTION,
        .metadata =
            {
                .observed_at_ms = timestamp_ms,
                .source_id = 3,
            },
    };
    signal.data.fixed_vector.x = zpt_fixed_from_int(x);
    signal.data.fixed_vector.y = zpt_fixed_from_int(y);
    return signal;
}

static void test_typed_pipeline_and_metadata(void) {
    const struct transform_config normalize_config = {.numerator = 2, .denominator = 1};
    struct zpt_stage stage_storage[] = {
        {.stable_id = "normalize", .api = &normalize_api, .config = &normalize_config},
        {.stable_id = "pointer-map", .api = &pointer_map_api},
    };
    struct zpt_stage *stages[] = {&stage_storage[0], &stage_storage[1]};
    struct capture_sink_state sink_state = {0};
    struct zpt_sink sink = {
        .stable_id = "cursor",
        .api = &pointer_sink_api,
        .state = &sink_state,
    };
    struct zpt_pipeline pipeline = {
        .stable_id = "right-cursor",
        .input_kind = ZPT_SIGNAL_RAW_MOTION,
        .stages = stages,
        .stage_count = ARRAY_SIZE(stages),
        .sink = &sink,
        .dispatch_budget = 16,
    };

    assert(zpt_pipeline_validate(&pipeline) == 0);
    assert(zpt_pipeline_validate(&pipeline) == -EALREADY);
    assert(sink_state.resets == 1 && sink_state.last_reset == ZPT_RESET_INITIALIZATION);
    assert(zpt_pipeline_activate(&pipeline, ZPT_RESET_PIPELINE_ENTERED) == 0);
    assert(sink_state.activates == 1);

    struct zpt_signal input = raw_signal(100, 3, -4);
    struct zpt_pipeline_result result;
    assert(zpt_pipeline_push(&pipeline, &input, &result) == 0);
    assert(result.dispatches == 3 && result.outputs == 1);
    assert(sink_state.output_count == 1);
    const struct zpt_signal *output = &sink_state.outputs[0];
    assert(output->kind == ZPT_SIGNAL_POINTER_DELTA);
    assert(output->data.fixed_vector.x == zpt_fixed_from_int(6));
    assert(output->data.fixed_vector.y == zpt_fixed_from_int(-8));
    assert(output->metadata.observed_at_ms == 100);
    assert(output->metadata.sample_span_us == 8000);
    assert(output->metadata.flags == ZPT_SIGNAL_FLAG_LOCAL);
    assert(output->metadata.source_id == 7 && output->metadata.sequence == 42);
    assert(output->metadata.resolution_cpi == 700);
}

static void test_raw_pointer_identity_stage(void) {
    struct zpt_stage stage_storage[] = {
        {.stable_id = "identity", .api = &zpt_raw_pointer_identity_stage_api},
    };
    struct zpt_stage *stages[] = {&stage_storage[0]};
    struct capture_sink_state sink_state = {0};
    struct zpt_sink sink = {
        .stable_id = "cursor",
        .api = &pointer_sink_api,
        .state = &sink_state,
    };
    struct zpt_pipeline pipeline = {
        .stable_id = "identity-cursor",
        .input_kind = ZPT_SIGNAL_RAW_MOTION,
        .stages = stages,
        .stage_count = ARRAY_SIZE(stages),
        .sink = &sink,
        .dispatch_budget = 4,
    };
    assert(zpt_pipeline_validate(&pipeline) == 0);
    assert(zpt_pipeline_activate(&pipeline, ZPT_RESET_PIPELINE_ENTERED) == 0);

    struct zpt_pipeline_result result;
    struct zpt_signal input = raw_signal(25, -12, 9);
    assert(zpt_pipeline_push(&pipeline, &input, &result) == 0);
    assert(result.dispatches == 2 && result.outputs == 1);
    assert(sink_state.output_count == 1);
    assert(sink_state.outputs[0].kind == ZPT_SIGNAL_POINTER_DELTA);
    assert(sink_state.outputs[0].data.fixed_vector.x == zpt_fixed_from_int(-12));
    assert(sink_state.outputs[0].data.fixed_vector.y == zpt_fixed_from_int(9));
    assert(sink_state.outputs[0].metadata.source_id == input.metadata.source_id);

    input.data.raw_motion.x_counts = INT64_MAX;
    assert(zpt_pipeline_push(&pipeline, &input, &result) == -ERANGE);
    assert(result.outputs == 0 && sink_state.output_count == 1);
}

static void test_validation_rejects_incompatible_and_shared_state(void) {
    struct capture_sink_state sink_state = {0};
    struct zpt_sink sink = {
        .stable_id = "normalized-sink",
        .api = &normalized_sink_api,
        .state = &sink_state,
    };
    struct zpt_stage incompatible_storage[] = {
        {.stable_id = "pointer-map", .api = &pointer_map_api},
    };
    struct zpt_stage *incompatible[] = {&incompatible_storage[0]};
    struct zpt_pipeline bad_types = {
        .stable_id = "bad-types",
        .input_kind = ZPT_SIGNAL_RAW_MOTION,
        .stages = incompatible,
        .stage_count = ARRAY_SIZE(incompatible),
        .sink = &sink,
        .dispatch_budget = 8,
    };
    assert(zpt_pipeline_validate(&bad_types) == -EINVAL);

    struct zpt_stage *null_stage[] = {NULL};
    struct zpt_pipeline missing_stage = {
        .stable_id = "missing-stage",
        .input_kind = ZPT_SIGNAL_NORMALIZED_MOTION,
        .stages = null_stage,
        .stage_count = ARRAY_SIZE(null_stage),
        .sink = &sink,
        .dispatch_budget = 8,
    };
    assert(zpt_pipeline_validate(&missing_stage) == -EINVAL);

    struct buffered_stage_state shared_state = {0};
    struct zpt_stage duplicated_state_storage[] = {
        {.stable_id = "first", .api = &buffered_api, .state = &shared_state},
        {.stable_id = "second", .api = &buffered_api, .state = &shared_state},
    };
    struct zpt_stage *duplicated_state[] = {&duplicated_state_storage[0],
                                            &duplicated_state_storage[1]};
    struct zpt_pipeline bad_state = {
        .stable_id = "bad-state",
        .input_kind = ZPT_SIGNAL_NORMALIZED_MOTION,
        .stages = duplicated_state,
        .stage_count = ARRAY_SIZE(duplicated_state),
        .sink = &sink,
        .dispatch_budget = 8,
    };
    assert(zpt_pipeline_validate(&bad_state) == -EBUSY);

    struct buffered_stage_state owned_state = {0};
    struct zpt_stage owned_stage = {
        .stable_id = "owned",
        .api = &buffered_api,
        .state = &owned_state,
    };
    struct zpt_stage *owned_stages[] = {&owned_stage};
    struct zpt_pipeline first_owner = {
        .stable_id = "first-owner",
        .input_kind = ZPT_SIGNAL_NORMALIZED_MOTION,
        .stages = owned_stages,
        .stage_count = ARRAY_SIZE(owned_stages),
        .sink = &sink,
        .dispatch_budget = 8,
    };
    struct zpt_pipeline second_owner = first_owner;
    second_owner.stable_id = "second-owner";
    assert(zpt_pipeline_validate(&first_owner) == 0);
    assert(zpt_pipeline_validate(&second_owner) == -EBUSY);
}

static void test_zero_many_and_dispatch_budget(void) {
    struct zpt_stage stage_storage[] = {
        {.stable_id = "duplicate", .api = &duplicate_api},
    };
    struct zpt_stage *stages[] = {&stage_storage[0]};
    struct capture_sink_state sink_state = {0};
    struct zpt_sink sink = {
        .stable_id = "normalized-sink",
        .api = &normalized_sink_api,
        .state = &sink_state,
    };
    struct zpt_pipeline pipeline = {
        .stable_id = "duplicate-pipeline",
        .input_kind = ZPT_SIGNAL_NORMALIZED_MOTION,
        .stages = stages,
        .stage_count = ARRAY_SIZE(stages),
        .sink = &sink,
        .dispatch_budget = 3,
    };
    assert(zpt_pipeline_validate(&pipeline) == 0);
    assert(zpt_pipeline_activate(&pipeline, ZPT_RESET_PIPELINE_ENTERED) == 0);

    struct zpt_signal input = normalized_signal(10, 1, 2);
    struct zpt_pipeline_result result;
    assert(zpt_pipeline_push(&pipeline, &input, &result) == 0);
    assert(result.dispatches == 3 && result.outputs == 2);
    assert(sink_state.output_count == 2);

    pipeline.dispatch_budget = 2;
    assert(zpt_pipeline_push(&pipeline, &input, &result) == -E2BIG);
    assert(result.dispatches == 2 && result.outputs == 1);
}

static void test_deadline_flush_and_wrap(void) {
    struct buffered_stage_state buffer = {.delay_ms = 20};
    struct zpt_stage stage_storage[] = {
        {.stable_id = "buffer", .api = &buffered_api, .state = &buffer},
    };
    struct zpt_stage *stages[] = {&stage_storage[0]};
    struct capture_sink_state sink_state = {0};
    struct zpt_sink sink = {
        .stable_id = "normalized-sink",
        .api = &normalized_sink_api,
        .state = &sink_state,
    };
    struct zpt_pipeline pipeline = {
        .stable_id = "delayed",
        .input_kind = ZPT_SIGNAL_NORMALIZED_MOTION,
        .stages = stages,
        .stage_count = ARRAY_SIZE(stages),
        .sink = &sink,
        .dispatch_budget = 8,
    };
    assert(zpt_pipeline_validate(&pipeline) == 0);
    assert(zpt_pipeline_activate(&pipeline, ZPT_RESET_PIPELINE_ENTERED) == 0);

    struct zpt_pipeline_result result;
    struct zpt_signal input = normalized_signal(100, 4, -2);
    assert(zpt_pipeline_push(&pipeline, &input, &result) == 0);
    assert(result.outputs == 0);
    uint32_t deadline;
    assert(zpt_pipeline_next_deadline(&pipeline, 100, &deadline) && deadline == 120);
    assert(zpt_pipeline_flush(&pipeline, 119, &result) == 0 && result.outputs == 0);
    assert(zpt_pipeline_flush(&pipeline, 120, &result) == 0 && result.outputs == 1);
    assert(!zpt_pipeline_next_deadline(&pipeline, 120, &deadline));

    input = normalized_signal(UINT32_MAX - 5U, 1, 1);
    assert(zpt_pipeline_push(&pipeline, &input, &result) == 0);
    assert(zpt_pipeline_next_deadline(&pipeline, UINT32_MAX - 5U, &deadline));
    assert(deadline == 14U);
    assert(zpt_pipeline_flush(&pipeline, 13U, &result) == 0 && result.outputs == 0);
    assert(zpt_pipeline_flush(&pipeline, 14U, &result) == 0 && result.outputs == 1);
}

static void test_lifecycle_emits_then_resets(void) {
    struct buffered_stage_state buffer = {
        .emit_on_deactivate = true,
        .delay_ms = 100,
    };
    struct zpt_stage stage_storage[] = {
        {.stable_id = "buffer", .api = &buffered_api, .state = &buffer},
    };
    struct zpt_stage *stages[] = {&stage_storage[0]};
    struct capture_sink_state sink_state = {0};
    struct zpt_sink sink = {
        .stable_id = "normalized-sink",
        .api = &normalized_sink_api,
        .state = &sink_state,
    };
    struct zpt_pipeline pipeline = {
        .stable_id = "lifecycle",
        .input_kind = ZPT_SIGNAL_NORMALIZED_MOTION,
        .stages = stages,
        .stage_count = ARRAY_SIZE(stages),
        .sink = &sink,
        .dispatch_budget = 8,
    };
    assert(zpt_pipeline_validate(&pipeline) == 0);
    assert(zpt_pipeline_activate(&pipeline, ZPT_RESET_PIPELINE_ENTERED) == 0);
    assert(buffer.activates == 1);

    struct zpt_signal input = normalized_signal(50, 9, 3);
    struct zpt_pipeline_result result;
    assert(zpt_pipeline_push(&pipeline, &input, &result) == 0 && result.outputs == 0);
    assert(zpt_pipeline_deactivate(&pipeline, 60, ZPT_RESET_PIPELINE_LEFT, &result) == 0);
    assert(result.outputs == 1);
    assert(buffer.deactivates == 1);
    assert(buffer.last_reset == ZPT_RESET_PIPELINE_LEFT && !buffer.have_pending);
    assert(sink_state.deactivates == 1 && sink_state.last_reset == ZPT_RESET_PIPELINE_LEFT);
    assert(!pipeline.active);
    result = (struct zpt_pipeline_result){.dispatches = 99, .outputs = 99};
    assert(zpt_pipeline_push(&pipeline, &input, &result) == -EACCES);
    assert(result.dispatches == 0 && result.outputs == 0);
    uint32_t deadline;
    assert(!zpt_pipeline_next_deadline(&pipeline, 60, &deadline));
}

static void test_runtime_rejects_wrong_stage_output(void) {
    struct zpt_stage stage_storage[] = {
        {.stable_id = "wrong-kind", .api = &wrong_kind_api},
    };
    struct zpt_stage *stages[] = {&stage_storage[0]};
    struct capture_sink_state sink_state = {0};
    struct zpt_sink sink = {
        .stable_id = "normalized-sink",
        .api = &normalized_sink_api,
        .state = &sink_state,
    };
    struct zpt_pipeline pipeline = {
        .stable_id = "wrong-output",
        .input_kind = ZPT_SIGNAL_RAW_MOTION,
        .stages = stages,
        .stage_count = ARRAY_SIZE(stages),
        .sink = &sink,
        .dispatch_budget = 8,
    };
    assert(zpt_pipeline_validate(&pipeline) == 0);
    assert(zpt_pipeline_activate(&pipeline, ZPT_RESET_PIPELINE_ENTERED) == 0);

    struct zpt_signal input = raw_signal(1, 1, 1);
    struct zpt_pipeline_result result;
    assert(zpt_pipeline_push(&pipeline, &input, &result) == -EPROTOTYPE);
    assert(result.outputs == 0 && sink_state.output_count == 0);
}

int main(void) {
    test_typed_pipeline_and_metadata();
    test_raw_pointer_identity_stage();
    test_validation_rejects_incompatible_and_shared_state();
    test_zero_many_and_dispatch_budget();
    test_deadline_flush_and_wrap();
    test_lifecycle_emits_then_resets();
    test_runtime_rejects_wrong_stage_output();
    puts("pipeline tests passed");
    return 0;
}
