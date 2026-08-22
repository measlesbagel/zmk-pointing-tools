/* SPDX-License-Identifier: MIT */

#include <assert.h>
#include <errno.h>
#include <stdio.h>

#include <zmk/pointing_tools/core/pipeline.h>
#include <zmk/pointing_tools/stage/axis_intent.h>
#include <zmk/pointing_tools/stage/resolution_normalize.h>

static const struct zpt_axis_intent_settings settings = {
    .engage_ratio_percent = 220,
    .release_ratio_percent = 150,
    .activation_distance = 16,
    .window_ms = 64,
};

/* 700 CPI sensor: counts to Q16 millimetres. */
static zpt_fixed_t counts_to_q16_mm(int32_t counts) {
    return ((zpt_fixed_t)counts * ZPT_FIXED_ONE * 254 + 3500) / 7000;
}

/* 16 counts at 700 CPI, matching the count-domain legacy activation. */
static struct zpt_axis_intent_settings stage_settings(void) {
    struct zpt_axis_intent_settings stage = settings;
    stage.activation_distance = counts_to_q16_mm(16);
    return stage;
}

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

struct intent_fixture {
    struct zpt_axis_intent_stage_config config;
    struct zpt_axis_intent_stage_state state;
    struct zpt_stage stage;
    struct zpt_stage *stages[1];
    struct capture_state capture;
    struct zpt_sink sink;
    struct zpt_pipeline pipeline;
};

static void intent_fixture_init(struct intent_fixture *fixture, enum zpt_axis_policy policy) {
    *fixture = (struct intent_fixture){0};
    fixture->config = (struct zpt_axis_intent_stage_config){
        .settings = stage_settings(),
        .policy = policy,
    };
    fixture->stage = (struct zpt_stage){
        .stable_id = "axis-intent",
        .api = &zpt_axis_intent_stage_api,
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
        .stable_id = "axis-intent-test",
        .input_kind = ZPT_SIGNAL_NORMALIZED_MOTION,
        .stages = fixture->stages,
        .stage_count = 1,
        .sink = &fixture->sink,
        .dispatch_budget = 3,
    };
    assert(zpt_pipeline_validate(&fixture->pipeline) == 0);
    assert(zpt_pipeline_activate(&fixture->pipeline, ZPT_RESET_PIPELINE_ENTERED) == 0);
}

static struct zpt_signal normalized_signal(uint32_t timestamp, zpt_fixed_t x, zpt_fixed_t y) {
    return (struct zpt_signal){
        .kind = ZPT_SIGNAL_NORMALIZED_MOTION,
        .metadata = {.observed_at_ms = timestamp},
        .data.fixed_vector = {.x = x, .y = y},
    };
}

static void test_fixed_micrometre_conversion(void) {
    static_assert(ZPT_MICROMETERS_TO_FIXED_MILLIMETERS(0) == 0, "0 um is zero");
    static_assert(ZPT_MICROMETERS_TO_FIXED_MILLIMETERS(1000) == ZPT_FIXED_ONE,
                  "1000 um is one millimetre");
    static_assert(ZPT_MICROMETERS_TO_FIXED_MILLIMETERS(500) == ZPT_FIXED_ONE / 2,
                  "500 um rounds to one millimetre");
    /* Rounds to the nearest micrometre. */
    static_assert(ZPT_MICROMETERS_TO_FIXED_MILLIMETERS(1) == 66, "1 um rounds to 66");
    /* The smoke keymap's 580 um activation distance. */
    static_assert(ZPT_MICROMETERS_TO_FIXED_MILLIMETERS(580) == 38011,
                  "580 um is 38011 fixed units");
}

static void test_strategy_forced_policies(void) {
    struct zpt_axis_intent_state state = {0};
    assert(zpt_axis_intent_estimate(&state, &settings, ZPT_AXIS_POLICY_FREE, 20, 1, 8) ==
           ZPT_AXIS_INTENT_FREE);
    assert(zpt_axis_intent_estimate(&state, &settings, ZPT_AXIS_POLICY_HORIZONTAL, 0, 20, 8) ==
           ZPT_AXIS_INTENT_HORIZONTAL);
    assert(zpt_axis_intent_estimate(&state, &settings, ZPT_AXIS_POLICY_VERTICAL, 20, 0, 8) ==
           ZPT_AXIS_INTENT_VERTICAL);
}

