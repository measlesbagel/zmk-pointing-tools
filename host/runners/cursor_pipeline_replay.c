/* SPDX-License-Identifier: MIT */

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include <zmk/pointing_tools/core/pipeline.h>
#include <zmk/pointing_tools/stage/cursor_quantizer.h>
#include <zmk/pointing_tools/stage/cursor_transfer.h>
#include <zmk/pointing_tools/stage/resolution_normalize.h>

static int capture_emit(struct zpt_sink *sink, const struct zpt_signal *signal) {
    (void)sink;
    if (signal->kind != ZPT_SIGNAL_POINTER_DELTA) {
        return -EPROTOTYPE;
    }
    printf("O\t%" PRIu32 "\t%d\t%d\n", signal->metadata.observed_at_ms, signal->data.delta.x,
           signal->data.delta.y);
    return 0;
}

static const struct zpt_sink_api capture_api = {
    .type_id = "pointer-capture",
    .accepted_kinds = ZPT_SIGNAL_KIND_MASK(ZPT_SIGNAL_POINTER_DELTA),
    .emit = capture_emit,
};

struct cursor_pipeline_fixture {
    struct zpt_cursor_transfer_config transfer_config;
    struct zpt_cursor_quantizer_config quantizer_config;
    struct zpt_cursor_quantizer_state quantizer_state;
    struct zpt_stage normalize_stage;
    struct zpt_stage transfer_stage;
    struct zpt_stage quantizer_stage;
    struct zpt_stage *stages[3];
    struct zpt_sink sink;
    struct zpt_pipeline pipeline;
};

int main(void) {
    struct cursor_pipeline_fixture fixture = {0};
    uint16_t resolution_cpi;
    uint16_t scale_multiplier;
    uint16_t scale_divisor;
    int32_t units_per_meter;
    char line[256];

    if (fgets(line, sizeof(line), stdin) == NULL ||
        sscanf(line, "C %" SCNu16 " %" SCNu16 " %" SCNu16 " %" SCNd32, &resolution_cpi,
               &scale_multiplier, &scale_divisor, &units_per_meter) != 4 ||
        resolution_cpi == 0 || scale_divisor == 0 || units_per_meter <= 0) {
        fputs("invalid replay configuration\n", stderr);
        return 2;
    }

    fixture.transfer_config = (struct zpt_cursor_transfer_config){
        .scale_multiplier = scale_multiplier,
        .scale_divisor = scale_divisor,
    };
    fixture.quantizer_config = (struct zpt_cursor_quantizer_config){
        .units_per_millimeter = ZPT_PER_METER_TO_FIXED_PER_MILLIMETER(units_per_meter),
    };
    fixture.normalize_stage = (struct zpt_stage){
        .stable_id = "resolution-normalize",
        .api = &zpt_resolution_normalize_stage_api,
    };
    fixture.transfer_stage = (struct zpt_stage){
        .stable_id = "cursor-transfer",
        .api = &zpt_cursor_transfer_stage_api,
        .config = &fixture.transfer_config,
    };
    fixture.quantizer_stage = (struct zpt_stage){
        .stable_id = "cursor-quantizer",
        .api = &zpt_cursor_quantizer_stage_api,
        .config = &fixture.quantizer_config,
        .state = &fixture.quantizer_state,
    };
    fixture.stages[0] = &fixture.normalize_stage;
    fixture.stages[1] = &fixture.transfer_stage;
    fixture.stages[2] = &fixture.quantizer_stage;
    fixture.sink = (struct zpt_sink){
        .stable_id = "capture",
        .api = &capture_api,
    };
    fixture.pipeline = (struct zpt_pipeline){
        .stable_id = "cursor-replay",
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

    while (fgets(line, sizeof(line), stdin) != NULL) {
        char type;
        uint32_t timestamp;
        int32_t x;
        int32_t y;
        if (sscanf(line, " %c %" SCNu32 " %" SCNd32 " %" SCNd32, &type, &timestamp, &x, &y) != 4 ||
            type != 'M') {
            fputs("cursor replay only accepts motion events\n", stderr);
            return 2;
        }

        struct zpt_signal signal = {
            .kind = ZPT_SIGNAL_RAW_MOTION,
            .metadata = {.observed_at_ms = timestamp, .resolution_cpi = resolution_cpi},
            .data.raw_motion = {.x_counts = x, .y_counts = y},
        };
        struct zpt_pipeline_result result;
        if (zpt_pipeline_push(&fixture.pipeline, &signal, &result) < 0) {
            fputs("pipeline push failed\n", stderr);
            return 2;
        }

        printf("D\t%" PRIu32 "\t%" PRId64 "\t%" PRId64 "\t%" PRId64 "\t%" PRId64 "\t%d\n",
               timestamp, signal.data.raw_motion.x_counts, signal.data.raw_motion.y_counts,
               fixture.quantizer_state.remainder_x, fixture.quantizer_state.remainder_y,
               (signal.metadata.flags & ZPT_SIGNAL_FLAG_CLIPPED) != 0);
    }
    return 0;
}
