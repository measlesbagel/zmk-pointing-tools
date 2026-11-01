/* SPDX-License-Identifier: MIT */

/*
 * Replays motion through a cursor pipeline assembled from the input.
 *
 * Input is line based. A header names the source and the stages in order,
 * then events follow:
 *
 *   R <cpi>                 sensor resolution of every frame
 *   L usb|ble               link the coalescer reads
 *   S <type> <stable-id>    append a stage: orientation, normalize, gate,
 *                           acceleration, transfer, quantizer, coalescer
 *   P <key> <value>         set a parameter of the last stage, by its
 *                           tuning key (the tuner profile's key)
 *   F                       deadlines run only at F events (a recording)
 *   E                       end of header
 *   M <t_us> <x> <y> <span_us> [late_ms]  motion arriving at t_us; span 0
 *                           = none; its stages see the millisecond late_ms
 *                           before t_us (a recorded frame that waited)
 *   K <t_us> [late_ms]      physical keypress
 *   F <t_us> [late_ms]      run the deadlines due then, as recorded
 *
 * Tunable stages take every tuning parameter exactly once and run the
 * firmware's own tuning conversions, so profile values replay exactly as a
 * device applies them. Frames carry the millisecond arrival time firmware
 * timestamps with, plus the sensor span when the source has one. Deadlines
 * flush at their own time, as the firmware's work queue does, unless the
 * header asks for recorded flushes: then they run only where a recording
 * saw them run, and nothing runs after the last event.
 *
 * Output:
 *   D <t_ms> <x> <y> <remainder_x> <remainder_y> <clipped> <t_us> <speed> <gain>
 *     per input frame; speed is the acceleration stage's Q16 mm/s
 *     estimate and gain the Q16 sensitivity it applied, both -1 when the
 *     frame did not reach that stage
 *   O <t_ms> <x> <y> <t_us>  per cursor report
 */

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zmk/pointing_tools/core/pipeline.h>
#include <zmk/pointing_tools/policy/suppression.h>
#include <zmk/pointing_tools/stage/cursor_acceleration.h>
#include <zmk/pointing_tools/stage/cursor_coalescer.h>
#include <zmk/pointing_tools/stage/cursor_quantizer.h>
#include <zmk/pointing_tools/stage/cursor_transfer.h>
#include <zmk/pointing_tools/stage/motion_gate/coherent_displacement.h>
#include <zmk/pointing_tools/stage/orientation.h>
#include <zmk/pointing_tools/stage/resolution_normalize.h>
#include <zmk/pointing_tools/stage/stage_tuning.h>

#define MAX_STAGES 8
#define MAX_ID 64
#define PARAMETER_SET_WORDS 2

enum stage_type {
    STAGE_ORIENTATION = 0,
    STAGE_NORMALIZE,
    STAGE_GATE,
    STAGE_ACCELERATION,
    STAGE_TRANSFER,
    STAGE_QUANTIZER,
    STAGE_COALESCER,
    STAGE_TYPE_COUNT,
};

static const char *const stage_names[STAGE_TYPE_COUNT] = {
    "orientation", "normalize", "gate", "acceleration", "transfer", "quantizer", "coalescer",
};

union stage_config {
    struct zpt_orientation_config orientation;
    struct zpt_coherent_displacement_stage_config gate;
    struct zpt_cursor_acceleration_config acceleration;
    struct zpt_cursor_transfer_config transfer;
    struct zpt_cursor_coalescer_config coalescer;
};

union stage_state {
    struct zpt_coherent_displacement_stage_state gate;
    struct zpt_cursor_acceleration_state acceleration;
    struct zpt_cursor_quantizer_state quantizer;
    struct zpt_cursor_coalescer_state coalescer;
};

struct replay_stage {
    enum stage_type type;
    char stable_id[MAX_ID];
    union stage_config config;
    union stage_state state;
    uint32_t parameters_set[PARAMETER_SET_WORDS];
};

