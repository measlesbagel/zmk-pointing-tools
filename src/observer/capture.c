/* SPDX-License-Identifier: MIT */

#include <errno.h>
#include <string.h>

#include <zmk/pointing_tools/observer/capture.h>
#include <zmk/pointing_tools/stage/cursor_coalescer.h>
#include <zmk/pointing_tools/stage/motion_gate/coherent_displacement.h>
#include <zmk/pointing_tools/stage/orientation.h>

static void put_le16(uint8_t *out, uint16_t value) {
    out[0] = (uint8_t)(value & 0xffU);
    out[1] = (uint8_t)(value >> 8);
}

static void put_le32(uint8_t *out, uint32_t value) {
    for (size_t i = 0; i < 4U; i++) {
        out[i] = (uint8_t)(value >> (8U * i));
    }
}

static int32_t saturate_int32(int64_t value) {
    return value > INT32_MAX ? INT32_MAX : (value < INT32_MIN ? INT32_MIN : (int32_t)value);
}

size_t zpt_capture_record_encode(const struct zpt_capture_record *record, uint8_t *out) {
    out[0] = record->kind;
    switch (record->kind) {
    case ZPT_CAPTURE_RECORD_FRAME:
        out[1] = record->flags;
        put_le32(&out[2], record->time_ms);
        put_le16(&out[6], record->time_us);
        put_le32(&out[8], (uint32_t)record->x);
        put_le32(&out[12], (uint32_t)record->y);
        put_le32(&out[16], record->span_us);
        return 20;
    case ZPT_CAPTURE_RECORD_SOURCE:
        out[1] = record->link;
        put_le16(&out[2], record->source_id);
        put_le16(&out[4], record->resolution_cpi);
        return 6;
    case ZPT_CAPTURE_RECORD_KEYPRESS:
    case ZPT_CAPTURE_RECORD_FLUSH:
        put_le32(&out[1], record->time_ms);
        return 5;
    default:
        return 1;
    }
}

static const char *const replayable_strategies[] = {
    "orthogonal-orientation", "resolution-normalize-mm", "coherent-displacement",
    "cursor-acceleration",    "cursor-transfer",         "cursor-quantizer",
    "cursor-coalescer",
};

static bool replayable_stage(const struct zpt_stage *stage) {
    for (size_t i = 0; i < sizeof(replayable_strategies) / sizeof(replayable_strategies[0]); i++) {
        if (strcmp(stage->api->strategy_id, replayable_strategies[i]) == 0) {
            return true;
        }
    }
    return false;
}

bool zpt_capture_pipeline_replayable(const struct zpt_pipeline *pipeline) {
    for (size_t i = 0; i < pipeline->stage_count; i++) {
        if (!replayable_stage(pipeline->stages[i])) {
            return false;
        }
    }
    return pipeline->stage_count > 0U;
}

static const struct zpt_stage *find_stage(const struct zpt_pipeline *pipeline,
                                          const char *strategy) {
    for (size_t i = 0; i < pipeline->stage_count; i++) {
        if (strcmp(pipeline->stages[i]->api->strategy_id, strategy) == 0) {
            return pipeline->stages[i];
        }
    }
    return NULL;
}

uint8_t zpt_capture_pipeline_link(const struct zpt_pipeline *pipeline) {
    const struct zpt_stage *stage = find_stage(pipeline, "cursor-coalescer");
    if (stage == NULL) {
        return ZPT_CAPTURE_LINK_NONE;
    }
    const struct zpt_cursor_coalescer_config *config = stage->config;
    const enum zpt_cursor_link link =
        config->current_link != NULL ? config->current_link() : ZPT_CURSOR_LINK_USB;
    return link == ZPT_CURSOR_LINK_BLE ? ZPT_CAPTURE_LINK_BLE : ZPT_CAPTURE_LINK_USB;
}

/* The keypress guard the pipeline's motion gate references, if it can be
 * frozen; capture records key presses only for such a guard. */
static const struct zpt_suppression_policy *find_guard(const struct zpt_pipeline *pipeline) {
    const struct zpt_stage *stage = find_stage(pipeline, "coherent-displacement");
    if (stage == NULL) {
        return NULL;
    }
    const struct zpt_coherent_displacement_stage_config *config = stage->config;
    const struct zpt_suppression_policy *guard = config->suppression;
    return guard != NULL && guard->freeze_keypress != NULL && guard->thaw_keypress != NULL ? guard
                                                                                           : NULL;
}

