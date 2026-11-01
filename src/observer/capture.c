/* SPDX-License-Identifier: MIT */

#include <errno.h>
#include <string.h>

#include <zmk/pointing_tools/observer/capture.h>
#include <zmk/pointing_tools/stage/axis_constraint.h>
#include <zmk/pointing_tools/stage/axis_intent.h>
#include <zmk/pointing_tools/stage/cursor_coalescer.h>
#include <zmk/pointing_tools/stage/motion_gate/coherent_displacement.h>
#include <zmk/pointing_tools/stage/orientation.h>
#include <zmk/pointing_tools/stage/scroll_batcher.h>
#include <zmk/pointing_tools/stage/text_nav.h>

static void put_le16(uint8_t *out, uint16_t value) {
    out[0] = (uint8_t)(value & 0xffU);
    out[1] = (uint8_t)(value >> 8);
}

static void put_le32(uint8_t *out, uint32_t value) {
    for (size_t i = 0; i < 4U; i++) {
        out[i] = (uint8_t)(value >> (8U * i));
    }
}

static uint32_t get_le32(const uint8_t *bytes) {
    return (uint32_t)bytes[0] | (uint32_t)bytes[1] << 8 | (uint32_t)bytes[2] << 16 |
           (uint32_t)bytes[3] << 24;
}

static int32_t saturate_int32(int64_t value) {
    return value > INT32_MAX ? INT32_MAX : (value < INT32_MIN ? INT32_MIN : (int32_t)value);
}

/* Encode one record in full, without its sequence; out holds
 * ZPT_CAPTURE_RECORD_MAX_SIZE bytes. Returns the encoded length. */
static size_t encode_full(const struct zpt_capture_record *record, uint8_t *out) {
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
    case ZPT_CAPTURE_RECORD_ENTERED:
        put_le32(&out[1], record->settings_hash);
        return 5;
    default:
        return 1;
    }
}

static const char *const replayable_strategies[] = {
    "orthogonal-orientation",
    "resolution-normalize-mm",
    "coherent-displacement",
    "cursor-acceleration",
    "cursor-transfer",
    "cursor-quantizer",
    "cursor-coalescer",
    "axis-intent",
    "axis-constraint",
    "scroll-batcher",
    "text-nav",
};

static bool replayable_stage(const struct zpt_stage *stage) {
    for (size_t i = 0; i < sizeof(replayable_strategies) / sizeof(replayable_strategies[0]); i++) {
        if (strcmp(stage->api->strategy_id, replayable_strategies[i]) == 0) {
            return true;
        }
    }
    return false;
}

static bool is_strategy(const struct zpt_stage *stage, const char *strategy) {
    return strcmp(stage->api->strategy_id, strategy) == 0;
}

/* The suppression policy a stage references, for the stages that take one. */
static const struct zpt_suppression_policy *stage_guard(const struct zpt_stage *stage) {
    if (is_strategy(stage, "coherent-displacement")) {
        return ((const struct zpt_coherent_displacement_stage_config *)stage->config)->suppression;
    }
    if (is_strategy(stage, "axis-intent")) {
        return ((const struct zpt_axis_intent_stage_config *)stage->config)->suppression;
    }
    if (is_strategy(stage, "axis-constraint")) {
        return ((const struct zpt_axis_constraint_config *)stage->config)->suppression;
    }
    if (is_strategy(stage, "scroll-batcher")) {
        return ((const struct zpt_scroll_batcher_config *)stage->config)->suppression;
    }
    return NULL;
}

static bool freezable(const struct zpt_suppression_policy *guard) {
    return guard != NULL && guard->freeze_keypress != NULL && guard->thaw_keypress != NULL;
}

/* The keypress guard the pipeline's stages reference, if it can be frozen;
 * capture records key presses only for such a guard. */
static const struct zpt_suppression_policy *find_guard(const struct zpt_pipeline *pipeline) {
    for (size_t i = 0; i < pipeline->stage_count; i++) {
        const struct zpt_suppression_policy *guard = stage_guard(pipeline->stages[i]);
        if (freezable(guard)) {
            return guard;
        }
    }
    return NULL;
}

/* Replay knows every stage, and the stages that take a keypress guard
 * share one that capture can freeze, so the recording holds every key
 * press they decide with. */
