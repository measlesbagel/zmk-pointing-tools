/* SPDX-License-Identifier: MIT */

#include <assert.h>
#include <errno.h>
#include <stdio.h>

#include <zmk/pointing_tools/core/fixed.h>
#include <zmk/pointing_tools/core/pipeline.h>
#include <zmk/pointing_tools/policy/suppression.h>
#include <zmk/pointing_tools/stage/axis_constraint.h>
#include <zmk/pointing_tools/stage/axis_intent.h>
#include <zmk/pointing_tools/stage/resolution_normalize.h>
#include <zmk/pointing_tools/stage/scroll_batcher.h>
#include <zmk/pointing_tools/stage/text_nav.h>

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
    .type_id = "capture",
    .accepted_kinds =
        ZPT_SIGNAL_KIND_MASK(ZPT_SIGNAL_SCROLL_STEPS) | ZPT_SIGNAL_KIND_MASK(ZPT_SIGNAL_ACTION),
    .emit = capture_emit,
};

struct suppression_state {
    bool active;
};

static bool suppression_active(void *context, const struct zpt_signal *signal, uint32_t now_ms) {
    (void)signal;
    (void)now_ms;
    return ((struct suppression_state *)context)->active;
}

struct scroll_fixture {
    struct suppression_state suppression_state;
    struct zpt_suppression_policy suppression;
    struct zpt_axis_intent_stage_config intent_config;
    struct zpt_axis_intent_stage_state intent_state;
    struct zpt_axis_constraint_config constraint_config;
    struct zpt_axis_constraint_state constraint_state;
    struct zpt_scroll_batcher_config batcher_config;
    struct zpt_scroll_batcher_state batcher_state;
    struct zpt_stage intent_stage;
    struct zpt_stage constraint_stage;
    struct zpt_stage batcher_stage;
    struct zpt_stage *stages[3];
    struct capture_state capture;
    struct zpt_sink sink;
    struct zpt_pipeline pipeline;
};

static void scroll_fixture_init(struct scroll_fixture *fixture) {
    *fixture = (struct scroll_fixture){0};
    fixture->suppression = (struct zpt_suppression_policy){
        .is_suppressed = suppression_active,
        .context = &fixture->suppression_state,
    };
    fixture->intent_config = (struct zpt_axis_intent_stage_config){
        .settings =
            {
                .engage_ratio_percent = 300,
                .release_ratio_percent = 180,
                /* 16 counts at 700 CPI in Q16 millimetres. */
                .activation_distance = (zpt_fixed_t)16 * ZPT_FIXED_ONE * 254 / 7000,
                .window_ms = 64,
            },
        .policy = ZPT_AXIS_POLICY_ADAPTIVE,
        .idle_timeout_ms = 120,
        .suppression = &fixture->suppression,
    };
    fixture->constraint_config = (struct zpt_axis_constraint_config){
        .discard_unclassified = false,
        .idle_timeout_ms = 120,
        .suppression = &fixture->suppression,
    };
    fixture->batcher_config = (struct zpt_scroll_batcher_config){
        /* One wheel step per eight counts at 700 CPI in Q16 steps/mm. */
        .steps_per_millimeter = (zpt_fixed_t)3445 * ZPT_FIXED_ONE / 1000,
        .report_interval_ms = 16,
        .suppression = &fixture->suppression,
    };
    fixture->intent_stage = (struct zpt_stage){
        .stable_id = "axis-intent",
        .api = &zpt_axis_intent_stage_api,
        .config = &fixture->intent_config,
        .state = &fixture->intent_state,
    };
    fixture->constraint_stage = (struct zpt_stage){
        .stable_id = "axis-constraint",
        .api = &zpt_axis_constraint_stage_api,
        .config = &fixture->constraint_config,
        .state = &fixture->constraint_state,
    };
    fixture->batcher_stage = (struct zpt_stage){
        .stable_id = "scroll-batcher",
        .api = &zpt_scroll_batcher_stage_api,
        .config = &fixture->batcher_config,
        .state = &fixture->batcher_state,
    };
    fixture->stages[0] = &fixture->intent_stage;
    fixture->stages[1] = &fixture->constraint_stage;
    fixture->stages[2] = &fixture->batcher_stage;
    fixture->sink = (struct zpt_sink){
        .stable_id = "capture",
        .api = &capture_api,
        .state = &fixture->capture,
    };
    fixture->pipeline = (struct zpt_pipeline){
        .stable_id = "scroll-test",
        .input_kind = ZPT_SIGNAL_NORMALIZED_MOTION,
        .stages = fixture->stages,
        .stage_count = 3,
        .sink = &fixture->sink,
        .dispatch_budget = 8,
    };
    assert(zpt_pipeline_validate(&fixture->pipeline) == 0);
    assert(zpt_pipeline_activate(&fixture->pipeline, ZPT_RESET_PIPELINE_ENTERED) == 0);
}