struct replay {
    uint16_t resolution_cpi;
    enum zpt_cursor_link link;
    uint16_t suppress_after_keypress_ms;
    bool keypress_seen;
    bool recorded_flushes;
    uint32_t last_keypress_ms;
    struct zpt_suppression_policy suppression;
    struct replay_stage replay_stages[MAX_STAGES];
    struct zpt_stage stages[MAX_STAGES];
    struct zpt_stage *stage_refs[MAX_STAGES];
    size_t stage_count;
    struct zpt_sink sink;
    struct zpt_pipeline pipeline;
    /* Time of the operation in progress, for report timestamps. */
    uint32_t now_us;
    const struct zpt_cursor_acceleration_config *acceleration;
    zpt_fixed_t frame_speed;
    bool have_frame_speed;
};

/* The coalescer reads the link through a plain function pointer. */
static enum zpt_cursor_link replay_link = ZPT_CURSOR_LINK_USB;

static enum zpt_cursor_link current_link(void) { return replay_link; }

static bool due(uint32_t now, uint32_t deadline) { return (int32_t)(now - deadline) >= 0; }

static int fail(const char *message) {
    fprintf(stderr, "%s\n", message);
    return -EINVAL;
}

static const struct zpt_stage_tuning_ops *tuning_ops(enum stage_type type) {
    switch (type) {
    case STAGE_GATE:
        return &zpt_coherent_displacement_tuning_ops;
    case STAGE_ACCELERATION:
        return &zpt_cursor_acceleration_tuning_ops;
    case STAGE_TRANSFER:
        return &zpt_cursor_transfer_tuning_ops;
    case STAGE_COALESCER:
        return &zpt_cursor_coalescer_tuning_ops;
    default:
        return NULL;
    }
}

static const struct zpt_stage_api *stage_api(enum stage_type type) {
    static const struct zpt_stage_api *const apis[STAGE_TYPE_COUNT] = {
        &zpt_orthogonal_orientation_stage_api, &zpt_resolution_normalize_stage_api,
        &zpt_coherent_displacement_stage_api,  &zpt_cursor_acceleration_stage_api,
        &zpt_cursor_transfer_stage_api,        &zpt_cursor_quantizer_stage_api,
        &zpt_cursor_coalescer_stage_api,
    };
    return apis[type];
}

static bool replay_suppressed(void *context, const struct zpt_signal *signal, uint32_t now_ms) {
    (void)signal;
    const struct replay *replay = context;
    return replay->suppress_after_keypress_ms > 0U && replay->keypress_seen &&
           now_ms - replay->last_keypress_ms < replay->suppress_after_keypress_ms;
}

static int capture_emit(struct zpt_sink *sink, const struct zpt_signal *signal) {
    const struct replay *replay = sink->state;
    if (signal->kind != ZPT_SIGNAL_POINTER_DELTA) {
        return -EPROTOTYPE;
    }
    printf("O\t%" PRIu32 "\t%d\t%d\t%" PRIu32 "\n", signal->metadata.observed_at_ms,
           signal->data.delta.x, signal->data.delta.y, replay->now_us);
    return 0;
}

static const struct zpt_sink_api capture_api = {
    .type_id = "pointer-capture",
    .accepted_kinds = ZPT_SIGNAL_KIND_MASK(ZPT_SIGNAL_POINTER_DELTA),
    .emit = capture_emit,
};

static void record_speed(const struct zpt_stage *stage, const struct zpt_signal *signal,
                         uint32_t now_ms, void *user_data) {
    (void)stage;
    (void)now_ms;
    struct replay *replay = user_data;
    replay->frame_speed = signal->annotations.speed_per_second;
    replay->have_frame_speed = true;
}

static int add_stage(struct replay *replay, const char *type_name, const char *stable_id) {
    const size_t id_length = strlen(stable_id);
    if (replay->stage_count >= MAX_STAGES || id_length >= MAX_ID) {
        return fail("too many stages or stable id too long");
    }
    for (int type = 0; type < STAGE_TYPE_COUNT; type++) {
        if (strcmp(type_name, stage_names[type]) == 0) {
            struct replay_stage *stage = &replay->replay_stages[replay->stage_count++];
            stage->type = (enum stage_type)type;
            memcpy(stage->stable_id, stable_id, id_length + 1U);
            if (type == STAGE_GATE) {
                stage->config.gate.suppression = &replay->suppression;
            } else if (type == STAGE_COALESCER) {
                stage->config.coalescer.current_link = current_link;
            }
            return 0;
        }
    }
    fprintf(stderr, "unknown stage type %s\n", type_name);
    return -EINVAL;
}