static void submit_kind(const struct zpt_capture_tap *tap, uint8_t kind, uint32_t time_ms) {
    const struct zpt_capture_record record = {.kind = kind, .time_ms = time_ms};
    tap->sink->submit(tap->sink->context, &record);
}

/* Describe the source and key press again before the next frame. */
static void restart(struct zpt_capture_tap *tap) {
    tap->described = false;
    tap->keypress_recorded = false;
    tap->have_last_frame = false;
}

static void reset_and_enter(struct zpt_capture_tap *tap) {
    /* Replay starts from reset stages, so the recorded pipeline does too. */
    zpt_pipeline_reset(tap->pipeline, ZPT_RESET_ADMINISTRATIVE);
    restart(tap);
    submit_kind(tap, ZPT_CAPTURE_RECORD_ENTERED, 0U);
}

/* How long the pipeline must rest before a resync: the acceleration
 * stage's restart gap, or the coalescer's report interval if longer, so a
 * reset coalescer sends the next report when the running one would. */
static uint32_t resync_rest_ms(const struct zpt_pipeline *pipeline) {
    const struct zpt_stage *stage = find_stage(pipeline, "cursor-coalescer");
    uint32_t interval = 0;
    if (stage != NULL) {
        const struct zpt_cursor_coalescer_config *config = stage->config;
        interval = zpt_capture_pipeline_link(pipeline) == ZPT_CAPTURE_LINK_BLE
                       ? config->ble_report_interval_ms
                       : config->usb_report_interval_ms;
    }
    return interval > ZPT_CAPTURE_RESYNC_REST_MS ? interval : ZPT_CAPTURE_RESYNC_REST_MS;
}

/* After a drop, resync at rest: no frame or flush for the rest time and no
 * deadline pending, so the gate is idle and the coalescer has nothing left
 * to send. The reset then clears only what a rest leaves behind: the
 * quantizer's sub-pixel remainder and the speed estimate the acceleration
 * stage blends a short sensor span after a rest into. */
static void resync_at_rest(struct zpt_capture_tap *tap, uint32_t now_ms) {
    uint32_t deadline_ms;
    if (!tap->replayable || !tap->have_last_frame ||
        (int32_t)(now_ms - tap->last_frame_ms) < (int32_t)resync_rest_ms(tap->pipeline) ||
        zpt_pipeline_next_deadline(tap->pipeline, now_ms, &deadline_ms) ||
        !tap->sink->take_resync(tap->sink->context)) {
        return;
    }
    reset_and_enter(tap);
}

static void describe_source(struct zpt_capture_tap *tap,
                            const struct zpt_signal_metadata *metadata) {
    const uint8_t link = zpt_capture_pipeline_link(tap->pipeline);
    if (tap->described && tap->source_id == metadata->source_id &&
        tap->resolution_cpi == metadata->resolution_cpi && tap->link == link) {
        return;
    }
    tap->source_id = metadata->source_id;
    tap->resolution_cpi = metadata->resolution_cpi;
    tap->link = link;
    tap->described = true;
    const struct zpt_capture_record source = {
        .kind = ZPT_CAPTURE_RECORD_SOURCE,
        .time_ms = metadata->observed_at_ms,
        .link = link,
        .source_id = metadata->source_id,
        .resolution_cpi = metadata->resolution_cpi,
    };
    tap->sink->submit(tap->sink->context, &source);
}

/* Freeze the guard for this frame and record its key press if it is new,
 * so the recording holds exactly the key press the stages decide with. */
static void record_keypress(struct zpt_capture_tap *tap) {
    if (tap->guard == NULL) {
        return;
    }
    uint32_t keypress_ms;
    const bool have = tap->guard->freeze_keypress(tap->guard->context, &keypress_ms);
    tap->frozen = true;
    if (have && (!tap->keypress_recorded || keypress_ms != tap->keypress_ms)) {
        tap->keypress_ms = keypress_ms;
        tap->keypress_recorded = true;
        submit_kind(tap, ZPT_CAPTURE_RECORD_KEYPRESS, keypress_ms);
    }
}