/* 700 CPI sensor: counts to Q16 millimetres via the shared normalizer. */
static zpt_fixed_t counts_to_q16_mm(int32_t counts) {
    zpt_fixed_t millimeters;
    assert(zpt_counts_to_millimeters(counts, 700, &millimeters) == 0);
    return millimeters;
}

static int push_normalized(struct zpt_pipeline *pipeline, struct zpt_pipeline_result *result,
                           uint32_t timestamp, zpt_fixed_t x, zpt_fixed_t y) {
    struct zpt_signal signal = {
        .kind = ZPT_SIGNAL_NORMALIZED_MOTION,
        .metadata = {.observed_at_ms = timestamp},
        .data.fixed_vector = {.x = x, .y = y},
    };
    return zpt_pipeline_push(pipeline, &signal, result);
}

static int push_annotated(struct zpt_pipeline *pipeline, struct zpt_pipeline_result *result,
                          uint32_t timestamp, zpt_fixed_t x, zpt_fixed_t y, uint8_t axis_intent) {
    struct zpt_signal signal = {
        .kind = ZPT_SIGNAL_NORMALIZED_MOTION,
        .metadata = {.observed_at_ms = timestamp},
        .annotations = {.axis_intent = axis_intent},
        .data.fixed_vector = {.x = x, .y = y},
    };
    return zpt_pipeline_push(pipeline, &signal, result);
}

static void test_suppression_clears_buffers_and_keeps_remainder(void) {
    struct scroll_fixture fixture;
    scroll_fixture_init(&fixture);

    struct zpt_pipeline_result result;
    assert(push_normalized(&fixture.pipeline, &result, 0, counts_to_q16_mm(10),
                           counts_to_q16_mm(1)) == 0);
    assert(push_normalized(&fixture.pipeline, &result, 8, counts_to_q16_mm(10),
                           counts_to_q16_mm(1)) == 0);
    assert(fixture.batcher_state.pending_x == counts_to_q16_mm(20));
    assert(fixture.constraint_state.have_undecided == false);

    /* A flush produces two steps and a remainder, then suppression clears
     * the pending motion while the remainder survives. */
    assert(zpt_pipeline_flush(&fixture.pipeline, 16, &result) == 0);
    assert(fixture.capture.outputs == 1);
    assert(fixture.capture.signal.kind == ZPT_SIGNAL_SCROLL_STEPS);
    assert(fixture.capture.signal.data.delta.x == 2);
    assert(fixture.batcher_state.remainder_x ==
           zpt_fixed_multiply(counts_to_q16_mm(20), fixture.batcher_config.steps_per_millimeter) -
               2 * ZPT_FIXED_ONE);

    fixture.suppression_state.active = true;
    assert(push_normalized(&fixture.pipeline, &result, 20, counts_to_q16_mm(10), 0) == 0);
    assert(fixture.batcher_state.pending_x == 0);
    assert(fixture.batcher_state.remainder_x ==
           zpt_fixed_multiply(counts_to_q16_mm(20), fixture.batcher_config.steps_per_millimeter) -
               2 * ZPT_FIXED_ONE);
    assert(fixture.intent_state.estimator.horizontal_energy == 0);
}

static void test_discard_unclassified_clears_undecided_on_expiry(void) {
    struct scroll_fixture fixture;
    scroll_fixture_init(&fixture);
    fixture.constraint_config.discard_unclassified = true;

    struct zpt_pipeline_result result;
    assert(push_normalized(&fixture.pipeline, &result, 0, counts_to_q16_mm(3),
                           counts_to_q16_mm(3)) == 0);
    assert(fixture.constraint_state.have_undecided);
    assert(push_normalized(&fixture.pipeline, &result, 8, counts_to_q16_mm(3),
                           counts_to_q16_mm(3)) == 0);
    assert(fixture.constraint_state.undecided_x == counts_to_q16_mm(6));

    /* Idle expiry folds or discards the undecided motion. */
    uint32_t deadline;
    assert(zpt_pipeline_next_deadline(&fixture.pipeline, 128, &deadline));
    assert(zpt_pipeline_flush(&fixture.pipeline, 128, &result) == 0);
    assert(!fixture.constraint_state.have_undecided);
    /* Discarded: nothing reached the batcher. */
    assert(fixture.batcher_state.pending_x == 0);
    assert(fixture.capture.outputs == 0);
}

