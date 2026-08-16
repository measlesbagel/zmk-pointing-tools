/* SPDX-License-Identifier: MIT */

#include <assert.h>
#include <errno.h>
#include <stdio.h>

#include <zmk/pointing_tools/core/pipeline.h>
#include <zmk/pointing_tools/policy/suppression.h>
#include <zmk/pointing_tools/stage/axis_constraint.h>
#include <zmk/pointing_tools/stage/axis_intent.h>
#include <zmk/pointing_tools/stage/motion_gate/coherent_displacement.h>
#include <zmk/pointing_tools/stage/scroll_batcher.h>
#include <zmk/pointing_tools/stage/text_nav.h>

struct event_record {
    enum zpt_stage_event event;
    int64_t value;
    uint32_t now_ms;
};

struct observer_state {
    struct event_record records[16];
    size_t count;
};

static void record_observer(const struct zpt_stage *stage, enum zpt_stage_event event,
                            int64_t value, uint32_t now_ms, void *user_data) {
    (void)stage;
    struct observer_state *observer = user_data;
    if (observer->count < 16) {
        observer->records[observer->count++] =
            (struct event_record){.event = event, .value = value, .now_ms = now_ms};
    }
}

static bool has_event(const struct observer_state *observer, enum zpt_stage_event event,
                      int64_t value) {
    for (size_t index = 0; index < observer->count; index++) {
        if (observer->records[index].event == event && observer->records[index].value == value) {
            return true;
        }
    }
    return false;
}

struct capture_state {
    uint32_t outputs;
};

static int capture_emit(struct zpt_sink *sink, const struct zpt_signal *signal) {
    (void)signal;
    ((struct capture_state *)sink->state)->outputs++;
    return 0;
}