static void tap_input(const struct zpt_pipeline *pipeline, const struct zpt_signal *signal,
                      void *user_data) {
    (void)pipeline;
    struct zpt_capture_tap *tap = user_data;
    const struct zpt_capture_sink *sink = tap->sink;
    if (!sink->recording(sink->context) || signal->kind != ZPT_SIGNAL_RAW_MOTION) {
        return;
    }

    const struct zpt_signal_metadata *metadata = &signal->metadata;
    resync_at_rest(tap, metadata->observed_at_ms);
    describe_source(tap, metadata);
    record_keypress(tap);
    const struct zpt_capture_record frame = {
        .kind = ZPT_CAPTURE_RECORD_FRAME,
        .time_ms = metadata->observed_at_ms,
        .time_us = sink->sub_ms_us(sink->context, metadata->observed_at_ms),
        .flags = (uint8_t)(metadata->flags & 0xffU),
        .x = saturate_int32(signal->data.raw_motion.x_counts),
        .y = saturate_int32(signal->data.raw_motion.y_counts),
        .span_us = metadata->sample_span_us,
    };
    sink->submit(sink->context, &frame);
    tap->last_frame_ms = metadata->observed_at_ms;
    tap->have_last_frame = true;
}

static void tap_input_done(const struct zpt_pipeline *pipeline, void *user_data) {
    (void)pipeline;
    struct zpt_capture_tap *tap = user_data;
    if (tap->frozen) {
        tap->frozen = false;
        tap->guard->thaw_keypress(tap->guard->context);
    }
}

static void tap_flush(const struct zpt_pipeline *pipeline, uint32_t now_ms, void *user_data) {
    (void)pipeline;
    struct zpt_capture_tap *tap = user_data;
    if (tap->sink->recording(tap->sink->context)) {
        submit_kind(tap, ZPT_CAPTURE_RECORD_FLUSH, now_ms);
        /* A flush can send a report, so the rest counts from it too. */
        tap->last_frame_ms = now_ms;
    }
}

static void tap_lifecycle(const struct zpt_pipeline *pipeline, bool active, void *user_data) {
    (void)pipeline;
    struct zpt_capture_tap *tap = user_data;
    if (!tap->sink->recording(tap->sink->context)) {
        return;
    }
    if (active) {
        restart(tap);
    }
    submit_kind(tap, active ? ZPT_CAPTURE_RECORD_ENTERED : ZPT_CAPTURE_RECORD_LEFT, 0U);
}

void zpt_capture_tap_attach(struct zpt_capture_tap *tap, struct zpt_pipeline *pipeline,
                            const struct zpt_capture_sink *sink) {
    *tap = (struct zpt_capture_tap){
        .sink = sink,
        .pipeline = pipeline,
        .guard = find_guard(pipeline),
        .replayable = zpt_capture_pipeline_replayable(pipeline),
    };
    pipeline->observer = (struct zpt_pipeline_observer){
        .input = tap_input,
        .input_done = tap_input_done,
        .flush = tap_flush,
        .lifecycle = tap_lifecycle,
        .user_data = tap,
    };
}

void zpt_capture_tap_start(struct zpt_capture_tap *tap) {
    restart(tap);
    if (tap->replayable && tap->pipeline->active) {
        reset_and_enter(tap);
    }
}

void zpt_capture_batch_clear(struct zpt_capture_batch *batch) {
    batch->length = ZPT_CAPTURE_BATCH_HEADER_SIZE;
    batch->count = 0;
    batch->next_sequence = 0;
}

bool zpt_capture_batch_add(struct zpt_capture_batch *batch,
                           const struct zpt_capture_record *record) {
    uint8_t encoded[ZPT_CAPTURE_RECORD_MAX_SIZE];
    const size_t length = zpt_capture_record_encode(record, encoded);
    if (batch->count > 0U &&
        (record->sequence != batch->next_sequence || batch->count == UINT8_MAX ||
         batch->length + length > sizeof(batch->payload))) {
        return false;
    }
    if (batch->count == 0U) {
        batch->length = ZPT_CAPTURE_BATCH_HEADER_SIZE;
        put_le32(&batch->payload[0], record->sequence);
    }
    memcpy(&batch->payload[batch->length], encoded, length);
    batch->length = (uint16_t)(batch->length + length);
    batch->count++;
    batch->payload[4] = batch->count;
    batch->next_sequence = record->sequence + 1U;
    return true;
}