static int set_orientation(struct zpt_orientation_config *config, const char *key, int32_t value) {
    bool *field = strcmp(key, "swap-xy") == 0    ? &config->swap_xy
                  : strcmp(key, "invert-x") == 0 ? &config->invert_x
                  : strcmp(key, "invert-y") == 0 ? &config->invert_y
                                                 : NULL;
    if (field == NULL || value < 0 || value > 1) {
        return fail("orientation takes swap-xy, invert-x, and invert-y as 0 or 1");
    }
    *field = value != 0;
    return 0;
}

/* Apply one tuning value through the firmware's own conversion. */
static int set_tuned(struct replay_stage *stage, const struct zpt_stage_tuning_ops *ops,
                     const char *key, int32_t value) {
    for (size_t i = 0; i < ops->parameter_count; i++) {
        const struct zpt_tuning_parameter *parameter = &ops->parameters[i];
        if (strcmp(parameter->key, key) != 0) {
            continue;
        }
        const uint32_t bit = UINT32_C(1) << (parameter->id % 32U);
        uint32_t *word = &stage->parameters_set[parameter->id / 32U];
        if ((*word & bit) != 0U || value < parameter->minimum || value > parameter->maximum ||
            (value - parameter->minimum) % parameter->step != 0) {
            fprintf(stderr, "%s.%s: duplicate or out-of-range value %" PRId32 "\n",
                    stage->stable_id, key, value);
            return -EINVAL;
        }
        *word |= bit;
        return ops->put(&stage->config, parameter->id, value);
    }
    fprintf(stderr, "%s: unknown parameter %s\n", stage->stable_id, key);
    return -EINVAL;
}

static int set_parameter(struct replay *replay, const char *key, int32_t value) {
    if (replay->stage_count == 0U) {
        return fail("parameter before any stage");
    }
    struct replay_stage *stage = &replay->replay_stages[replay->stage_count - 1U];
    if (stage->type == STAGE_ORIENTATION) {
        return set_orientation(&stage->config.orientation, key, value);
    }
    /* The keypress guard is a devicetree reference, not a tuning value. */
    if (stage->type == STAGE_GATE && strcmp(key, "suppress-after-keypress-ms") == 0) {
        if (value < 0 || value > UINT16_MAX) {
            return fail("suppress-after-keypress-ms is out of range");
        }
        replay->suppress_after_keypress_ms = (uint16_t)value;
        return 0;
    }
    const struct zpt_stage_tuning_ops *ops = tuning_ops(stage->type);
    if (ops == NULL) {
        fprintf(stderr, "%s takes no parameters\n", stage->stable_id);
        return -EINVAL;
    }
    return set_tuned(stage, ops, key, value);
}

static int check_complete(const struct replay_stage *stage) {
    const struct zpt_stage_tuning_ops *ops = tuning_ops(stage->type);
    if (ops == NULL) {
        return 0;
    }
    for (size_t i = 0; i < ops->parameter_count; i++) {
        const uint8_t id = ops->parameters[i].id;
        if ((stage->parameters_set[id / 32U] & (UINT32_C(1) << (id % 32U))) == 0U) {
            fprintf(stderr, "%s: missing parameter %s\n", stage->stable_id, ops->parameters[i].key);
            return -EINVAL;
        }
    }
    uint8_t failed = 0;
    if (ops->validate != NULL && ops->validate(&stage->config, &failed) < 0) {
        fprintf(stderr, "%s: invalid parameter %s\n", stage->stable_id,
                ops->parameters[failed].key);
        return -EINVAL;
    }
    return 0;
}