bool zpt_capture_pipeline_replayable(const struct zpt_pipeline *pipeline) {
    const struct zpt_suppression_policy *guard = find_guard(pipeline);
    for (size_t i = 0; i < pipeline->stage_count; i++) {
        const struct zpt_suppression_policy *own = stage_guard(pipeline->stages[i]);
        if (!replayable_stage(pipeline->stages[i]) || (own != NULL && own != guard)) {
            return false;
        }
    }
    return pipeline->stage_count > 0U;
}

static const struct zpt_stage *find_stage(const struct zpt_pipeline *pipeline,
                                          const char *strategy) {
    for (size_t i = 0; i < pipeline->stage_count; i++) {
        if (is_strategy(pipeline->stages[i], strategy)) {
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

static void submit_kind(const struct zpt_capture_tap *tap, uint8_t kind, uint32_t time_ms) {
    const struct zpt_capture_record record = {.kind = kind, .time_ms = time_ms};
    tap->sink->submit(tap->sink->context, &record);
}

/* Describe the source and key press again before the next frame. */
static void restart(struct zpt_capture_tap *tap) {
    tap->described = false;
    tap->keypress_recorded = false;
    tap->have_last_frame = false;
    tap->resync_pending = false;
}

static void enter(const struct zpt_capture_tap *tap) {
    const struct zpt_capture_record record = {
        .kind = ZPT_CAPTURE_RECORD_ENTERED,
        .settings_hash = tap->sink->settings_hash(tap->sink->context),
    };
    tap->sink->submit(tap->sink->context, &record);
}

static void reset_and_enter(struct zpt_capture_tap *tap) {
    /* Replay starts from reset stages, so the recorded pipeline does too. */
    zpt_pipeline_reset(tap->pipeline, ZPT_RESET_ADMINISTRATIVE);
    restart(tap);
    enter(tap);
}

/* The longest a stage remembers motion across a pause: its idle timeout
 * or report interval. */
static uint32_t stage_memory_ms(const struct zpt_pipeline *pipeline,
                                const struct zpt_stage *stage) {
    if (is_strategy(stage, "cursor-coalescer")) {
        const struct zpt_cursor_coalescer_config *config = stage->config;
        return zpt_capture_pipeline_link(pipeline) == ZPT_CAPTURE_LINK_BLE
                   ? config->ble_report_interval_ms
                   : config->usb_report_interval_ms;
    }
    if (is_strategy(stage, "axis-intent")) {
        return ((const struct zpt_axis_intent_stage_config *)stage->config)->idle_timeout_ms;
    }
    if (is_strategy(stage, "axis-constraint")) {
        const struct zpt_axis_constraint_config *config = stage->config;
        /* It forgets its axis after the idle timeout, but a held remainder
         * waits for the fold interval, so the longer of the two. */
        return config->idle_timeout_ms > config->fold_interval_ms ? config->idle_timeout_ms
                                                                  : config->fold_interval_ms;
    }
    if (is_strategy(stage, "scroll-batcher")) {
        return ((const struct zpt_scroll_batcher_config *)stage->config)->report_interval_ms;
    }
    if (is_strategy(stage, "text-nav")) {
        return ((const struct zpt_text_nav_config *)stage->config)->idle_timeout_ms;
    }
    return 0;
}

/* How long the pipeline must rest before a resync: the acceleration
 * stage's restart gap, or the longest any stage remembers motion if
 * longer, so the reset clears only what a rest leaves behind. */
static uint32_t resync_rest_ms(const struct zpt_pipeline *pipeline) {
    uint32_t rest = ZPT_CAPTURE_RESYNC_REST_MS;
    for (size_t i = 0; i < pipeline->stage_count; i++) {
        const uint32_t memory = stage_memory_ms(pipeline, pipeline->stages[i]);
        rest = memory > rest ? memory : rest;
    }
    return rest;
}

/* After a drop, a settings change, or a source change, resync at rest: no
 * frame or flush for the rest time and no
 * deadline pending, so the gate is idle and the coalescer has nothing left
 * to send. The reset then clears only what a rest leaves behind: the
 * quantizer's sub-pixel remainder and the speed estimate the acceleration
 * stage blends a short sensor span after a rest into. */
static void resync_at_rest(struct zpt_capture_tap *tap, uint32_t now_ms) {
    uint32_t deadline_ms;
    if (!tap->replayable || !tap->have_last_frame ||
        (int32_t)(now_ms - tap->last_frame_ms) < (int32_t)resync_rest_ms(tap->pipeline) ||
        zpt_pipeline_next_deadline(tap->pipeline, now_ms, &deadline_ms)) {
        return;
    }
    const bool requested = tap->sink->take_resync(tap->sink->context);
    if (requested || tap->resync_pending) {
        reset_and_enter(tap);
    }
}

static void describe_source(struct zpt_capture_tap *tap,
                            const struct zpt_signal_metadata *metadata) {
    const uint8_t link = zpt_capture_pipeline_link(tap->pipeline);
    if (tap->described && tap->source_id == metadata->source_id &&
        tap->resolution_cpi == metadata->resolution_cpi && tap->link == link) {
        return;
    }
    /* Replay can't follow a change mid-trace; start a new one at rest. */
    tap->resync_pending |= tap->described;
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

static void tap_done(const struct zpt_pipeline *pipeline, void *user_data) {
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
        /* A flush can hand held motion to a stage that asks the guard, as
         * the axis constraint does to the scroll batcher; freeze it and
         * record a new key press first, as for a frame. */
        record_keypress(tap);
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
        enter(tap);
    } else {
        submit_kind(tap, ZPT_CAPTURE_RECORD_LEFT, 0U);
    }
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
        .input_done = tap_done,
        .flush = tap_flush,
        .flush_done = tap_done,
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

/* Internal to the buffer, never sent: kind, then how many records were
 * dropped (u32) before the record that follows. */
#define GAP_MARKER 0x7fU
#define GAP_MARKER_SIZE 5U

static size_t put_varint(uint8_t *out, uint32_t value) {
    size_t length = 0;
    while (value >= 0x80U) {
        out[length++] = (uint8_t)(value | 0x80U);
        value >>= 7;
    }
    out[length++] = (uint8_t)value;
    return length;
}

static uint32_t zigzag(int32_t value) {
    return ((uint32_t)value << 1) ^ (value < 0 ? UINT32_MAX : 0U);
}

static size_t varint_size(uint32_t value) {
    uint8_t scratch[5];
    return put_varint(scratch, value);
}

static uint64_t frame_us(const struct zpt_capture_record *record) {
    return (uint64_t)record->time_ms * 1000U + record->time_us;
}

/* A frame as a delta from the buffer's previous frame, or 0 when it has to
 * go whole. out holds ZPT_CAPTURE_RECORD_MAX_SIZE + 2 bytes. */
static size_t encode_delta(const struct zpt_capture_buffer *buffer,
                           const struct zpt_capture_record *record, uint8_t *out) {
    const int64_t elapsed = (int64_t)(frame_us(record) - buffer->previous_us);
    if (!buffer->have_previous || elapsed > INT32_MAX || elapsed < INT32_MIN) {
        return 0;
    }
    uint8_t mode = ZPT_CAPTURE_SPAN_ABSOLUTE;
    if (record->span_us == buffer->previous_span_us) {
        mode = ZPT_CAPTURE_SPAN_PREVIOUS;
    } else if (elapsed >= 0 && record->span_us == (uint64_t)elapsed) {
        mode = ZPT_CAPTURE_SPAN_ELAPSED;
    } else if ((int64_t)record->span_us - elapsed >= INT32_MIN &&
               (int64_t)record->span_us - elapsed <= INT32_MAX &&
               varint_size(zigzag((int32_t)((int64_t)record->span_us - elapsed))) <
                   varint_size(record->span_us)) {
        mode = ZPT_CAPTURE_SPAN_ELAPSED_PLUS;
    }
    const bool flags = record->flags != buffer->previous_flags;
    size_t length = 1;
    out[0] = (uint8_t)(ZPT_CAPTURE_DELTA_FRAME | (flags ? ZPT_CAPTURE_DELTA_FLAGS : 0U) | mode);
    length += put_varint(&out[length], zigzag((int32_t)elapsed));
    length += put_varint(&out[length], zigzag(record->x));
    length += put_varint(&out[length], zigzag(record->y));
    if (flags) {
        out[length++] = record->flags;
    }
    if (mode == ZPT_CAPTURE_SPAN_ELAPSED_PLUS) {
        length += put_varint(&out[length], zigzag((int32_t)((int64_t)record->span_us - elapsed)));
    } else if (mode == ZPT_CAPTURE_SPAN_ABSOLUTE) {
        length += put_varint(&out[length], record->span_us);
    }
    return length < ZPT_CAPTURE_RECORD_MAX_SIZE ? length : 0;
}

static size_t varint_length(const uint8_t *bytes, size_t available) {
    size_t length = 0;
    while (length < available && length < 5U && (bytes[length] & 0x80U) != 0U) {
        length++;
    }
    return length + 1U;
}

/* The length of the stored record or gap marker that starts bytes. */
static size_t stored_length(const uint8_t *bytes, size_t available) {
    const uint8_t kind = bytes[0];
    if ((kind & ZPT_CAPTURE_DELTA_FRAME) != 0U) {
        size_t length = 1;
        for (int field = 0; field < 3; field++) {
            length += varint_length(&bytes[length], available - length);
        }
        length += (kind & ZPT_CAPTURE_DELTA_FLAGS) != 0U ? 1U : 0U;
        const uint8_t mode = kind & 0x03U;
        if (mode == ZPT_CAPTURE_SPAN_ELAPSED_PLUS || mode == ZPT_CAPTURE_SPAN_ABSOLUTE) {
            length += varint_length(&bytes[length], available - length);
        }
        return length;
    }
    switch (kind) {
    case ZPT_CAPTURE_RECORD_FRAME:
        return 20;
    case ZPT_CAPTURE_RECORD_SOURCE:
        return 6;
    case ZPT_CAPTURE_RECORD_KEYPRESS:
    case ZPT_CAPTURE_RECORD_FLUSH:
    case ZPT_CAPTURE_RECORD_ENTERED:
    case GAP_MARKER:
        return 5;
    default:
        return 1;
    }
}

static void ring_write(struct zpt_capture_buffer *buffer, const uint8_t *bytes, uint32_t length) {
    uint32_t tail = (buffer->head + buffer->used) % buffer->capacity;
    for (uint32_t i = 0; i < length; i++) {
        buffer->data[tail] = bytes[i];
        tail = tail + 1U == buffer->capacity ? 0U : tail + 1U;
    }
    buffer->used += length;
}

static void ring_read(const struct zpt_capture_buffer *buffer, uint32_t offset, uint8_t *out,
                      uint32_t length) {
    uint32_t position = (buffer->head + offset) % buffer->capacity;
    for (uint32_t i = 0; i < length; i++) {
        out[i] = buffer->data[position];
        position = position + 1U == buffer->capacity ? 0U : position + 1U;
    }
}

void zpt_capture_buffer_init(struct zpt_capture_buffer *buffer, uint8_t *storage,
                             uint32_t capacity) {
    *buffer = (struct zpt_capture_buffer){.data = storage, .capacity = capacity};
}

void zpt_capture_buffer_reset(struct zpt_capture_buffer *buffer) {
    zpt_capture_buffer_init(buffer, buffer->data, buffer->capacity);
}

bool zpt_capture_buffer_put(struct zpt_capture_buffer *buffer,
                            const struct zpt_capture_record *record) {
    buffer->next_sequence++;
    /* Every trace starts with a whole frame, so a host that lost its place
     * picks up again at the next entered record. */
    if (record->kind == ZPT_CAPTURE_RECORD_ENTERED) {
        buffer->have_previous = false;
    }
    uint8_t encoded[ZPT_CAPTURE_RECORD_MAX_SIZE + 2U];
    size_t length = 0;
    const bool frame = record->kind == ZPT_CAPTURE_RECORD_FRAME;
    if (frame) {
        length = encode_delta(buffer, record, encoded);
    }
    if (length == 0U) {
        length = encode_full(record, encoded);
    }
    const uint32_t needed = (uint32_t)length + (buffer->pending_gap > 0U ? GAP_MARKER_SIZE : 0U);
    if (buffer->capacity - buffer->used < needed) {
        buffer->pending_gap++;
        /* The host can't follow a delta past a lost frame. */
        buffer->have_previous = false;
        return false;
    }
    if (buffer->pending_gap > 0U) {
        uint8_t marker[GAP_MARKER_SIZE] = {GAP_MARKER};
        put_le32(&marker[1], buffer->pending_gap);
        ring_write(buffer, marker, sizeof(marker));
        buffer->pending_gap = 0;
    }
    ring_write(buffer, encoded, (uint32_t)length);
    buffer->records++;
    if (frame) {
        buffer->previous_us = frame_us(record);
        buffer->previous_span_us = record->span_us;
        buffer->previous_flags = record->flags;
        buffer->have_previous = true;
    }
    return true;
}

size_t zpt_capture_buffer_peek(const struct zpt_capture_buffer *buffer, uint8_t *out,
                               size_t capacity, struct zpt_capture_take *take) {
    uint32_t offset = 0;
    uint32_t sequence = buffer->out_sequence;
    size_t length = ZPT_CAPTURE_BATCH_HEADER_SIZE;
    uint32_t count = 0;
    while (offset < buffer->used && count < UINT8_MAX) {
        uint8_t record[ZPT_CAPTURE_RECORD_MAX_SIZE] = {0};
        const uint32_t left = buffer->used - offset;
        const uint32_t available = left < sizeof(record) ? left : (uint32_t)sizeof(record);
        ring_read(buffer, offset, record, available);
        const size_t record_length = stored_length(record, available);
        if (record[0] == GAP_MARKER) {
            /* Records after a gap start the next batch. */
            if (count > 0U) {
                break;
            }
            sequence += get_le32(&record[1]);
        } else {
            if (length + record_length > capacity) {
                break;
            }
            memcpy(&out[length], record, record_length);
            length += record_length;
            count++;
        }
        offset += (uint32_t)record_length;
    }
    *take = (struct zpt_capture_take){
        .bytes = offset, .records = count, .next_sequence = sequence + count};
    if (count == 0U) {
        return 0;
    }
    put_le32(out, sequence);
    out[4] = (uint8_t)count;
    return length;
}

void zpt_capture_buffer_consume(struct zpt_capture_buffer *buffer,
                                const struct zpt_capture_take *take) {
    buffer->head = (buffer->head + take->bytes) % buffer->capacity;
    buffer->used -= take->bytes;
    buffer->records -= take->records;
    buffer->out_sequence = take->next_sequence;
}

uint32_t zpt_capture_buffer_discard(struct zpt_capture_buffer *buffer) {
    const uint32_t lost = buffer->records;
    buffer->head = 0;
    buffer->used = 0;
    buffer->records = 0;
    buffer->pending_gap = 0;
    buffer->out_sequence = buffer->next_sequence;
    buffer->have_previous = false;
    return lost;
}

/* Settings a stage takes that runtime tuning doesn't publish. */
struct fixed_setting {
    const char *key;
    int32_t value;
};

static size_t fixed_settings(const struct zpt_pipeline *pipeline, const struct zpt_stage *stage,
                             struct fixed_setting settings[3]) {
    const char *strategy = stage->api->strategy_id;
    if (strcmp(strategy, "orthogonal-orientation") == 0) {
        const struct zpt_orientation_config *config = stage->config;
        settings[0] = (struct fixed_setting){"swap-xy", config->swap_xy};
        settings[1] = (struct fixed_setting){"invert-x", config->invert_x};
        settings[2] = (struct fixed_setting){"invert-y", config->invert_y};
        return 3;
    }
    size_t count = 0;
    if (strcmp(strategy, "axis-intent") == 0) {
        const struct zpt_axis_intent_stage_config *config = stage->config;
        settings[count++] = (struct fixed_setting){"policy", (int32_t)config->policy};
    }
    if (strcmp(strategy, "coherent-displacement") == 0 || strcmp(strategy, "axis-intent") == 0 ||
        strcmp(strategy, "axis-constraint") == 0 || strcmp(strategy, "scroll-batcher") == 0) {
        /* Capture records key presses only for a guard it can freeze; a
         * stage without one replays without it. */
        const struct zpt_suppression_policy *guard = find_guard(pipeline);
        const uint32_t window =
            guard != NULL && stage_guard(stage) == guard ? guard->keypress_window_ms : 0U;
        settings[count++] = (struct fixed_setting){"suppress-after-keypress-ms", (int32_t)window};
    }
    return count;
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
        const size_t count = fixed_settings(pipeline, stage, settings);
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
