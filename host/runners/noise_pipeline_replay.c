/* SPDX-License-Identifier: MIT */

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include <zmk/pointing_tools/core/pipeline.h>
#include <zmk/pointing_tools/policy/suppression.h>
#include <zmk/pointing_tools/stage/motion_gate/coherent_displacement.h>
#include <zmk/pointing_tools/stage/resolution_normalize.h>

struct replay_suppression {
    bool active;
};

static bool replay_suppressed(void *context, const struct zpt_signal *signal, uint32_t now_ms) {
    (void)signal;
    (void)now_ms;
    return ((struct replay_suppression *)context)->active;
}

struct capture_state {
    int64_t output_x;
    int64_t output_y;
    bool have_output;
};

static int capture_emit(struct zpt_sink *sink, const struct zpt_signal *signal) {
    struct capture_state *capture = sink->state;
    if (signal->kind == ZPT_SIGNAL_NORMALIZED_MOTION) {
        capture->output_x = signal->data.fixed_vector.x;
        capture->output_y = signal->data.fixed_vector.y;
        capture->have_output = true;
    }
    return 0;
}

static const struct zpt_sink_api capture_api = {
    .type_id = "normalized-capture",
    .accepted_kinds = ZPT_SIGNAL_KIND_MASK(ZPT_SIGNAL_NORMALIZED_MOTION),
    .emit = capture_emit,
};

struct event_flags {
    bool qualified;
    bool suppressed;
    bool discarded;
};

static void record_observer(const struct zpt_stage *stage, enum zpt_stage_event event,
                            int64_t value, uint32_t now_ms, void *user_data) {
    (void)stage;
    (void)value;
    (void)now_ms;
    struct event_flags *flags = user_data;
    switch (event) {
    case ZPT_STAGE_EVENT_QUALIFIED:
        flags->qualified = true;
        break;
    case ZPT_STAGE_EVENT_SUPPRESSED:
        flags->suppressed = true;
        break;
    case ZPT_STAGE_EVENT_DISCARDED:
        flags->discarded = true;
        break;
    default:
        break;
    }
}

struct noise_pipeline_fixture {
    struct replay_suppression suppression;
    uint16_t suppression_after_ms;
    uint16_t resolution_cpi;
    struct zpt_suppression_policy suppression_policy;
    struct zpt_coherent_displacement_stage_config config;
    struct zpt_coherent_displacement_stage_state state;
    struct zpt_stage normalize_stage;
    struct zpt_stage stage;
    struct zpt_stage *stages[2];
    struct capture_state capture;
    struct zpt_sink sink;
    struct zpt_pipeline pipeline;
    struct event_flags events;
};

struct replay_context {
    bool have_last_frame;
    uint32_t last_frame;
    bool keypress_seen;
    uint32_t last_keypress;
};

static int read_replay_config(char *line, int line_size, struct noise_pipeline_fixture *fixture,
                              int32_t *activation_micrometers) {
    unsigned int enabled_value;
    if (fgets(line, line_size, stdin) == NULL ||
        sscanf(line, "C %u %" SCNu16 " %" SCNd32 " %" SCNu16 " %" SCNu16 " %" SCNu16 " %" SCNu16,
               &enabled_value, &fixture->resolution_cpi, activation_micrometers,
               &fixture->config.settings.coherence_percent,
               &fixture->config.settings.qualification_timeout_ms,
               &fixture->config.settings.idle_timeout_ms, &fixture->suppression_after_ms) != 7 ||
        fixture->resolution_cpi == 0 || *activation_micrometers <= 0 ||
        fixture->config.settings.qualification_timeout_ms == 0 ||
        fixture->config.settings.idle_timeout_ms == 0) {
        fputs("invalid replay configuration\n", stderr);
        return -1;
    }
    fixture->config.settings.enabled = enabled_value != 0;
    fixture->config.settings.activation_distance =
        ZPT_MICROMETERS_TO_FIXED_MILLIMETERS(*activation_micrometers);
    return 0;
}