static int read_header_line(struct replay *replay, const char *line, bool *done) {
    char word[MAX_ID];
    char name[MAX_ID];
    int32_t value;
    uint16_t cpi;
    switch (line[0]) {
    case 'R':
        if (sscanf(line, "R %" SCNu16, &cpi) != 1 || cpi == 0U) {
            return fail("invalid resolution");
        }
        replay->resolution_cpi = cpi;
        return 0;
    case 'L':
        if (sscanf(line, "L %63s", word) != 1 ||
            (strcmp(word, "usb") != 0 && strcmp(word, "ble") != 0)) {
            return fail("link must be usb or ble");
        }
        replay->link = strcmp(word, "ble") == 0 ? ZPT_CURSOR_LINK_BLE : ZPT_CURSOR_LINK_USB;
        return 0;
    case 'S':
        if (sscanf(line, "S %63s %63s", word, name) != 2) {
            return fail("stage needs a type and a stable id");
        }
        return add_stage(replay, word, name);
    case 'P':
        if (sscanf(line, "P %63s %" SCNd32, word, &value) != 2) {
            return fail("parameter needs a key and an integer value");
        }
        return set_parameter(replay, word, value);
    case 'F':
        replay->recorded_flushes = true;
        return 0;
    case 'E':
        *done = true;
        return 0;
    default:
        return fail("unknown header line");
    }
}

static int build_pipeline(struct replay *replay) {
    replay->suppression = (struct zpt_suppression_policy){
        .is_suppressed = replay_suppressed,
        .context = replay,
    };
    for (size_t i = 0; i < replay->stage_count; i++) {
        struct replay_stage *source = &replay->replay_stages[i];
        if (check_complete(source) < 0) {
            return -EINVAL;
        }
        replay->stages[i] = (struct zpt_stage){
            .stable_id = source->stable_id,
            .api = stage_api(source->type),
            .config = &source->config,
            .state = &source->state,
        };
        if (source->type == STAGE_ACCELERATION) {
            replay->acceleration = &source->config.acceleration;
            replay->stages[i].observer.frame = record_speed;
            replay->stages[i].observer.user_data = replay;
        }
        replay->stage_refs[i] = &replay->stages[i];
    }
    replay->sink = (struct zpt_sink){
        .stable_id = "capture",
        .api = &capture_api,
        .state = replay,
    };
    replay->pipeline = (struct zpt_pipeline){
        .stable_id = "cursor-replay",
        .input_kind = ZPT_SIGNAL_RAW_MOTION,
        .stages = replay->stage_refs,
        .stage_count = replay->stage_count,
        .sink = &replay->sink,
        .dispatch_budget = 16,
    };
    replay_link = replay->link;
    if (zpt_pipeline_validate(&replay->pipeline) < 0 ||
        zpt_pipeline_activate(&replay->pipeline, ZPT_RESET_PIPELINE_ENTERED) < 0) {
        return fail("pipeline failed to activate");
    }
    return 0;
}

static const struct replay_stage *find_stage(const struct replay *replay, enum stage_type type) {
    for (size_t i = 0; i < replay->stage_count; i++) {
        if (replay->replay_stages[i].type == type) {
            return &replay->replay_stages[i];
        }
    }
    return NULL;
}

/* Run every deadline due by now_ms at its own time. */
static int flush_until(struct replay *replay, uint32_t now_ms, bool all) {
    for (;;) {
        uint32_t deadline;
        if (!zpt_pipeline_next_deadline(&replay->pipeline, now_ms, &deadline) ||
            (!all && !due(now_ms, deadline))) {
            return 0;
        }
        struct zpt_pipeline_result result;
        replay->now_us = deadline * 1000U;
        if (zpt_pipeline_flush(&replay->pipeline, deadline, &result) < 0) {
            return fail("pipeline flush failed");
        }
    }
}

