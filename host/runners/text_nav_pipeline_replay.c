/* SPDX-License-Identifier: MIT */

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include <zmk/pointing_tools/core/pipeline.h>
#include <zmk/pointing_tools/stage/text_nav.h>

struct capture_state {
    uint32_t outputs;
};

static int capture_emit(struct zpt_sink *sink, const struct zpt_signal *signal) {
    (void)signal;
    struct capture_state *capture = sink->state;
    capture->outputs++;
    return 0;
}

static const struct zpt_sink_api capture_api = {
    .type_id = "action-capture",
    .accepted_kinds = ZPT_SIGNAL_KIND_MASK(ZPT_SIGNAL_ACTION),
    .emit = capture_emit,
};

struct text_pipeline_fixture {
    struct zpt_text_nav_config config;
    struct zpt_text_nav_state state;
    struct zpt_stage stage;
    struct zpt_stage *stages[1];
    struct capture_state capture;
    struct zpt_sink sink;
    struct zpt_pipeline pipeline;
};

int main(void) {
    struct text_pipeline_fixture fixture = {0};
    char line[256];

    if (fgets(line, sizeof(line), stdin) == NULL ||
        sscanf(line, "C %" SCNd64 " %" SCNd64 " %" SCNu16 " %" SCNd64 " %" SCNu16,
               &fixture.config.horizontal_threshold, &fixture.config.vertical_threshold,
               &fixture.config.idle_timeout_ms, &fixture.config.activation_distance,
               &fixture.config.engage_ratio_percent) != 5 ||
        fixture.config.horizontal_threshold == 0 || fixture.config.vertical_threshold == 0 ||
        fixture.config.idle_timeout_ms == 0 || fixture.config.activation_distance == 0) {
        fputs("invalid replay configuration\n", stderr);
        return 2;
    }

    fixture.stage = (struct zpt_stage){
        .stable_id = "text-nav",
        .api = &zpt_text_nav_stage_api,
        .config = &fixture.config,
        .state = &fixture.state,
    };
    fixture.stages[0] = &fixture.stage;
    fixture.sink = (struct zpt_sink){
        .stable_id = "capture",
        .api = &capture_api,
        .state = &fixture.capture,
    };
    fixture.pipeline = (struct zpt_pipeline){
        .stable_id = "text-replay",
        .input_kind = ZPT_SIGNAL_RAW_MOTION,
        .stages = fixture.stages,
        .stage_count = 1,
        .sink = &fixture.sink,
        .dispatch_budget = 4,
    };
    if (zpt_pipeline_validate(&fixture.pipeline) < 0 ||
        zpt_pipeline_activate(&fixture.pipeline, ZPT_RESET_PIPELINE_ENTERED) < 0) {
        fputs("pipeline failed to activate\n", stderr);
        return 2;
    }

    while (fgets(line, sizeof(line), stdin) != NULL) {
        char type;
        uint32_t timestamp;
        int32_t x;
        int32_t y;
        if (sscanf(line, " %c %" SCNu32 " %" SCNd32 " %" SCNd32, &type, &timestamp, &x, &y) != 4 ||
            type != 'M') {
            fputs("text navigation only accepts motion events\n", stderr);
            return 2;
        }

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

        printf("D\t%" PRIu32 "\t%d\t%" PRId64 "\t%" PRId64 "\t%d\n", timestamp,
               fixture.state.intent, fixture.state.accumulated_x, fixture.state.accumulated_y,
               fixture.state.last_direction);
        if (fixture.state.last_direction != ZPT_TEXT_NAV_NONE) {
            printf("O\t%" PRIu32 "\t%d\n", timestamp, fixture.state.last_direction);
        }
    }
    return 0;
}
