/* SPDX-License-Identifier: MIT */

/*
 * Records a synthetic input capture the way firmware does, for the capture
 * replay check (host/replay/capture-check.js).
 *
 * A cursor pipeline built from the firmware's stages runs synthetic motion
 * with sub-millisecond arrivals, sensor spans, clipped spans, key presses
 * under the keypress guard, and stage deadlines flushed at their time, as
 * the router executor does. Partway through, a recording starts as the
 * telemetry service starts one: the pipeline resets, an entered record and
 * the latest key press follow, and the capture tap records from then on.
 *
 * Prints one JSON object: the protocol bytes a host would receive (the
 * capture description and batches, as hex), the settings as a tuner
 * profile export, and every cursor report the pipeline sent while
 * recording. Replaying the converted capture must reproduce those reports.
 */

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include <zmk/pointing_tools/core/pipeline.h>
#include <zmk/pointing_tools/observer/capture.h>
#include <zmk/pointing_tools/policy/suppression.h>
#include <zmk/pointing_tools/stage/cursor_acceleration.h>
#include <zmk/pointing_tools/stage/cursor_coalescer.h>
#include <zmk/pointing_tools/stage/cursor_quantizer.h>
#include <zmk/pointing_tools/stage/cursor_transfer.h>
#include <zmk/pointing_tools/stage/motion_gate/coherent_displacement.h>
#include <zmk/pointing_tools/stage/orientation.h>
#include <zmk/pointing_tools/stage/resolution_normalize.h>
#include <zmk/pointing_tools/stage/stage_tuning.h>

#define STAGE_COUNT 7
#define MAX_STREAM 65536
#define MAX_OUTPUTS 4096
#define CAPTURE_DESCRIBE_RESPONSE 0x8d
#define CAPTURE_BATCH_EVENT 0x92
#define KEYPRESS_WINDOW_MS 40U
#define RESOLUTION_CPI 800U
#define SOURCE_ID 3U

struct setting {
    const char *key;
    int32_t value;
};

struct tuned_stage {
    const char *stable_id;
    const struct zpt_stage_tuning_ops *ops;
    void *config;
    const struct setting *settings;
    size_t setting_count;
};

static const struct setting gate_settings[] = {
    {"enabled", 1},        {"activation-distance", 220},
    {"coherence", 60},     {"qualification-timeout", 160},
    {"idle-timeout", 120},
};
static const struct setting acceleration_settings[] = {
    {"curve-speed-1", 0},         {"curve-speed-2", 8},         {"curve-speed-3", 20},
    {"curve-speed-4", 45},        {"curve-speed-5", 90},        {"curve-speed-6", 160},
    {"curve-speed-7", 260},       {"curve-speed-8", 400},       {"curve-sensitivity-1", 65},
    {"curve-sensitivity-2", 66},  {"curve-sensitivity-3", 73},  {"curve-sensitivity-4", 99},
    {"curve-sensitivity-5", 159}, {"curve-sensitivity-6", 225}, {"curve-sensitivity-7", 264},
    {"curve-sensitivity-8", 283}, {"speed-rise", 10},           {"speed-fall", 5},
};
static const struct setting transfer_settings[] = {{"scale-multiplier", 3}, {"scale-divisor", 2}};
static const struct setting coalescer_settings[] = {{"usb-report-interval", 1},
                                                    {"ble-report-interval", 8}};

struct session {
    uint32_t now_us;
    bool recording;
    bool have_keypress;
    uint32_t last_keypress_ms;
    uint32_t sequence;
    struct zpt_capture_batch batch;
    uint8_t stream[MAX_STREAM];
    size_t stream_length;
    int32_t outputs[MAX_OUTPUTS][3];
    size_t output_count;
    uint32_t rng;

    struct zpt_suppression_policy suppression;
    struct zpt_orientation_config orientation;
    struct zpt_coherent_displacement_stage_config gate;
    struct zpt_coherent_displacement_stage_state gate_state;
    struct zpt_cursor_acceleration_config acceleration;
    struct zpt_cursor_acceleration_state acceleration_state;
    struct zpt_cursor_transfer_config transfer;
    struct zpt_cursor_quantizer_state quantizer_state;
    struct zpt_cursor_coalescer_config coalescer;
    struct zpt_cursor_coalescer_state coalescer_state;
    struct zpt_stage stages[STAGE_COUNT];
    struct zpt_stage *stage_refs[STAGE_COUNT];
    struct zpt_sink sink;
    struct zpt_pipeline pipeline;
    struct zpt_capture_sink capture_sink;
    struct zpt_capture_tap tap;
};

static struct session session;

static enum zpt_cursor_link ble_link(void) { return ZPT_CURSOR_LINK_BLE; }

