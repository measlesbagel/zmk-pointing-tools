/* SPDX-License-Identifier: MIT */

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zmk/pointing_tools/core/pipeline.h>
#include <zmk/pointing_tools/policy/suppression.h>
#include <zmk/pointing_tools/stage/axis_constraint.h>
#include <zmk/pointing_tools/stage/axis_intent.h>
#include <zmk/pointing_tools/stage/scroll_batcher.h>

static bool due(uint32_t now, uint32_t deadline) { return (int32_t)(now - deadline) >= 0; }

struct replay_suppression {
    bool active;
};

static bool replay_suppressed(void *context, const struct zpt_signal *signal, uint32_t now_ms) {
    (void)signal;
    (void)now_ms;
    return ((struct replay_suppression *)context)->active;
}

struct capture_state {
    uint32_t outputs;
};

static int capture_emit(struct zpt_sink *sink, const struct zpt_signal *signal) {
    (void)sink;
    if (signal->kind != ZPT_SIGNAL_SCROLL_STEPS) {
        return -EPROTOTYPE;
    }
    printf("O\t%" PRIu32 "\t%d\t%d\n", signal->metadata.observed_at_ms, signal->data.steps.x,
           signal->data.steps.y);
    return 0;
}

static const struct zpt_sink_api capture_api = {
    .type_id = "scroll-steps-capture",
    .accepted_kinds = ZPT_SIGNAL_KIND_MASK(ZPT_SIGNAL_SCROLL_STEPS),
    .emit = capture_emit,
};