static void test_strategy_cardinal_and_diagonal(void) {
    struct zpt_axis_intent_state state;
    zpt_axis_intent_reset(&state);

    assert(zpt_axis_intent_estimate(&state, &settings, ZPT_AXIS_POLICY_ADAPTIVE, 10, 2, 8) ==
           ZPT_AXIS_INTENT_UNDECIDED);
    assert(zpt_axis_intent_estimate(&state, &settings, ZPT_AXIS_POLICY_ADAPTIVE, 10, 2, 8) ==
           ZPT_AXIS_INTENT_HORIZONTAL);

    zpt_axis_intent_reset(&state);
    assert(zpt_axis_intent_estimate(&state, &settings, ZPT_AXIS_POLICY_ADAPTIVE, 4, 12, 8) ==
           ZPT_AXIS_INTENT_VERTICAL);

    zpt_axis_intent_reset(&state);
    assert(zpt_axis_intent_estimate(&state, &settings, ZPT_AXIS_POLICY_ADAPTIVE, 9, 8, 8) ==
           ZPT_AXIS_INTENT_FREE);
}

static void test_strategy_hysteresis_and_turning(void) {
    struct zpt_axis_intent_state state;
    zpt_axis_intent_reset(&state);

    zpt_axis_intent_estimate(&state, &settings, ZPT_AXIS_POLICY_ADAPTIVE, 20, 2, 8);
    assert(state.intent == ZPT_AXIS_INTENT_HORIZONTAL);

    /* One noisy frame does not immediately break a stable horizontal lock. */
    zpt_axis_intent_estimate(&state, &settings, ZPT_AXIS_POLICY_ADAPTIVE, 7, 6, 8);
    assert(state.intent == ZPT_AXIS_INTENT_HORIZONTAL);

    /* A sustained turn eventually releases and then selects vertical. */
    for (int i = 0; i < 8; i++) {
        zpt_axis_intent_estimate(&state, &settings, ZPT_AXIS_POLICY_ADAPTIVE, 1, 14, 8);
    }
    assert(state.intent == ZPT_AXIS_INTENT_VERTICAL);
}

static void test_strategy_window_expires(void) {
    struct zpt_axis_intent_state state;
    zpt_axis_intent_reset(&state);
    zpt_axis_intent_estimate(&state, &settings, ZPT_AXIS_POLICY_ADAPTIVE, 20, 1, 8);
    assert(state.intent == ZPT_AXIS_INTENT_HORIZONTAL);

    zpt_axis_intent_reset(&state);
    assert(zpt_axis_intent_estimate(&state, &settings, ZPT_AXIS_POLICY_ADAPTIVE, 1, 20, 80) ==
           ZPT_AXIS_INTENT_VERTICAL);
}

static void test_strategy_gap_decays_energy(void) {
    struct zpt_axis_intent_state state;
    zpt_axis_intent_reset(&state);
    zpt_axis_intent_estimate(&state, &settings, ZPT_AXIS_POLICY_ADAPTIVE, 20, 1, 8);
    assert(state.horizontal_energy == 20U);
    assert(zpt_axis_intent_estimate(&state, &settings, ZPT_AXIS_POLICY_ADAPTIVE, 20, 1, 32) ==
           ZPT_AXIS_INTENT_HORIZONTAL);
    /* Half of the window elapsed, so half the energy decayed. */
    assert(state.horizontal_energy == 30U);
}

static void test_strategy_confidence(void) {
    struct zpt_axis_intent_state state;
    zpt_axis_intent_reset(&state);
    assert(zpt_axis_intent_confidence(&state) == 0U);

    zpt_axis_intent_estimate(&state, &settings, ZPT_AXIS_POLICY_ADAPTIVE, 10, 2, 8);
    assert(zpt_axis_intent_confidence(&state) == 0U);
    zpt_axis_intent_estimate(&state, &settings, ZPT_AXIS_POLICY_ADAPTIVE, 10, 2, 8);
    assert(zpt_axis_intent_confidence(&state) > 0U);
    assert(zpt_axis_intent_confidence(&state) <= 100U);

    /* A strongly dominant axis approaches full confidence. */
    struct zpt_axis_intent_state dominant = {
        .horizontal_energy = 1000U, .vertical_energy = 1U, .intent = ZPT_AXIS_INTENT_HORIZONTAL};
    assert(zpt_axis_intent_confidence(&dominant) == 99U);

    struct zpt_axis_intent_state pure = {
        .horizontal_energy = 1000U, .vertical_energy = 0U, .intent = ZPT_AXIS_INTENT_HORIZONTAL};
    assert(zpt_axis_intent_confidence(&pure) == 100U);

    struct zpt_axis_intent_state even = {
        .horizontal_energy = 500U, .vertical_energy = 500U, .intent = ZPT_AXIS_INTENT_HORIZONTAL};
    assert(zpt_axis_intent_confidence(&even) == 50U);
}