static void test_discard_off_folds_undecided_on_expiry(void) {
    struct scroll_fixture fixture;
    scroll_fixture_init(&fixture);

    struct zpt_pipeline_result result;
    assert(push_normalized(&fixture.pipeline, &result, 0, counts_to_q16_mm(3),
                           counts_to_q16_mm(3)) == 0);
    assert(push_normalized(&fixture.pipeline, &result, 8, counts_to_q16_mm(3),
                           counts_to_q16_mm(3)) == 0);

    uint32_t deadline;
    assert(zpt_pipeline_next_deadline(&fixture.pipeline, 128, &deadline));
    assert(zpt_pipeline_flush(&fixture.pipeline, 128, &result) == 0);
    /* Folded as free motion into the batcher; the batcher's own due flush
     * scaled it to zero steps and kept the fractional remainder. */
    assert(fixture.batcher_state.pending_x == 0);
    assert(fixture.batcher_state.remainder_x ==
           zpt_fixed_multiply(counts_to_q16_mm(6), fixture.batcher_config.steps_per_millimeter));
    assert(fixture.batcher_state.pending_y == 0);
    assert(fixture.batcher_state.remainder_y ==
           zpt_fixed_multiply(counts_to_q16_mm(6), fixture.batcher_config.steps_per_millimeter));
}

static void test_text_nav_emits_actions_and_resets_on_idle(void) {
    struct zpt_text_nav_config config = {
        .horizontal_threshold = counts_to_q16_mm(75),
        .vertical_threshold = counts_to_q16_mm(75),
        .idle_timeout_ms = 40,
    };
    struct zpt_text_nav_state state = {0};
    struct zpt_stage stage = {
        .stable_id = "text-nav",
        .api = &zpt_text_nav_stage_api,
        .config = &config,
        .state = &state,
    };
    struct zpt_stage *stages[1] = {&stage};
    struct capture_state capture = {0};
    struct zpt_sink sink = {
        .stable_id = "capture",
        .api = &capture_api,
        .state = &capture,
    };
    struct zpt_pipeline pipeline = {
        .stable_id = "text-test",
        .input_kind = ZPT_SIGNAL_NORMALIZED_MOTION,
        .stages = stages,
        .stage_count = 1,
        .sink = &sink,
        .dispatch_budget = 4,
    };
    assert(zpt_pipeline_validate(&pipeline) == 0);
    assert(zpt_pipeline_activate(&pipeline, ZPT_RESET_PIPELINE_ENTERED) == 0);

    struct zpt_pipeline_result result;
    assert(push_annotated(&pipeline, &result, 0, counts_to_q16_mm(20), counts_to_q16_mm(3),
                          ZPT_AXIS_INTENT_HORIZONTAL) == 0);
    assert(push_annotated(&pipeline, &result, 8, counts_to_q16_mm(25), counts_to_q16_mm(-2),
                          ZPT_AXIS_INTENT_HORIZONTAL) == 0);
    assert(push_annotated(&pipeline, &result, 16, counts_to_q16_mm(30), counts_to_q16_mm(4),
                          ZPT_AXIS_INTENT_HORIZONTAL) == 0);
    assert(capture.outputs == 1);
    assert(capture.signal.kind == ZPT_SIGNAL_ACTION);
    assert(capture.signal.data.action.id == ZPT_TEXT_NAV_RIGHT);

    /* Idle gap resets the gesture accumulator. */
    assert(push_annotated(&pipeline, &result, 80, counts_to_q16_mm(3), counts_to_q16_mm(-41),
                          ZPT_AXIS_INTENT_VERTICAL) == 0);
    assert(push_annotated(&pipeline, &result, 88, counts_to_q16_mm(-2), counts_to_q16_mm(-35),
                          ZPT_AXIS_INTENT_VERTICAL) == 0);
    assert(capture.outputs == 2);
    assert(capture.signal.data.action.id == ZPT_TEXT_NAV_UP);
}

int main(void) {
    test_suppression_clears_buffers_and_keeps_remainder();
    test_discard_unclassified_clears_undecided_on_expiry();
    test_discard_off_folds_undecided_on_expiry();
    test_text_nav_emits_actions_and_resets_on_idle();
    puts("scroll and text pipeline tests passed");
    return 0;
}
