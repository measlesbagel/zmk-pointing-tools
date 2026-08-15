/* SPDX-License-Identifier: MIT */

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include <zmk/pointing_tools/core/pipeline.h>
#include <zmk/pointing_tools/stage/resolution_normalize.h>
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
    uint16_t resolution_cpi;
    struct zpt_text_nav_config config;
    struct zpt_text_nav_state state;
    struct zpt_stage normalize_stage;
    struct zpt_stage stage;
    struct zpt_stage *stages[2];
    struct capture_state capture;
    struct zpt_sink sink;
    struct zpt_pipeline pipeline;
};

int main(void) {
    struct text_pipeline_fixture fixture = {0};
    char line[256];

    int32_t horizontal_micrometers;
    int32_t vertical_micrometers;
    int32_t activation_micrometers;
    if (fgets(line, sizeof(line), stdin) == NULL ||
        sscanf(line, "C %" SCNu16 " %" SCNd32 " %" SCNd32 " %" SCNu16 " %" SCNd32 " %" SCNu16,
               &fixture.resolution_cpi, &horizontal_micrometers, &vertical_micrometers,
               &fixture.config.idle_timeout_ms, &activation_micrometers,
               &fixture.config.engage_ratio_percent) != 6 ||
        fixture.resolution_cpi == 0 || horizontal_micrometers <= 0 || vertical_micrometers <= 0 ||
        fixture.config.idle_timeout_ms == 0 || activation_micrometers <= 0) {
        fputs("invalid replay configuration\n", stderr);
        return 2;
    }
    fixture.config.horizontal_threshold =
        ZPT_MICROMETERS_TO_FIXED_MILLIMETERS(horizontal_micrometers);
    fixture.config.vertical_threshold = ZPT_MICROMETERS_TO_FIXED_MILLIMETERS(vertical_micrometers);
    fixture.config.activation_distance =
        ZPT_MICROMETERS_TO_FIXED_MILLIMETERS(activation_micrometers);

    fixture.normalize_stage = (struct zpt_stage){
        .stable_id = "resolution-normalize",
        .api = &zpt_resolution_normalize_stage_api,
    };
    fixture.stage = (struct zpt_stage){
        .stable_id = "text-nav",
        .api = &zpt_text_nav_stage_api,
        .config = &fixture.config,
        .state = &fixture.state,
    };
    fixture.stages[0] = &fixture.normalize_stage;
    fixture.stages[1] = &fixture.stage;
    fixture.sink = (struct zpt_sink){
        .stable_id = "capture",
        .api = &capture_api,
        .state = &fixture.capture,
    };
    fixture.pipeline = (struct zpt_pipeline){
        .stable_id = "text-replay",
        .input_kind = ZPT_SIGNAL_RAW_MOTION,
        .stages = fixture.stages,
        .stage_count = 2,
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
            .metadata = {.observed_at_ms = timestamp, .resolution_cpi = fixture.resolution_cpi},
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