static uint32_t next_random(void) {
    session.rng ^= session.rng << 13;
    session.rng ^= session.rng >> 17;
    session.rng ^= session.rng << 5;
    return session.rng;
}

static uint32_t random_between(uint32_t low, uint32_t high) {
    return low + next_random() % (high - low + 1U);
}

static bool guard_suppressed(void *context, const struct zpt_signal *signal, uint32_t now_ms) {
    (void)context;
    (void)signal;
    return session.have_keypress && now_ms - session.last_keypress_ms < KEYPRESS_WINDOW_MS;
}

static int report_emit(struct zpt_sink *sink, const struct zpt_signal *signal) {
    (void)sink;
    if (session.recording && session.output_count < MAX_OUTPUTS) {
        int32_t *output = session.outputs[session.output_count++];
        output[0] = (int32_t)signal->metadata.observed_at_ms;
        output[1] = signal->data.delta.x;
        output[2] = signal->data.delta.y;
    }
    return 0;
}

static const struct zpt_sink_api report_api = {
    .type_id = "report-capture",
    .accepted_kinds = ZPT_SIGNAL_KIND_MASK(ZPT_SIGNAL_POINTER_DELTA),
    .emit = report_emit,
};

static void stream_frame(uint8_t type, const uint8_t *payload, size_t length) {
    if (session.stream_length + 5U + length > sizeof(session.stream)) {
        fputs("capture stream full\n", stderr);
        return;
    }
    uint8_t *out = &session.stream[session.stream_length];
    out[0] = 0x5a;
    out[1] = 0x50;
    out[2] = type;
    out[3] = (uint8_t)(length & 0xffU);
    out[4] = (uint8_t)(length >> 8);
    memcpy(&out[5], payload, length);
    session.stream_length += 5U + length;
}

static void send_batch(void) {
    if (session.batch.count > 0U) {
        stream_frame(CAPTURE_BATCH_EVENT, session.batch.payload, session.batch.length);
    }
    zpt_capture_batch_clear(&session.batch);
}

/* The telemetry service's path: number the record and batch it. */
static void capture_submit(void *context, const struct zpt_capture_record *record) {
    (void)context;
    struct zpt_capture_record numbered = *record;
    numbered.sequence = session.sequence++;
    if (!zpt_capture_batch_add(&session.batch, &numbered)) {
        send_batch();
        (void)zpt_capture_batch_add(&session.batch, &numbered);
    }
}

static bool capture_recording(void *context) {
    (void)context;
    return session.recording;
}

static uint16_t capture_sub_ms_us(void *context, uint32_t time_ms) {
    (void)context;
    return session.now_us / 1000U == time_ms ? (uint16_t)(session.now_us % 1000U) : 999U;
}

static int apply_settings(const struct tuned_stage *stage) {
    for (size_t i = 0; i < stage->setting_count; i++) {
        bool found = false;
        for (size_t j = 0; j < stage->ops->parameter_count; j++) {
            const struct zpt_tuning_parameter *parameter = &stage->ops->parameters[j];
            if (strcmp(parameter->key, stage->settings[i].key) == 0) {
                found =
                    stage->ops->put(stage->config, parameter->id, stage->settings[i].value) == 0;
            }
        }
        if (!found) {
            fprintf(stderr, "%s: can't set %s\n", stage->stable_id, stage->settings[i].key);
            return -EINVAL;
        }
    }
    uint8_t failed = 0;
    return stage->ops->validate != NULL ? stage->ops->validate(stage->config, &failed) : 0;
}

static const struct tuned_stage *tuned_stages(size_t *count) {
    static struct tuned_stage stages[4];
    stages[0] = (struct tuned_stage){"cursor-gate", &zpt_coherent_displacement_tuning_ops,
                                     &session.gate, gate_settings, 5};
    stages[1] = (struct tuned_stage){"cursor-acceleration", &zpt_cursor_acceleration_tuning_ops,
                                     &session.acceleration, acceleration_settings, 18};
    stages[2] = (struct tuned_stage){"cursor-transfer", &zpt_cursor_transfer_tuning_ops,
                                     &session.transfer, transfer_settings, 2};
    stages[3] = (struct tuned_stage){"cursor-coalescer", &zpt_cursor_coalescer_tuning_ops,
                                     &session.coalescer, coalescer_settings, 2};
    *count = 4;
    return stages;
}

static void add_stage(size_t index, const char *stable_id, const struct zpt_stage_api *api,
                      const void *config, void *state) {
    session.stages[index] = (struct zpt_stage){
        .stable_id = stable_id,
        .api = api,
        .config = config,
        .state = state,
    };
    session.stage_refs[index] = &session.stages[index];
}

