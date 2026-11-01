/* SPDX-License-Identifier: MIT */

/*
 * Records a synthetic input capture the way firmware does, for the capture
 * replay check (host/replay/capture-check.js).
 *
 * A cursor pipeline built from the firmware's stages runs synthetic motion
 * with sub-millisecond arrivals, sensor spans, clipped spans, key presses
 * under the keypress guard, and stage deadlines flushed up to 3 ms late, as
 * a busy router executor does, so frames sometimes follow a later flush.
 * Some key presses land while a frame runs, after the guard was read for
 * it. Partway through, a recording starts through zpt_capture_tap_start,
 * as the telemetry service starts one. Records go through the firmware's
 * burst buffer and leave over a simulated Bluetooth link: a few credits of
 * 62-byte notifications per connection event. The simulation is generous:
 * every event refills the whole credit budget and HID reports never share
 * the connection, so a real link is slower. Fast flicks outrun the link;
 * a small buffer fills and drops records, and a tuning change lands
 * mid-stroke, as an agent's A/B preview does; the tap resyncs at the next
 * rest after each.
 *
 *   capture_session [buffer-bytes [credits [interval-us]]]
 *
 * Prints one JSON object: the protocol bytes a host would receive (the
 * capture description and batches, as hex), the settings as a tuner
 * profile export, and every cursor report the pipeline sent while
 * recording, tagged with the trace it belongs to (the sequence of the
 * entered record that starts it); reports sent between a drop and the
 * resync belong to none. Replaying each converted trace must reproduce its reports.
 */

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
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
/* Longer than the resync rest, so a key press is re-recorded after one. */
#define KEYPRESS_WINDOW_MS 100U
#define MAX_PROFILE 4096
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
    /* The guard's frozen view while a frame runs. */
    bool frozen;
    bool frozen_have;
    uint32_t frozen_ms;
    /* Records still to drop, whether a drop awaits a resync, and the trace
     * reports belong to (-1 between a drop and the resync). */
    bool resync;
    /* Key presses land during strokes; not during the fast flicks. */
    bool typing;
    int32_t trace;
    uint32_t dropped;
    uint32_t injected_keypresses;
    uint32_t late_flushes;
    uint32_t settings_hash;
    uint32_t start_hash;
    /* Profile exports before and after the tuning change. */
    char profiles[2][MAX_PROFILE];
    /* The burst buffer and the simulated link. */
    struct zpt_capture_buffer buffer;
    uint8_t storage[65536];
    uint32_t credits;
    uint32_t interval_us;
    uint32_t next_event_us;
    uint32_t records;
    uint32_t frames;
    uint32_t frame_bytes;
    uint32_t peak_used;
    uint8_t stream[MAX_STREAM];
    size_t stream_length;
    int32_t outputs[MAX_OUTPUTS][4];
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

/* The keypress guard, frozen for capture as the ZMK one is. */
static bool guard_suppressed(void *context, const struct zpt_signal *signal, uint32_t now_ms) {
    (void)context;
    (void)signal;
    const bool have = session.frozen ? session.frozen_have : session.have_keypress;
    const uint32_t keypress_ms = session.frozen ? session.frozen_ms : session.last_keypress_ms;
    return have && now_ms - keypress_ms < KEYPRESS_WINDOW_MS;
}

static bool guard_freeze(void *context, uint32_t *time_ms) {
    (void)context;
    session.frozen = true;
    session.frozen_have = session.have_keypress;
    session.frozen_ms = session.last_keypress_ms;
    *time_ms = session.frozen_ms;
    return session.frozen_have;
}

static void guard_thaw(void *context) {
    (void)context;
    session.frozen = false;
}