struct scroll_pipeline_fixture {
    struct replay_suppression suppression;
    struct zpt_suppression_policy suppression_policy;
    uint16_t suppression_suppress_after_ms;
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

int main(void) {
    struct scroll_pipeline_fixture fixture = {0};
    unsigned int discard;
    unsigned int policy_value;
    uint16_t activation_distance;
    char line[256];

    if (fgets(line, sizeof(line), stdin) == NULL ||
        sscanf(line,
               "C %" SCNu16 " %" SCNu16 " %" SCNu16 " %" SCNu16 " %" SCNu16 " %u %" SCNu16
               " %" SCNu16 " %" SCNu16 " %" SCNu16 " %u",
               &fixture.batcher_config.scale_multiplier, &fixture.batcher_config.scale_divisor,
               &fixture.batcher_config.report_interval_ms, &fixture.intent_config.idle_timeout_ms,
               &fixture.suppression_suppress_after_ms, &discard,
               &fixture.intent_config.settings.engage_ratio_percent,
               &fixture.intent_config.settings.release_ratio_percent, &activation_distance,
               &fixture.intent_config.settings.window_ms, &policy_value) != 11 ||
        fixture.batcher_config.scale_multiplier == 0 || fixture.batcher_config.scale_divisor == 0 ||
        fixture.batcher_config.report_interval_ms == 0 || policy_value > ZPT_AXIS_POLICY_VERTICAL) {
        fputs("invalid replay configuration\n", stderr);
        return 2;
    }

    fixture.constraint_config.discard_unclassified = discard != 0;
    fixture.constraint_config.idle_timeout_ms = fixture.intent_config.idle_timeout_ms;
    fixture.intent_config.policy = (enum zpt_axis_policy)policy_value;
    fixture.intent_config.settings.activation_distance = activation_distance;

    fixture.suppression_policy = (struct zpt_suppression_policy){
        .is_suppressed = replay_suppressed,
        .context = &fixture.suppression,
    };
    fixture.intent_config.suppression = &fixture.suppression_policy;
    fixture.constraint_config.suppression = &fixture.suppression_policy;
    fixture.batcher_config.suppression = &fixture.suppression_policy;

    fixture.intent_stage = (struct zpt_stage){
        .stable_id = "axis-intent",
        .api = &zpt_axis_intent_raw_stage_api,
        .config = &fixture.intent_config,
        .state = &fixture.intent_state,
    };
    fixture.constraint_stage = (struct zpt_stage){
        .stable_id = "axis-constraint",
        .api = &zpt_axis_constraint_stage_api,
        .config = &fixture.constraint_config,
        .state = &fixture.constraint_state,
    };
    fixture.batcher_stage = (struct zpt_stage){
        .stable_id = "scroll-batcher",
        .api = &zpt_scroll_batcher_stage_api,
        .config = &fixture.batcher_config,
        .state = &fixture.batcher_state,
    };
    fixture.stages[0] = &fixture.intent_stage;
    fixture.stages[1] = &fixture.constraint_stage;
    fixture.stages[2] = &fixture.batcher_stage;
    fixture.sink = (struct zpt_sink){
        .stable_id = "capture",
        .api = &capture_api,
        .state = &fixture.capture,
    };
    fixture.pipeline = (struct zpt_pipeline){
        .stable_id = "scroll-replay",
        .input_kind = ZPT_SIGNAL_RAW_MOTION,
        .stages = fixture.stages,
        .stage_count = 3,
        .sink = &fixture.sink,
        .dispatch_budget = 8,
    };
    if (zpt_pipeline_validate(&fixture.pipeline) < 0 ||
        zpt_pipeline_activate(&fixture.pipeline, ZPT_RESET_PIPELINE_ENTERED) < 0) {
        fputs("pipeline failed to activate\n", stderr);
        return 2;
    }

    bool have_last_frame = false;
    uint32_t last_frame = 0;
    bool keypress_seen = false;
    uint32_t last_keypress = 0;

    while (fgets(line, sizeof(line), stdin) != NULL) {
        char type;
        uint32_t timestamp;
        int32_t x;
        int32_t y;
        int fields =
            sscanf(line, " %c %" SCNu32 " %" SCNd32 " %" SCNd32, &type, &timestamp, &x, &y);
        if (fields < 2) {
            fputs("invalid replay event\n", stderr);
            return 2;
        }

        uint32_t deadline;
        while (zpt_pipeline_next_deadline(&fixture.pipeline, timestamp, &deadline) &&
               due(timestamp, deadline)) {
            struct zpt_pipeline_result result;
            if (zpt_pipeline_flush(&fixture.pipeline, timestamp, &result) < 0) {
                fputs("pipeline flush failed\n", stderr);
                return 2;
            }
        }

        if (type == 'K') {
            keypress_seen = true;
            last_keypress = timestamp;
            continue;
        }
        if (type != 'M' || fields != 4) {
            fputs("unknown replay event\n", stderr);
            return 2;
        }

        bool suppressed = fixture.suppression_suppress_after_ms > 0 && keypress_seen &&
                          timestamp - last_keypress < fixture.suppression_suppress_after_ms;
        bool idle = !suppressed && (!have_last_frame || timestamp - last_frame >=
                                                            fixture.intent_config.idle_timeout_ms);
        fixture.suppression.active = suppressed;

        struct zpt_signal signal = {
            .kind = ZPT_SIGNAL_RAW_MOTION,
            .metadata = {.observed_at_ms = timestamp},
            .data.raw_motion = {.x_counts = x, .y_counts = y},
        };
        struct zpt_pipeline_result result;
        if (zpt_pipeline_push(&fixture.pipeline, &signal, &result) < 0) {
            fputs("pipeline push failed\n", stderr);
            return 2;
        }

        printf("D\t%" PRIu32 "\t%d\t%" PRIu64 "\t%" PRIu64 "\t%" PRId64 "\t%" PRId64 "\t%" PRId64
               "\t%" PRId64 "\t%" PRId64 "\t%" PRId64 "\t%d\t%d\n",
               timestamp, fixture.intent_state.estimator.intent,
               fixture.intent_state.estimator.horizontal_energy,
               fixture.intent_state.estimator.vertical_energy, fixture.constraint_state.undecided_x,
               fixture.constraint_state.undecided_y, fixture.batcher_state.pending_x,
               fixture.batcher_state.pending_y, fixture.batcher_state.remainder_x,
               fixture.batcher_state.remainder_y, idle, suppressed);

        if (!suppressed) {
            have_last_frame = true;
            last_frame = timestamp;
        }
    }

    for (;;) {
        uint32_t next;
        if (!zpt_pipeline_next_deadline(&fixture.pipeline, 0, &next)) {
            break;
        }
        struct zpt_pipeline_result result;
        if (zpt_pipeline_flush(&fixture.pipeline, next, &result) < 0) {
            fputs("final pipeline flush failed\n", stderr);
            return 2;
        }
    }
    return 0;
}