static int init_pipeline(struct noise_pipeline_fixture *fixture) {
    fixture->suppression_policy = (struct zpt_suppression_policy){
        .is_suppressed = replay_suppressed,
        .context = &fixture->suppression,
    };
    fixture->config.suppression = &fixture->suppression_policy;

    fixture->normalize_stage = (struct zpt_stage){
        .stable_id = "resolution-normalize",
        .api = &zpt_resolution_normalize_stage_api,
    };
    fixture->stage = (struct zpt_stage){
        .stable_id = "motion-gate",
        .api = &zpt_coherent_displacement_stage_api,
        .config = &fixture->config,
        .state = &fixture->state,
        .observer = {.callback = record_observer, .user_data = &fixture->events},
    };
    fixture->stages[0] = &fixture->normalize_stage;
    fixture->stages[1] = &fixture->stage;
    fixture->sink = (struct zpt_sink){
        .stable_id = "capture",
        .api = &capture_api,
        .state = &fixture->capture,
    };
    fixture->pipeline = (struct zpt_pipeline){
        .stable_id = "noise-replay",
        .input_kind = ZPT_SIGNAL_RAW_MOTION,
        .stages = fixture->stages,
        .stage_count = 2,
        .sink = &fixture->sink,
        .dispatch_budget = 4,
    };
    if (zpt_pipeline_validate(&fixture->pipeline) < 0 ||
        zpt_pipeline_activate(&fixture->pipeline, ZPT_RESET_PIPELINE_ENTERED) < 0) {
        fputs("pipeline failed to activate\n", stderr);
        return -1;
    }
    return 0;
}

static int motion_gate_phase(const struct noise_pipeline_fixture *fixture) {
    if (!fixture->config.settings.enabled) {
        return ZPT_MOTION_GATE_BYPASS;
    }
    if (fixture->state.strategy.active) {
        return ZPT_MOTION_GATE_ACTIVE;
    }
    if (fixture->state.strategy.sample_count != 0U) {
        return ZPT_MOTION_GATE_PENDING;
    }
    return ZPT_MOTION_GATE_IDLE;
}

static int process_motion_event(struct noise_pipeline_fixture *fixture,
                                struct replay_context *context, uint32_t timestamp, int32_t x,
                                int32_t y) {
    bool suppressed = fixture->suppression_after_ms > 0 && context->keypress_seen &&
                      timestamp - context->last_keypress < fixture->suppression_after_ms;
    bool idle = context->have_last_frame &&
                timestamp - context->last_frame >= fixture->config.settings.idle_timeout_ms;
    bool had_pending = fixture->state.strategy.sample_count != 0U || fixture->state.strategy.active;
    fixture->suppression.active = suppressed;
    fixture->events = (struct event_flags){0};
    fixture->capture.have_output = false;

    struct zpt_signal signal = {
        .kind = ZPT_SIGNAL_RAW_MOTION,
        .metadata = {.observed_at_ms = timestamp, .resolution_cpi = fixture->resolution_cpi},
        .data.raw_motion = {.x_counts = x, .y_counts = y},
    };
    struct zpt_pipeline_result result;
    int ret = zpt_pipeline_push(&fixture->pipeline, &signal, &result);
    if (ret < 0) {
        fprintf(stderr, "pipeline push failed: %d\n", ret);
        return ret;
    }

    bool discarded = fixture->events.discarded || (fixture->events.suppressed && had_pending);
    int phase = motion_gate_phase(fixture);
    printf("D\t%" PRIu32 "\t%d\t%" PRId64 "\t%" PRId64 "\t%" PRIu32 "\t%" PRIu64
           "\t%d\t%d\t%d\t%d\t%d\t%" PRId64 "\t%" PRId64 "\n",
           timestamp, phase, fixture->state.strategy.pending_x, fixture->state.strategy.pending_y,
           fixture->state.strategy.sample_count, fixture->state.strategy.squared_energy, idle,
           fixture->events.discarded && !idle, fixture->events.suppressed,
           fixture->events.qualified, discarded,
           fixture->capture.have_output ? fixture->capture.output_x : 0,
           fixture->capture.have_output ? fixture->capture.output_y : 0);
    if (fixture->capture.have_output &&
        (fixture->capture.output_x != 0 || fixture->capture.output_y != 0)) {
        printf("O\t%" PRIu32 "\t%" PRId64 "\t%" PRId64 "\n", timestamp, fixture->capture.output_x,
               fixture->capture.output_y);
    }

    if (!suppressed) {
        context->have_last_frame = true;
        context->last_frame = timestamp;
    }
    return 0;
}

int main(void) {
    struct noise_pipeline_fixture fixture = {0};
    struct replay_context context = {0};
    char line[256];
    int32_t activation_micrometers;

    if (read_replay_config(line, sizeof(line), &fixture, &activation_micrometers) < 0 ||
        init_pipeline(&fixture) < 0) {
        return 2;
    }

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
        if (type == 'K') {
            context.keypress_seen = true;
            context.last_keypress = timestamp;
            continue;
        }
        if (type != 'M' || fields != 4) {
            fputs("unknown replay event\n", stderr);
            return 2;
        }
        if (process_motion_event(&fixture, &context, timestamp, x, y) < 0) {
            return 2;
        }
    }
    return 0;
}