static void test_strategy_validation(void) {
    assert(zpt_axis_intent_validate(&settings) == 0);
    struct zpt_axis_intent_settings invalid = settings;
    invalid.engage_ratio_percent = 0;
    assert(zpt_axis_intent_validate(&invalid) < 0);
    invalid = settings;
    invalid.activation_distance = 0;
    assert(zpt_axis_intent_validate(&invalid) < 0);
    invalid = settings;
    invalid.window_ms = 0;
    assert(zpt_axis_intent_validate(&invalid) < 0);
    assert(zpt_axis_intent_validate(NULL) < 0);
}

static void test_stage_populates_annotations(void) {
    struct intent_fixture fixture;
    intent_fixture_init(&fixture, ZPT_AXIS_POLICY_ADAPTIVE);

    struct zpt_pipeline_result result;
    struct zpt_signal signal = normalized_signal(0, counts_to_q16_mm(10), counts_to_q16_mm(2));
    assert(zpt_pipeline_push(&fixture.pipeline, &signal, &result) == 0);
    assert(fixture.capture.outputs == 1);
    assert(fixture.capture.signal.annotations.axis_intent == ZPT_AXIS_INTENT_UNDECIDED);
    assert(fixture.capture.signal.annotations.speed_per_second == 0);

    signal = normalized_signal(8, counts_to_q16_mm(10), counts_to_q16_mm(2));
    assert(zpt_pipeline_push(&fixture.pipeline, &signal, &result) == 0);
    assert(fixture.capture.signal.annotations.axis_intent == ZPT_AXIS_INTENT_HORIZONTAL);
    assert(fixture.capture.signal.annotations.axis_confidence_percent > 0U);
    assert(fixture.capture.signal.annotations.axis_confidence_percent <= 100U);
    /* 12 counts over 8 ms at 700 CPI is 54.4 mm/s in Q16. */
    assert(fixture.capture.signal.annotations.speed_per_second == counts_to_q16_mm(12) * 1000 / 8);
}

static void test_stage_forced_policy_annotations(void) {
    struct intent_fixture fixture;
    intent_fixture_init(&fixture, ZPT_AXIS_POLICY_HORIZONTAL);

    struct zpt_pipeline_result result;
    struct zpt_signal signal = normalized_signal(0, counts_to_q16_mm(2), counts_to_q16_mm(20));
    assert(zpt_pipeline_push(&fixture.pipeline, &signal, &result) == 0);
    assert(fixture.capture.signal.annotations.axis_intent == ZPT_AXIS_INTENT_HORIZONTAL);
    assert(fixture.capture.signal.annotations.axis_confidence_percent == 100U);
}

static void test_stage_reset_clears_state(void) {
    struct intent_fixture fixture;
    intent_fixture_init(&fixture, ZPT_AXIS_POLICY_ADAPTIVE);

    struct zpt_pipeline_result result;
    struct zpt_signal signal = normalized_signal(0, counts_to_q16_mm(10), counts_to_q16_mm(2));
    zpt_pipeline_push(&fixture.pipeline, &signal, &result);
    signal = normalized_signal(8, counts_to_q16_mm(10), counts_to_q16_mm(2));
    zpt_pipeline_push(&fixture.pipeline, &signal, &result);
    assert(fixture.capture.signal.annotations.axis_intent == ZPT_AXIS_INTENT_HORIZONTAL);

    zpt_pipeline_reset(&fixture.pipeline, ZPT_RESET_PIPELINE_LEFT);
    /* After the reset the first frame starts fresh: undecided again. */
    signal = normalized_signal(1000, counts_to_q16_mm(10), counts_to_q16_mm(2));
    zpt_pipeline_push(&fixture.pipeline, &signal, &result);
    assert(fixture.capture.signal.annotations.axis_intent == ZPT_AXIS_INTENT_UNDECIDED);
}

int main(void) {
    test_fixed_micrometre_conversion();
    test_strategy_forced_policies();
    test_strategy_cardinal_and_diagonal();
    test_strategy_hysteresis_and_turning();
    test_strategy_window_expires();
    test_strategy_gap_decays_energy();
    test_strategy_confidence();
    test_strategy_validation();
    test_stage_populates_annotations();
    test_stage_forced_policy_annotations();
    test_stage_reset_clears_state();
    puts("axis intent stage tests passed");
    return 0;
}