/* Settings a stage takes that runtime tuning doesn't publish. */
struct fixed_setting {
    const char *key;
    int32_t value;
};

static size_t fixed_settings(const struct zpt_stage *stage, struct fixed_setting settings[3]) {
    const char *strategy = stage->api->strategy_id;
    if (strcmp(strategy, "orthogonal-orientation") == 0) {
        const struct zpt_orientation_config *config = stage->config;
        settings[0] = (struct fixed_setting){"swap-xy", config->swap_xy};
        settings[1] = (struct fixed_setting){"invert-x", config->invert_x};
        settings[2] = (struct fixed_setting){"invert-y", config->invert_y};
        return 3;
    }
    if (strcmp(strategy, "coherent-displacement") == 0) {
        const struct zpt_coherent_displacement_stage_config *config = stage->config;
        const uint32_t window =
            config->suppression != NULL ? config->suppression->keypress_window_ms : 0U;
        settings[0] = (struct fixed_setting){"suppress-after-keypress-ms", (int32_t)window};
        return 1;
    }
    return 0;
}

struct writer {
    uint8_t *out;
    size_t capacity;
    size_t offset;
    bool overflow;
};

static void write_bytes(struct writer *writer, const void *data, size_t length) {
    if (writer->overflow || writer->capacity - writer->offset < length) {
        writer->overflow = true;
        return;
    }
    memcpy(&writer->out[writer->offset], data, length);
    writer->offset += length;
}

static void write_u8(struct writer *writer, uint8_t value) { write_bytes(writer, &value, 1); }

static void write_string(struct writer *writer, const char *text) {
    const size_t length = strlen(text);
    if (length > UINT8_MAX) {
        writer->overflow = true;
        return;
    }
    write_u8(writer, (uint8_t)length);
    write_bytes(writer, text, length);
}

uint32_t zpt_capture_settings_hash_add(uint32_t hash, const char *stable_id, const char *key,
                                       int32_t value) {
    uint32_t fnv = 2166136261U;
    const char *const parts[] = {stable_id, key};
    for (size_t part = 0; part < 2U; part++) {
        for (const char *c = parts[part]; *c != '\0'; c++) {
            fnv = (fnv ^ (uint8_t)*c) * 16777619U;
        }
        fnv = (fnv ^ 0U) * 16777619U;
    }
    uint8_t bytes[4];
    put_le32(bytes, (uint32_t)value);
    for (size_t i = 0; i < sizeof(bytes); i++) {
        fnv = (fnv ^ bytes[i]) * 16777619U;
    }
    return hash + fnv;
}

int zpt_capture_describe_pipeline(const struct zpt_pipeline *pipeline, uint8_t pipeline_id,
                                  uint32_t settings_hash, uint8_t *out, size_t capacity) {
    struct writer writer = {.out = out, .capacity = capacity};
    uint8_t hash[4];
    put_le32(hash, settings_hash);
    write_u8(&writer, pipeline_id);
    write_string(&writer, pipeline->stable_id);
    write_bytes(&writer, hash, sizeof(hash));
    write_u8(&writer, (uint8_t)pipeline->stage_count);
    for (size_t i = 0; i < pipeline->stage_count; i++) {
        const struct zpt_stage *stage = pipeline->stages[i];
        struct fixed_setting settings[3];
        const size_t count = fixed_settings(stage, settings);
        write_string(&writer, stage->api->strategy_id);
        write_string(&writer, stage->stable_id);
        write_u8(&writer, (uint8_t)count);
        for (size_t j = 0; j < count; j++) {
            uint8_t value[4];
            put_le32(value, (uint32_t)settings[j].value);
            write_string(&writer, settings[j].key);
            write_bytes(&writer, value, sizeof(value));
        }
    }
    return writer.overflow || pipeline->stage_count > UINT8_MAX ? -ENOSPC : (int)writer.offset;
}
