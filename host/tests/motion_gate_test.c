/* SPDX-License-Identifier: MIT */

#include <assert.h>
#include <errno.h>
#include <stdio.h>

#include <zmk/pointing_tools/core/pipeline.h>
#include <zmk/pointing_tools/stage/motion_gate/coherent_displacement.h>

static const struct zpt_coherent_displacement_settings count_settings = {
    .enabled = true,
    .activation_distance = 6,
    .coherence_percent = 60,
    .qualification_timeout_ms = 160,
    .idle_timeout_ms = 120,
};

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
    .type_id = "normalized-capture",
    .accepted_kinds = ZPT_SIGNAL_KIND_MASK(ZPT_SIGNAL_NORMALIZED_MOTION),
    .emit = capture_emit,
};

struct suppression_context {
    bool active;
};

static bool suppression_active(void *context, const struct zpt_signal *signal, uint32_t now_ms) {
    (void)signal;
    (void)now_ms;
    return ((struct suppression_context *)context)->active;
}

struct gate_fixture {
    struct suppression_context suppression_context;
    struct zpt_motion_gate_suppression_policy suppression;
    struct zpt_coherent_displacement_stage_config config;
    struct zpt_coherent_displacement_stage_state state;
    struct zpt_stage stage;
    struct zpt_stage *stages[1];
    struct capture_state capture;
    struct zpt_sink sink;
    struct zpt_pipeline pipeline;
};

static void gate_fixture_init(struct gate_fixture *fixture, bool enabled, int64_t activation) {
    *fixture = (struct gate_fixture){0};
    fixture->suppression = (struct zpt_motion_gate_suppression_policy){
        .is_suppressed = suppression_active,
        .context = &fixture->suppression_context,
    };
    fixture->config = (struct zpt_coherent_displacement_stage_config){
        .settings =
            {
                .enabled = enabled,
                .activation_distance = activation,
                .coherence_percent = 60,
                .qualification_timeout_ms = 160,
                .idle_timeout_ms = 120,
            },
        .suppression = &fixture->suppression,
    };
    fixture->stage = (struct zpt_stage){
        .stable_id = "motion-gate",
        .api = &zpt_coherent_displacement_stage_api,
        .config = &fixture->config,
        .state = &fixture->state,
    };
    fixture->stages[0] = &fixture->stage;
    fixture->sink = (struct zpt_sink){
        .stable_id = "capture",
        .api = &capture_api,
        .state = &fixture->capture,
    };
    fixture->pipeline = (struct zpt_pipeline){
        .stable_id = "gate-test",
        .input_kind = ZPT_SIGNAL_NORMALIZED_MOTION,
        .stages = fixture->stages,
        .stage_count = 1,
        .sink = &fixture->sink,
        .dispatch_budget = 3,
    };
    assert(zpt_pipeline_validate(&fixture->pipeline) == 0);
    assert(zpt_pipeline_activate(&fixture->pipeline, ZPT_RESET_PIPELINE_ENTERED) == 0);
}

static struct zpt_signal normalized_signal(uint32_t timestamp, uint16_t sequence, int64_t x,
                                           int64_t y) {
    return (struct zpt_signal){
        .kind = ZPT_SIGNAL_NORMALIZED_MOTION,
        .metadata =
            {
                .observed_at_ms = timestamp,
                .sample_span_us = 8000,
                .flags = ZPT_SIGNAL_FLAG_LOCAL,
                .source_id = 7,
                .sequence = sequence,
                .resolution_cpi = 700,
            },
        .data.fixed_vector = {.x = x, .y = y},
    };
}

static void test_strategy_preserves_legacy_qualification_behavior(void) {
    struct zpt_coherent_displacement_state state;
    zpt_coherent_displacement_reset(&state);
    struct zpt_coherent_displacement_result result;
    for (int index = 0; index < 5; index++) {
        result = zpt_coherent_displacement_update(&state, &count_settings, 1, 0, index * 8, false);
        assert(result.x == 0 && result.y == 0);
        assert(result.phase == ZPT_MOTION_GATE_PENDING);
    }
    result = zpt_coherent_displacement_update(&state, &count_settings, 1, 0, 40, false);
    assert(result.qualified);
    assert(result.x == 6 && result.y == 0);
    result = zpt_coherent_displacement_update(&state, &count_settings, 1, -1, 48, false);
    assert(result.x == 1 && result.y == -1);

    zpt_coherent_displacement_reset(&state);
    for (int index = 0; index < 12; index++) {
        int64_t x = index % 2 == 0 ? 2 : -2;
        result = zpt_coherent_displacement_update(&state, &count_settings, x, 0, index * 8, false);
        assert(result.phase == ZPT_MOTION_GATE_PENDING);
        assert(!result.qualified);
    }
}

static void test_pipeline_releases_complete_normalized_evidence(void) {
    struct gate_fixture fixture;
    gate_fixture_init(&fixture, true, 6 * ZPT_FIXED_ONE);

    for (uint16_t index = 0; index < 6; index++) {
        struct zpt_signal signal = normalized_signal(index * 8U, index, ZPT_FIXED_ONE, 0);
        struct zpt_pipeline_result result;
        assert(zpt_pipeline_push(&fixture.pipeline, &signal, &result) == 0);
        assert(result.outputs == (index == 5 ? 1U : 0U));
    }

    assert(fixture.capture.outputs == 1);
    assert(fixture.capture.signal.data.fixed_vector.x == 6 * ZPT_FIXED_ONE);
    assert(fixture.capture.signal.data.fixed_vector.y == 0);
    assert(fixture.capture.signal.metadata.sample_span_us == 48000);
    assert(fixture.capture.signal.metadata.observed_at_ms == 40);
    assert(fixture.capture.signal.metadata.sequence == 5);
    assert((fixture.capture.signal.metadata.flags & ZPT_SIGNAL_FLAG_COALESCED) != 0U);

    struct zpt_signal active = normalized_signal(48, 6, ZPT_FIXED_ONE, -ZPT_FIXED_ONE);
    struct zpt_pipeline_result result;
    assert(zpt_pipeline_push(&fixture.pipeline, &active, &result) == 0);
    assert(result.outputs == 1);
    assert(fixture.capture.signal.data.fixed_vector.x == ZPT_FIXED_ONE);
    assert(fixture.capture.signal.data.fixed_vector.y == -ZPT_FIXED_ONE);
    assert(fixture.capture.signal.metadata.sample_span_us == 8000);
}