static const struct zpt_sink_api capture_api = {
    .type_id = "capture",
    .accepted_kinds = ZPT_SIGNAL_KIND_MASK(ZPT_SIGNAL_RAW_MOTION) |
                      ZPT_SIGNAL_KIND_MASK(ZPT_SIGNAL_NORMALIZED_MOTION) |
                      ZPT_SIGNAL_KIND_MASK(ZPT_SIGNAL_SCROLL_STEPS) |
                      ZPT_SIGNAL_KIND_MASK(ZPT_SIGNAL_ACTION),
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

static struct zpt_signal raw_signal(uint32_t timestamp, int32_t x, int32_t y) {
    return (struct zpt_signal){
        .kind = ZPT_SIGNAL_RAW_MOTION,
        .metadata = {.observed_at_ms = timestamp},
        .data.raw_motion = {.x_counts = x, .y_counts = y},
    };
}

static int push_raw(struct zpt_pipeline *pipeline, struct zpt_pipeline_result *result,
                    uint32_t timestamp, int32_t x, int32_t y) {
    struct zpt_signal signal = raw_signal(timestamp, x, y);
    return zpt_pipeline_push(pipeline, &signal, result);
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

static void test_gate_observer_sees_qualification_and_suppression(void) {
    struct zpt_suppression_policy unused_policy = {0};
    struct zpt_coherent_displacement_stage_config config = {
        .settings =
            {
                .enabled = true,
                .activation_distance = 6,
                .coherence_percent = 60,
                .qualification_timeout_ms = 160,
                .idle_timeout_ms = 120,
            },
        .suppression = &unused_policy,
    };
    struct zpt_coherent_displacement_stage_state state = {0};
    struct observer_state observer = {0};
    struct zpt_stage stage = {
        .stable_id = "motion-gate",
        .api = &zpt_coherent_displacement_stage_api,
        .config = &config,
        .state = &state,
        .observer = {.callback = record_observer, .user_data = &observer},
    };
    struct zpt_stage *stages[1] = {&stage};
    struct capture_state capture = {0};
    struct zpt_sink sink = {.stable_id = "capture", .api = &capture_api, .state = &capture};
    struct zpt_pipeline pipeline = {
        .stable_id = "gate-observer",
        .input_kind = ZPT_SIGNAL_NORMALIZED_MOTION,
        .stages = stages,
        .stage_count = 1,
        .sink = &sink,
        .dispatch_budget = 4,
    };
    assert(zpt_pipeline_validate(&pipeline) == 0);
    assert(zpt_pipeline_activate(&pipeline, ZPT_RESET_PIPELINE_ENTERED) == 0);

    struct zpt_pipeline_result result;
    /* Two coherent frames qualify the gate. */
    assert(push_normalized(&pipeline, &result, 0, 4, 4) == 0);
    assert(push_normalized(&pipeline, &result, 8, 4, 4) == 0);
    assert(has_event(&observer, ZPT_STAGE_EVENT_QUALIFIED, 0));

    /* Suppression discards and notifies. */
    config.suppression = &(struct zpt_suppression_policy){
        .is_suppressed = suppression_active,
        .context = &(struct suppression_state){.active = true},
    };
    assert(push_normalized(&pipeline, &result, 16, 4, 4) == 0);
    assert(has_event(&observer, ZPT_STAGE_EVENT_SUPPRESSED, 0));
}

static void test_scroll_observer_sees_intent_suppression_and_flush(void) {
    struct suppression_state suppression_context = {0};
    struct zpt_suppression_policy suppression = {
        .is_suppressed = suppression_active,
        .context = &suppression_context,
    };
    struct zpt_axis_intent_stage_config intent_config = {
        .settings =
            {
                .engage_ratio_percent = 300,
                .release_ratio_percent = 180,
                .activation_distance = 16,
                .window_ms = 64,
            },
        .policy = ZPT_AXIS_POLICY_ADAPTIVE,
        .idle_timeout_ms = 120,
        .suppression = &suppression,
    };
    struct zpt_axis_intent_stage_state intent_state = {0};
    struct zpt_axis_constraint_config constraint_config = {
        .idle_timeout_ms = 120,
        .suppression = &suppression,
    };
    struct zpt_axis_constraint_state constraint_state = {0};
    struct zpt_scroll_batcher_config batcher_config = {
        .scale_multiplier = 1,
        .scale_divisor = 8,
        .report_interval_ms = 16,
        .suppression = &suppression,
    };
    struct zpt_scroll_batcher_state batcher_state = {0};
    struct observer_state intent_observer = {0};
    struct observer_state batcher_observer = {0};
    struct zpt_stage intent_stage = {
        .stable_id = "axis-intent",
        .api = &zpt_axis_intent_raw_stage_api,
        .config = &intent_config,
        .state = &intent_state,
        .observer = {.callback = record_observer, .user_data = &intent_observer},
    };
    struct zpt_stage constraint_stage = {
        .stable_id = "axis-constraint",
        .api = &zpt_axis_constraint_stage_api,
        .config = &constraint_config,
        .state = &constraint_state,
    };
    struct zpt_stage batcher_stage = {
        .stable_id = "scroll-batcher",
        .api = &zpt_scroll_batcher_stage_api,
        .config = &batcher_config,
        .state = &batcher_state,
        .observer = {.callback = record_observer, .user_data = &batcher_observer},
    };
    struct zpt_stage *stages[3] = {&intent_stage, &constraint_stage, &batcher_stage};
    struct capture_state capture = {0};
    struct zpt_sink sink = {.stable_id = "capture", .api = &capture_api, .state = &capture};
    struct zpt_pipeline pipeline = {
        .stable_id = "scroll-observer",
        .input_kind = ZPT_SIGNAL_RAW_MOTION,
        .stages = stages,
        .stage_count = 3,
        .sink = &sink,
        .dispatch_budget = 8,
    };
    assert(zpt_pipeline_validate(&pipeline) == 0);
    assert(zpt_pipeline_activate(&pipeline, ZPT_RESET_PIPELINE_ENTERED) == 0);

    struct zpt_pipeline_result result;
    assert(push_raw(&pipeline, &result, 0, 10, 1) == 0);
    assert(push_raw(&pipeline, &result, 8, 10, 1) == 0);
    /* Undecided then horizontal intent changes. */
    assert(has_event(&intent_observer, ZPT_STAGE_EVENT_INTENT_CHANGED, ZPT_AXIS_INTENT_UNDECIDED));
    assert(has_event(&intent_observer, ZPT_STAGE_EVENT_INTENT_CHANGED, ZPT_AXIS_INTENT_HORIZONTAL));

    /* The report flush emits two wheel steps. */
    assert(zpt_pipeline_flush(&pipeline, 16, &result) == 0);
    assert(has_event(&batcher_observer, ZPT_STAGE_EVENT_FLUSHED, 2));

    /* Suppression clears and notifies every stage. */
    suppression_context.active = true;
    assert(push_raw(&pipeline, &result, 20, 10, 0) == 0);
    assert(has_event(&intent_observer, ZPT_STAGE_EVENT_SUPPRESSED, 0));
    assert(has_event(&batcher_observer, ZPT_STAGE_EVENT_SUPPRESSED, 0));
}

static void test_text_observer_sees_actions(void) {
    struct zpt_text_nav_config config = {
        .horizontal_threshold = 75,
        .vertical_threshold = 75,
        .idle_timeout_ms = 40,
        .activation_distance = 35,
        .engage_ratio_percent = 150,
    };
    struct zpt_text_nav_state state = {0};
    struct observer_state observer = {0};
    struct zpt_stage stage = {
        .stable_id = "text-nav",
        .api = &zpt_text_nav_stage_api,
        .config = &config,
        .state = &state,
        .observer = {.callback = record_observer, .user_data = &observer},
    };
    struct zpt_stage *stages[1] = {&stage};
    struct capture_state capture = {0};
    struct zpt_sink sink = {.stable_id = "capture", .api = &capture_api, .state = &capture};
    struct zpt_pipeline pipeline = {
        .stable_id = "text-observer",
        .input_kind = ZPT_SIGNAL_RAW_MOTION,
        .stages = stages,
        .stage_count = 1,
        .sink = &sink,
        .dispatch_budget = 4,
    };
    assert(zpt_pipeline_validate(&pipeline) == 0);
    assert(zpt_pipeline_activate(&pipeline, ZPT_RESET_PIPELINE_ENTERED) == 0);

    struct zpt_pipeline_result result;
    assert(push_raw(&pipeline, &result, 0, 20, 3) == 0);
    assert(push_raw(&pipeline, &result, 8, 25, -2) == 0);
    assert(push_raw(&pipeline, &result, 16, 30, 4) == 0);
    assert(has_event(&observer, ZPT_STAGE_EVENT_ACTION, ZPT_TEXT_NAV_RIGHT));
}

int main(void) {
    test_gate_observer_sees_qualification_and_suppression();
    test_scroll_observer_sees_intent_suppression_and_flush();
    test_text_observer_sees_actions();
    puts("stage observer tests passed");
    return 0;
}