static int report_emit(struct zpt_sink *sink, const struct zpt_signal *signal) {
    (void)sink;
    if (session.recording && session.trace >= 0 && session.output_count < MAX_OUTPUTS) {
        int32_t *output = session.outputs[session.output_count++];
        output[0] = session.trace;
        output[1] = (int32_t)signal->metadata.observed_at_ms;
        output[2] = signal->data.delta.x;
        output[3] = signal->data.delta.y;
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

/* Send batches while the link has credits, as the telemetry worker does;
 * a batch needs one credit per 62-byte notification of its frame. Like the
 * worker, peek from a copy of the bookkeeping. Returns whether the buffer
 * is empty. */
static bool send_batches(uint32_t credits) {
    while (credits > 0U) {
        const struct zpt_capture_buffer snapshot = session.buffer;
        uint8_t payload[ZPT_CAPTURE_BATCH_MAX_PAYLOAD];
        struct zpt_capture_take take;
        const size_t length = zpt_capture_buffer_peek(&snapshot, payload, sizeof(payload), &take);
        if (length == 0U) {
            return true;
        }
        const uint32_t needed = (uint32_t)(5U + length + 61U) / 62U;
        if (needed > credits) {
            return false;
        }
        credits -= needed;
        stream_frame(CAPTURE_BATCH_EVENT, payload, length);
        zpt_capture_buffer_consume(&session.buffer, &take);
    }
    return session.buffer.used == 0U;
}

/* The connection events up to now_us, each with a fresh credit budget. */
static void run_link(uint32_t now_us) {
    while ((int32_t)(now_us - session.next_event_us) >= 0) {
        (void)send_batches(session.credits);
        session.next_event_us += session.interval_us;
    }
}

static void keypress(void) {
    session.have_keypress = true;
    session.last_keypress_ms = session.now_us / 1000U;
}

/* The telemetry service's path: store the record in the burst buffer, or
 * drop it when the buffer is full, still numbered, so the host sees the
 * gap, and ask the tap to resync. */
static void capture_submit(void *context, const struct zpt_capture_record *record) {
    (void)context;
    if (record->kind == ZPT_CAPTURE_RECORD_SETTINGS_CHANGED) {
        session.trace = -1;
    }
    session.records++;
    const uint32_t before = session.buffer.used + (session.buffer.pending_gap > 0U ? 5U : 0U);
    if (!zpt_capture_buffer_put(&session.buffer, record)) {
        session.dropped++;
        session.resync = true;
        session.trace = -1;
        return;
    }
    /* Reports from here on belong to the trace this entered record starts. */
    if (record->kind == ZPT_CAPTURE_RECORD_ENTERED) {
        session.trace = (int32_t)(session.buffer.next_sequence - 1U);
    }
    if (record->kind == ZPT_CAPTURE_RECORD_FRAME) {
        session.frames++;
        session.frame_bytes += session.buffer.used - before;
    }
    if (session.buffer.used > session.peak_used) {
        session.peak_used = session.buffer.used;
    }
    /* A key press landing while the frame runs, after the tap read the
     * guard for it: the stages must not see it until the next frame. */
    if (record->kind == ZPT_CAPTURE_RECORD_FRAME && session.typing && random_between(0, 30) == 0) {
        keypress();
        session.injected_keypresses++;
    }
}

static uint32_t capture_settings_hash(void *context) {
    (void)context;
    return session.settings_hash;
}

static bool capture_take_resync(void *context) {
    (void)context;
    const bool resync = session.resync;
    session.resync = false;
    return resync;
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

/* The firmware's settings hash: every published parameter of every tuned
 * stage, at its current value. */
static uint32_t settings_hash(void) {
    size_t count;
    const struct tuned_stage *tuned = tuned_stages(&count);
    uint32_t hash = 0;
    for (size_t i = 0; i < count; i++) {
        for (size_t j = 0; j < tuned[i].ops->parameter_count; j++) {
            const struct zpt_tuning_parameter *parameter = &tuned[i].ops->parameters[j];
            int32_t value = 0;
            (void)tuned[i].ops->get(tuned[i].config, parameter->id, &value);
            hash = zpt_capture_settings_hash_add(hash, tuned[i].stable_id, parameter->key, value);
        }
    }
    return hash;
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
        .freeze_keypress = guard_freeze,
        .thaw_keypress = guard_thaw,
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
        .take_resync = capture_take_resync,
        .settings_hash = capture_settings_hash,
    };
    zpt_capture_tap_attach(&session.tap, &session.pipeline, &session.capture_sink);
    if (zpt_pipeline_validate(&session.pipeline) < 0 ||
        zpt_pipeline_activate(&session.pipeline, ZPT_RESET_PIPELINE_ENTERED) < 0) {
        return -EINVAL;
    }
    return 0;
}

/* Run the deadlines due by until_ms as the router executor's delayed work
 * does, each 0 to 3 ms late, so a flush can run after the millisecond of
 * the frame that follows it. */
static int run_deadlines(uint32_t until_ms) {
    uint32_t deadline;
    while (zpt_pipeline_next_deadline(&session.pipeline, until_ms, &deadline) &&
           (int32_t)(until_ms - deadline) >= 0) {
        struct zpt_pipeline_result result;
        const uint32_t late = random_between(0, 3);
        const uint32_t flush_ms = deadline + late;
        session.late_flushes += late > 0U ? 1U : 0U;
        session.now_us = flush_ms * 1000U;
        run_link(session.now_us);
        if (zpt_pipeline_flush(&session.pipeline, flush_ms, &result) < 0) {
            return -EIO;
        }
    }
    return 0;
}

static int advance(uint32_t delta_us) {
    const uint32_t target = session.now_us + delta_us;
    int ret = run_deadlines(target / 1000U);
    session.now_us = target;
    run_link(target);
    return ret;
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
    const int ret = zpt_pipeline_push(&session.pipeline, &signal, &result);
    /* The tap must thaw the guard once the pipeline is done with a frame. */
    if (session.frozen) {
        fputs("keypress guard left frozen after a frame\n", stderr);
        return -EIO;
    }
    return ret;
}

static int change_settings(void);
static void export_profile(char *out, size_t capacity);

/* What starting a recording does in the telemetry service
 * (zpt_capture_start_between_frames). */
static void start_recording(void) {
    zpt_capture_buffer_reset(&session.buffer);
    session.next_event_us = session.now_us;
    session.resync = false;
    session.trace = -1;
    session.settings_hash = settings_hash();
    session.start_hash = session.settings_hash;
    export_profile(session.profiles[0], sizeof(session.profiles[0]));
    session.recording = true;
    zpt_capture_tap_start(&session.tap);
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
        if (session.typing && random_between(0, 40) == 0) {
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

static int run_session(uint32_t buffer_bytes) {
    session.rng = 0x1234567U;
    session.trace = -1;
    session.now_us = 5000000U + 377U;
    session.typing = true;
    zpt_capture_buffer_init(&session.buffer, session.storage, buffer_bytes);
    int ret = build_pipeline();
    /* Before the recording: motion and a key press the guard remembers. */
    ret = ret < 0 ? ret : stroke(40, 3, 1, 900, 2500);
    keypress();
    ret = ret < 0 ? ret : advance(12000);
    start_recording();
    /* Slow aims, flicks, typing shake, and pauses past the idle timeouts;
     * the two flicks near the end read the sensor about every half
     * millisecond, with no typing, then a slow aim follows them. */
    static const int32_t strokes[][5] = {
        {120, 1, 0, 1500, 9000},  {60, 0, -2, 800, 3000},   {80, 14, 9, 900, 1300},
        {30, -1, 1, 3000, 45000}, {90, -6, 3, 700, 1100},   {50, 2, 2, 1000, 20000},
        {300, 70, -40, 400, 700}, {250, -90, 60, 400, 700}, {60, 1, 1, 1500, 4000},
    };
    for (size_t i = 0; ret == 0 && i < sizeof(strokes) / sizeof(strokes[0]); i++) {
        /* An A/B preview mid-stroke. */
        if (i == 4) {
            ret = ret < 0 ? ret
                          : stroke(30, (int32_t)strokes[i][1], (int32_t)strokes[i][2],
                                   (uint32_t)strokes[i][3], (uint32_t)strokes[i][4]);
            ret = ret < 0 ? ret : change_settings();
        }
        session.typing = i < 6 || i > 7;
        ret = ret < 0 ? ret
                      : stroke((uint32_t)strokes[i][0], strokes[i][1], strokes[i][2],
                               (uint32_t)strokes[i][3], (uint32_t)strokes[i][4]);
        keypress();
        /* After the tuning change, rest just past the resync gap but inside
         * the guard's window, so the resync re-records the key press that
         * ended the stroke. After a flick the gate's idle deadline (120 ms)
         * runs first, so the rest that resyncs ends 40 ms after it. */
        ret = ret < 0 ? ret
                      : advance(i == 4             ? 60000U
                                : i == 6 || i == 7 ? 200000U
                                                   : random_between(10000, 300000));
    }
    /* Let every pending deadline run, then end the recording: firmware
     * sends what is stored before the status, whatever the link rate. */
    ret = ret < 0 ? ret : advance(1000000);
    (void)send_batches(UINT32_MAX);
    return ret;
}

static void print_hex(const uint8_t *bytes, size_t length) {
    for (size_t i = 0; i < length; i++) {
        printf("%02x", bytes[i]);
    }
}

/* The current settings as a tuner profile export. */
static void export_profile(char *out, size_t capacity) {
    size_t count;
    const struct tuned_stage *tuned = tuned_stages(&count);
    size_t used = (size_t)snprintf(out, capacity,
                                   "{\"schema\":\"zmk-pointing-tools/tuning-profile\","
                                   "\"version\":1,\"targets\":[");
    for (size_t i = 0; i < count && used < capacity; i++) {
        used += (size_t)snprintf(&out[used], capacity - used,
                                 "%s{\"stableId\":\"%s\",\"kind\":4,\"parameters\":[",
                                 i == 0 ? "" : ",", tuned[i].stable_id);
        for (size_t j = 0; j < tuned[i].ops->parameter_count && used < capacity; j++) {
            const struct zpt_tuning_parameter *parameter = &tuned[i].ops->parameters[j];
            int32_t value = 0;
            (void)tuned[i].ops->get(tuned[i].config, parameter->id, &value);
            used += (size_t)snprintf(&out[used], capacity - used,
                                     "%s{\"key\":\"%s\",\"value\":%" PRId32 ",\"compiled\":%" PRId32
                                     "}",
                                     j == 0 ? "" : ",", parameter->key, value, value);
        }
        if (used < capacity) {
            used += (size_t)snprintf(&out[used], capacity - used, "]}");
        }
    }
    if (used < capacity) {
        (void)snprintf(&out[used], capacity - used, "]}");
    }
}

/* A tuning change landing between frames, as the service's
 * zpt_capture_settings_changed reports it. */
static int change_settings(void) {
    for (size_t j = 0; j < zpt_cursor_transfer_tuning_ops.parameter_count; j++) {
        const struct zpt_tuning_parameter *parameter =
            &zpt_cursor_transfer_tuning_ops.parameters[j];
        if (strcmp(parameter->key, "scale-multiplier") == 0 &&
            zpt_cursor_transfer_tuning_ops.put(&session.transfer, parameter->id, 2) < 0) {
            return -EINVAL;
        }
    }
    session.settings_hash = settings_hash();
    const struct zpt_capture_record changed = {.kind = ZPT_CAPTURE_RECORD_SETTINGS_CHANGED};
    capture_submit(NULL, &changed);
    session.resync = true;
    export_profile(session.profiles[1], sizeof(session.profiles[1]));
    return 0;
}

int main(int argc, char **argv) {
    const uint32_t buffer_bytes = argc > 1 ? (uint32_t)strtoul(argv[1], NULL, 0) : 4096U;
    session.credits = argc > 2 ? (uint32_t)strtoul(argv[2], NULL, 0) : 2U;
    session.interval_us = argc > 3 ? (uint32_t)strtoul(argv[3], NULL, 0) : 7500U;
    if (buffer_bytes == 0U || buffer_bytes > sizeof(session.storage) || session.credits < 2U ||
        session.interval_us == 0U) {
        fputs("usage: capture_session [buffer-bytes [credits >= 2 [interval-us]]]\n", stderr);
        return 2;
    }
    if (run_session(buffer_bytes) < 0) {
        fputs("capture session failed\n", stderr);
        return 1;
    }
    uint8_t description[512];
    const int length = zpt_capture_describe_pipeline(&session.pipeline, 0, session.start_hash,
                                                     description, sizeof(description));
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
    printf("\"profiles\":[%s,%s]", session.profiles[0], session.profiles[1]);
    printf(",\"records\":%" PRIu32 ",\"dropped\":%" PRIu32 ",\"frames\":%" PRIu32
           ",\"frameBytes\":%" PRIu32 ",\"peakBuffer\":%" PRIu32 ",\"bufferSize\":%" PRIu32
           ",\"injectedKeypresses\":%" PRIu32 ",\"lateFlushes\":%" PRIu32 ",\"outputs\":[",
           session.records, session.dropped, session.frames, session.frame_bytes, session.peak_used,
           buffer_bytes, session.injected_keypresses, session.late_flushes);
    for (size_t i = 0; i < session.output_count; i++) {
        printf("%s[%" PRId32 ",%" PRId32 ",%" PRId32 ",%" PRId32 "]", i == 0 ? "" : ",",
               session.outputs[i][0], session.outputs[i][1], session.outputs[i][2],
               session.outputs[i][3]);
    }
    printf("]}\n");
    return 0;
}