static void test_external_suppression_drops_and_resets(void) {
    struct gate_fixture fixture;
    gate_fixture_init(&fixture, true, ZPT_FIXED_ONE);

    struct zpt_signal signal = normalized_signal(0, 0, ZPT_FIXED_ONE, 0);
    struct zpt_pipeline_result result;
    assert(zpt_pipeline_push(&fixture.pipeline, &signal, &result) == 0);
    assert(result.outputs == 1 && fixture.state.strategy.active);

    fixture.suppression_context.active = true;
    signal = normalized_signal(8, 1, 4 * ZPT_FIXED_ONE, ZPT_FIXED_ONE);
    assert(zpt_pipeline_push(&fixture.pipeline, &signal, &result) == 0);
    assert(result.outputs == 0);
    assert(!fixture.state.strategy.active && fixture.state.strategy.sample_count == 0);

    fixture.suppression_context.active = false;
    signal = normalized_signal(48, 2, ZPT_FIXED_ONE, 0);
    assert(zpt_pipeline_push(&fixture.pipeline, &signal, &result) == 0);
    assert(result.outputs == 1);
}

static void test_deadlines_and_explicit_lifecycle_clear_state(void) {
    struct gate_fixture fixture;
    gate_fixture_init(&fixture, true, ZPT_FIXED_ONE);

    struct zpt_signal signal = normalized_signal(0, 0, ZPT_FIXED_ONE, 0);
    struct zpt_pipeline_result result;
    assert(zpt_pipeline_push(&fixture.pipeline, &signal, &result) == 0);
    uint32_t deadline;
    assert(zpt_pipeline_next_deadline(&fixture.pipeline, 0, &deadline));
    assert(deadline == 120);

    assert(zpt_pipeline_flush(&fixture.pipeline, 119, &result) == 0);
    assert(fixture.state.strategy.active);
    assert(zpt_pipeline_flush(&fixture.pipeline, 120, &result) == 0);
    assert(!fixture.state.strategy.active);
    assert(!zpt_pipeline_next_deadline(&fixture.pipeline, 120, &deadline));

    signal = normalized_signal(121, 1, ZPT_FIXED_ONE / 4, 0);
    assert(zpt_pipeline_push(&fixture.pipeline, &signal, &result) == 0);
    assert(fixture.state.strategy.sample_count == 1);
    zpt_pipeline_reset(&fixture.pipeline, ZPT_RESET_SETTINGS_CHANGED);
    assert(fixture.state.strategy.sample_count == 0);
    assert(!fixture.state.have_pending_evidence);

    signal = normalized_signal(200, 2, ZPT_FIXED_ONE / 4, 0);
    assert(zpt_pipeline_push(&fixture.pipeline, &signal, &result) == 0);
    signal = normalized_signal(300, 3, ZPT_FIXED_ONE / 4, 0);
    assert(zpt_pipeline_push(&fixture.pipeline, &signal, &result) == 0);
    assert(zpt_pipeline_next_deadline(&fixture.pipeline, 300, &deadline));
    assert(deadline == 360);
    assert(zpt_pipeline_flush(&fixture.pipeline, 359, &result) == 0);
    assert(fixture.state.strategy.sample_count == 2);
    assert(zpt_pipeline_flush(&fixture.pipeline, 360, &result) == 0);
    assert(fixture.state.strategy.sample_count == 0);
    assert(!zpt_pipeline_next_deadline(&fixture.pipeline, 360, &deadline));
}

static void test_disabled_stage_bypasses_and_invalid_config_fails_activation(void) {
    struct gate_fixture fixture;
    gate_fixture_init(&fixture, false, ZPT_FIXED_ONE);
    struct zpt_signal signal = normalized_signal(5, 1, -2 * ZPT_FIXED_ONE, 3 * ZPT_FIXED_ONE);
    struct zpt_pipeline_result result;
    assert(zpt_pipeline_push(&fixture.pipeline, &signal, &result) == 0);
    assert(result.outputs == 1);
    assert(fixture.capture.signal.data.fixed_vector.x == -2 * ZPT_FIXED_ONE);
    assert(fixture.capture.signal.data.fixed_vector.y == 3 * ZPT_FIXED_ONE);

    struct zpt_coherent_displacement_settings invalid = count_settings;
    invalid.activation_distance = 0;
    assert(zpt_coherent_displacement_validate(&invalid) == -EINVAL);
    invalid = count_settings;
    invalid.coherence_percent = 101;
    assert(zpt_coherent_displacement_validate(&invalid) == -EINVAL);
}

int main(void) {
    test_strategy_preserves_legacy_qualification_behavior();
    test_pipeline_releases_complete_normalized_evidence();
    test_external_suppression_drops_and_resets();
    test_deadlines_and_explicit_lifecycle_clear_state();
    test_disabled_stage_bypasses_and_invalid_config_fails_activation();
    puts("motion gate tests passed");
    return 0;
}