static int build_pipeline(void) {
    session.suppression = (struct zpt_suppression_policy){
        .is_suppressed = guard_suppressed,
        .keypress_window_ms = KEYPRESS_WINDOW_MS,
    };
    session.orientation = (struct zpt_orientation_config){.invert_x = true};
    session.gate.suppression = &session.suppression;
    session.coalescer.current_link = ble_link;
    size_t count;
    const struct tuned_stage *tuned = tuned_stages(&count);
    for (size_t i = 0; i < count; i++) {
        if (apply_settings(&tuned[i]) < 0) {
            return -EINVAL;
        }
    }
    add_stage(0, "cursor-orientation", &zpt_orthogonal_orientation_stage_api, &session.orientation,
              NULL);
    add_stage(1, "cursor-normalize", &zpt_resolution_normalize_stage_api, NULL, NULL);
    add_stage(2, "cursor-gate", &zpt_coherent_displacement_stage_api, &session.gate,
              &session.gate_state);
    add_stage(3, "cursor-acceleration", &zpt_cursor_acceleration_stage_api, &session.acceleration,
              &session.acceleration_state);
    add_stage(4, "cursor-transfer", &zpt_cursor_transfer_stage_api, &session.transfer, NULL);
    add_stage(5, "cursor-quantizer", &zpt_cursor_quantizer_stage_api, NULL,
              &session.quantizer_state);
    add_stage(6, "cursor-coalescer", &zpt_cursor_coalescer_stage_api, &session.coalescer,
              &session.coalescer_state);
    session.sink = (struct zpt_sink){.stable_id = "cursor", .api = &report_api};
    session.pipeline = (struct zpt_pipeline){
        .stable_id = "test-cursor",
        .input_kind = ZPT_SIGNAL_RAW_MOTION,
        .stages = session.stage_refs,
        .stage_count = STAGE_COUNT,
        .sink = &session.sink,
        .dispatch_budget = 16,
    };
    session.capture_sink = (struct zpt_capture_sink){
        .recording = capture_recording,
        .submit = capture_submit,
        .sub_ms_us = capture_sub_ms_us,
    };
    zpt_capture_tap_attach(&session.tap, &session.pipeline, &session.capture_sink);
    if (zpt_pipeline_validate(&session.pipeline) < 0 ||
        zpt_pipeline_activate(&session.pipeline, ZPT_RESET_PIPELINE_ENTERED) < 0) {
        return -EINVAL;
    }
    return 0;
}

/* Run the deadlines due before now, each at its own millisecond, as the
 * router executor's delayed work does. */
static int run_deadlines(uint32_t until_ms) {
    uint32_t deadline;
    while (zpt_pipeline_next_deadline(&session.pipeline, until_ms, &deadline) &&
           (int32_t)(until_ms - deadline) >= 0) {
        struct zpt_pipeline_result result;
        session.now_us = deadline * 1000U;
        if (zpt_pipeline_flush(&session.pipeline, deadline, &result) < 0) {
            return -EIO;
        }
    }
    return 0;
}

static int advance(uint32_t delta_us) {
    const uint32_t target = session.now_us + delta_us;
    int ret = run_deadlines(target / 1000U);
    session.now_us = target;
    return ret;
}

static void keypress(void) {
    session.have_keypress = true;
    session.last_keypress_ms = session.now_us / 1000U;
    if (session.recording) {
        const struct zpt_capture_record record = {.kind = ZPT_CAPTURE_RECORD_KEYPRESS,
                                                  .time_ms = session.last_keypress_ms};
        capture_submit(NULL, &record);
    }
}

static int push_frame(int32_t x, int32_t y, uint32_t span_us, uint32_t flags) {
    const struct zpt_signal signal = {
        .kind = ZPT_SIGNAL_RAW_MOTION,
        .metadata =
            {
                .observed_at_ms = session.now_us / 1000U,
                .sample_span_us = span_us,
                .flags = ZPT_SIGNAL_FLAG_LOCAL | flags,
                .source_id = SOURCE_ID,
                .resolution_cpi = RESOLUTION_CPI,
            },
        .data.raw_motion = {.x_counts = x, .y_counts = y},
    };
    struct zpt_pipeline_result result;
    return zpt_pipeline_push(&session.pipeline, &signal, &result);
}

/* What starting a recording does in the telemetry service. */
static void start_recording(void) {
    session.recording = true;
    session.sequence = 0;
    zpt_capture_tap_restart(&session.tap);
    zpt_pipeline_reset(&session.pipeline, ZPT_RESET_ADMINISTRATIVE);
    const struct zpt_capture_record entered = {.kind = ZPT_CAPTURE_RECORD_ENTERED};
    capture_submit(NULL, &entered);
    if (session.have_keypress) {
        const struct zpt_capture_record pressed = {.kind = ZPT_CAPTURE_RECORD_KEYPRESS,
                                                   .time_ms = session.last_keypress_ms};
        capture_submit(NULL, &pressed);
    }
}