static int push_motion(struct replay *replay, uint32_t t_us, uint32_t t_ms, int32_t x, int32_t y,
                       uint32_t span_us) {
    struct zpt_signal signal = {
        .kind = ZPT_SIGNAL_RAW_MOTION,
        .metadata = {.observed_at_ms = t_ms,
                     .sample_span_us = span_us,
                     .resolution_cpi = replay->resolution_cpi},
        .data.raw_motion = {.x_counts = x, .y_counts = y},
    };
    struct zpt_pipeline_result result;
    replay->now_us = t_us;
    replay->have_frame_speed = false;
    if (zpt_pipeline_push(&replay->pipeline, &signal, &result) < 0) {
        return fail("pipeline push failed");
    }
    const struct replay_stage *quantizer = find_stage(replay, STAGE_QUANTIZER);
    const zpt_fixed_t speed = replay->have_frame_speed ? replay->frame_speed : -1;
    const zpt_fixed_t gain = replay->have_frame_speed
                                 ? zpt_cursor_acceleration_sensitivity(replay->acceleration, speed)
                                 : -1;
    printf("D\t%" PRIu32 "\t%" PRId32 "\t%" PRId32 "\t%" PRId64 "\t%" PRId64 "\t%d\t%" PRIu32
           "\t%" PRId64 "\t%" PRId64 "\n",
           t_ms, x, y, quantizer != NULL ? quantizer->state.quantizer.remainder_x : 0,
           quantizer != NULL ? quantizer->state.quantizer.remainder_y : 0,
           (signal.metadata.flags & ZPT_SIGNAL_FLAG_CLIPPED) != 0, t_us, speed, gain);
    return 0;
}

struct replay_line {
    char type;
    uint32_t t_us;
    uint32_t t_ms;
    int32_t x;
    int32_t y;
    uint32_t span_us;
};

/* Parse one event line; returns -EINVAL for a malformed one. */
static int parse_event(const char *line, struct replay_line *event) {
    uint32_t values[3] = {0};
    uint32_t late_ms = 0;
    int fields = sscanf(line, " %c %" SCNu32, &event->type, &event->t_us);
    if (fields != 2) {
        return -EINVAL;
    }
    if (event->type == 'M') {
        int32_t x;
        int32_t y;
        fields = sscanf(line, " %*c %*u %" SCNd32 " %" SCNd32 " %" SCNu32 " %" SCNu32, &x, &y,
                        &values[0], &late_ms);
        if (fields < 3) {
            return -EINVAL;
        }
        event->x = x;
        event->y = y;
        event->span_us = values[0];
    } else if (event->type == 'K' || event->type == 'F') {
        (void)sscanf(line, " %*c %*u %" SCNu32, &late_ms);
    } else {
        return -EINVAL;
    }
    event->t_ms = event->t_us / 1000U - late_ms;
    return 0;
}

static int replay_event(struct replay *replay, const char *line) {
    struct replay_line event;
    if (parse_event(line, &event) < 0 ||
        (!replay->recorded_flushes && flush_until(replay, event.t_ms, false) < 0)) {
        return fail("invalid replay event");
    }
    if (event.type == 'F') {
        if (!replay->recorded_flushes) {
            return fail("flush events need the F header line");
        }
        struct zpt_pipeline_result result;
        replay->now_us = event.t_us;
        return zpt_pipeline_flush(&replay->pipeline, event.t_ms, &result) < 0
                   ? fail("pipeline flush failed")
                   : 0;
    }
    if (event.type == 'K') {
        replay->keypress_seen = true;
        replay->last_keypress_ms = event.t_ms;
        return 0;
    }
    return push_motion(replay, event.t_us, event.t_ms, event.x, event.y, event.span_us);
}

int main(void) {
    static struct replay replay;
    char line[256];
    bool header_done = false;

    while (!header_done) {
        if (fgets(line, sizeof(line), stdin) == NULL ||
            read_header_line(&replay, line, &header_done) < 0) {
            fputs("invalid replay configuration\n", stderr);
            return 2;
        }
    }
    if (replay.resolution_cpi == 0U || build_pipeline(&replay) < 0) {
        fputs("invalid replay configuration\n", stderr);
        return 2;
    }
    while (fgets(line, sizeof(line), stdin) != NULL) {
        if (replay_event(&replay, line) < 0) {
            return 2;
        }
    }
    if (replay.recorded_flushes) {
        return 0;
    }
    return flush_until(&replay, 0U, true) < 0 ? 2 : 0;
}