/* One stroke: frames at a cadence, a speed, and a direction; spans are the
 * time since the previous read, sometimes clipped as a split link does. */
static int stroke(uint32_t frames, int32_t step_x, int32_t step_y, uint32_t min_gap_us,
                  uint32_t max_gap_us) {
    for (uint32_t i = 0; i < frames; i++) {
        const uint32_t gap = random_between(min_gap_us, max_gap_us);
        if (advance(gap) < 0) {
            return -EIO;
        }
        if (random_between(0, 40) == 0) {
            keypress();
        }
        const int32_t jitter_x = (int32_t)random_between(0, 2) - 1;
        const int32_t jitter_y = (int32_t)random_between(0, 2) - 1;
        const bool clipped = random_between(0, 50) == 0;
        if (push_frame(step_x + jitter_x, step_y + jitter_y, clipped ? 30000U : gap,
                       clipped ? ZPT_SIGNAL_FLAG_SAMPLE_SPAN_CLIPPED : 0U) < 0) {
            return -EIO;
        }
    }
    return 0;
}

static int run_session(void) {
    session.rng = 0x1234567U;
    session.now_us = 5000000U + 377U;
    zpt_capture_batch_clear(&session.batch);
    int ret = build_pipeline();
    /* Before the recording: motion and a key press the guard remembers. */
    ret = ret < 0 ? ret : stroke(40, 3, 1, 900, 2500);
    keypress();
    ret = ret < 0 ? ret : advance(12000);
    start_recording();
    /* Slow aims, a flick, typing shake, and pauses past the idle timeouts. */
    static const int32_t strokes[][5] = {
        {120, 1, 0, 1500, 9000},  {60, 0, -2, 800, 3000}, {80, 14, 9, 900, 1300},
        {30, -1, 1, 3000, 45000}, {90, -6, 3, 700, 1100}, {50, 2, 2, 1000, 20000},
    };
    for (size_t i = 0; ret == 0 && i < sizeof(strokes) / sizeof(strokes[0]); i++) {
        ret = stroke((uint32_t)strokes[i][0], strokes[i][1], strokes[i][2], (uint32_t)strokes[i][3],
                     (uint32_t)strokes[i][4]);
        keypress();
        ret = ret < 0 ? ret : advance(random_between(10000, 300000));
    }
    /* Let every pending deadline run, then end the recording. */
    ret = ret < 0 ? ret : advance(1000000);
    send_batch();
    return ret;
}

static void print_hex(const uint8_t *bytes, size_t length) {
    for (size_t i = 0; i < length; i++) {
        printf("%02x", bytes[i]);
    }
}

static void print_profile(void) {
    size_t count;
    const struct tuned_stage *tuned = tuned_stages(&count);
    printf("\"profile\":{\"schema\":\"zmk-pointing-tools/tuning-profile\",\"version\":1,"
           "\"targets\":[");
    for (size_t i = 0; i < count; i++) {
        printf("%s{\"stableId\":\"%s\",\"kind\":4,\"parameters\":[", i == 0 ? "" : ",",
               tuned[i].stable_id);
        for (size_t j = 0; j < tuned[i].setting_count; j++) {
            printf("%s{\"key\":\"%s\",\"value\":%" PRId32 ",\"compiled\":%" PRId32 "}",
                   j == 0 ? "" : ",", tuned[i].settings[j].key, tuned[i].settings[j].value,
                   tuned[i].settings[j].value);
        }
        printf("]}");
    }
    printf("]}");
}

int main(void) {
    if (run_session() < 0) {
        fputs("capture session failed\n", stderr);
        return 1;
    }
    uint8_t description[512];
    const int length =
        zpt_capture_describe_pipeline(&session.pipeline, 0, description, sizeof(description));
    if (length < 0) {
        fputs("pipeline description failed\n", stderr);
        return 1;
    }
    /* The host asks for the description before it starts recording. */
    const uint8_t header[] = {0x5a, 0x50, CAPTURE_DESCRIBE_RESPONSE, (uint8_t)(length & 0xff),
                              (uint8_t)(length >> 8)};
    printf("{\"stream\":\"");
    print_hex(header, sizeof(header));
    print_hex(description, (size_t)length);
    print_hex(session.stream, session.stream_length);
    printf("\",");
    print_profile();
    printf(",\"outputs\":[");
    for (size_t i = 0; i < session.output_count; i++) {
        printf("%s[%" PRId32 ",%" PRId32 ",%" PRId32 "]", i == 0 ? "" : ",", session.outputs[i][0],
               session.outputs[i][1], session.outputs[i][2]);
    }
    printf("]}\n");
    return 0;
}
