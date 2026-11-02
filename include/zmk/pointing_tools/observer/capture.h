/* SPDX-License-Identifier: MIT */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zmk/pointing_tools/core/pipeline.h>
#include <zmk/pointing_tools/policy/suppression.h>

/*
 * Input capture: records of what goes into one pipeline, so a host can
 * replay it exactly (docs/protocol.md, "Input capture").
 *
 * Platform-neutral, so the telemetry service and host tests produce the same
 * bytes. A tap observes one pipeline and turns what it sees into records; a
 * sink decides whether a recording is running and takes the records. Batches
 * and pipeline descriptions are the payloads of the protocol messages.
 */

/* 2: compact delta frames and the burst buffer fill in capture status.
 * 3: device uptime in capture status, so a host can line its own clock up
 * with record times. */
#define ZPT_CAPTURE_SCHEMA_VERSION 3
/* A batch starts with the first record's sequence (u32) and a count (u8). */
#define ZPT_CAPTURE_BATCH_HEADER_SIZE 5
/* Two notifications at the 65-byte ATT MTU ZMK negotiates, less the frame
 * header, so a batch fits the default Bluetooth credit budget. */
#define ZPT_CAPTURE_BATCH_MAX_PAYLOAD 119
/* A full frame record; a delta frame is shorter or isn't used. */
#define ZPT_CAPTURE_RECORD_MAX_SIZE 20
/* After a drop, a settings change, or a change of source, resolution, or
 * link, the tap resyncs at the first frame that follows at least this long
 * a pause, or the coalescer's report interval if longer, with no stage
 * deadline pending: the acceleration stage's restart gap
 * (ZPT_CURSOR_ACCELERATION_RESTART_MS). */
#define ZPT_CAPTURE_RESYNC_REST_MS 40U

enum zpt_capture_record_kind {
    /* An input frame pushed into the pipeline. */
    ZPT_CAPTURE_RECORD_FRAME = 1,
    /* Source, resolution, and report link of the frames that follow. */
    ZPT_CAPTURE_RECORD_SOURCE = 2,
    /* The key press the pipeline's keypress guard decides the next frame
     * with, at the time the guard holds. */
    ZPT_CAPTURE_RECORD_KEYPRESS = 3,
    /* The pipeline was activated or reset: its stages start from reset,
     * with the settings the hash identifies. */
    ZPT_CAPTURE_RECORD_ENTERED = 4,
    /* The pipeline was deactivated. */
    ZPT_CAPTURE_RECORD_LEFT = 5,
    /* A tuning change landed while recording. */
    ZPT_CAPTURE_RECORD_SETTINGS_CHANGED = 6,
    /* Stage deadlines that were due ran. */
    ZPT_CAPTURE_RECORD_FLUSH = 7,
};

/* Wire kinds 0x80 to 0x87 are frames encoded as a delta from the previous
 * frame: bit 2 says a flags byte follows, bits 0 and 1 how the span is
 * given (enum zpt_capture_span_mode). */
#define ZPT_CAPTURE_DELTA_FRAME 0x80U
#define ZPT_CAPTURE_DELTA_FLAGS 0x04U

enum zpt_capture_span_mode {
    /* The previous frame's span. */
    ZPT_CAPTURE_SPAN_PREVIOUS = 0,
    /* The time since the previous frame. */
    ZPT_CAPTURE_SPAN_ELAPSED = 1,
    /* A signed varint difference from the time since the previous frame. */
    ZPT_CAPTURE_SPAN_ELAPSED_PLUS = 2,
    /* An unsigned varint span. */
    ZPT_CAPTURE_SPAN_ABSOLUTE = 3,
};

/* The link the pipeline's cursor reports go out on, from its coalescer. */
enum zpt_capture_link {
    ZPT_CAPTURE_LINK_USB = 0,
    ZPT_CAPTURE_LINK_BLE = 1,
    ZPT_CAPTURE_LINK_NONE = UINT8_MAX,
};

struct zpt_capture_record {
    /* Numbers the records of a recording from zero; a gap is a drop. */
    uint32_t sequence;
    /* Milliseconds of uptime: for a frame, the time its stages see. */
    uint32_t time_ms;
    /* Frame: microseconds into time_ms, 0 to 999. */
    uint16_t time_us;
    uint8_t kind;
    /* Frame: bits 0 to 7 of the signal's metadata flags. */
    uint8_t flags;
    /* Source record. */
    uint8_t link;
    uint16_t source_id;
    uint16_t resolution_cpi;
    /* Frame: raw counts, saturated to 32 bits, and the sensor sample span. */
    int32_t x;
    int32_t y;
    uint32_t span_us;
    /* Entered record: the pipeline's settings hash. */
    uint32_t settings_hash;
};

/* Where a tap's records go. recording is checked first on every frame, so
 * it must be cheap; submit numbers and stores a record; sub_ms_us returns
 * how many microseconds into time_ms it is now, or 999 once time_ms has
 * passed; take_resync returns and clears whether records were dropped or
 * settings changed since it was last asked; settings_hash returns the
 * recorded pipeline's settings hash now. Callbacks run on the thread that
 * drives the pipeline. */
struct zpt_capture_sink {
    bool (*recording)(void *context);
    void (*submit)(void *context, const struct zpt_capture_record *record);
    uint16_t (*sub_ms_us)(void *context, uint32_t time_ms);
    bool (*take_resync)(void *context);
    uint32_t (*settings_hash)(void *context);
    void *context;
};

/* Observes one pipeline for capture. */
struct zpt_capture_tap {
    const struct zpt_capture_sink *sink;
    struct zpt_pipeline *pipeline;
    /* The pipeline's keypress guard, or NULL. */
    const struct zpt_suppression_policy *guard;
    uint32_t keypress_ms;
    /* The last frame or flush recorded. */
    uint32_t last_frame_ms;
    uint16_t source_id;
    uint16_t resolution_cpi;
    uint8_t link;
    bool described;
    bool keypress_recorded;
    bool have_last_frame;
    bool frozen;
    bool replayable;
    /* The source, resolution, or link changed: resync at the next rest. */
    bool resync_pending;
};

/* Install the tap as the pipeline's observer. Call while no frame runs. */
void zpt_capture_tap_attach(struct zpt_capture_tap *tap, struct zpt_pipeline *pipeline,
                            const struct zpt_capture_sink *sink);

/* Start a recording: describe the source and key press again before the
 * next frame and, for a replayable pipeline that is active, reset its
 * stages and submit an entered record, so the recording starts where replay
 * starts. Call while no frame runs, with the sink already recording. */
void zpt_capture_tap_start(struct zpt_capture_tap *tap);

/* Whether every stage of the pipeline is one the host cursor runner
 * replays; only those pipelines are reset by a recording. */
bool zpt_capture_pipeline_replayable(const struct zpt_pipeline *pipeline);

/* The link the pipeline's cursor coalescer reads now, or
 * ZPT_CAPTURE_LINK_NONE without a coalescer. */
uint8_t zpt_capture_pipeline_link(const struct zpt_pipeline *pipeline);

/* Store and forward: records are encoded compactly as they are recorded
 * and wait here until the link has room, so only the average record rate
 * has to fit the link (docs/protocol.md, "Burst buffer"). A frame encodes
 * as a delta from the previous frame, falling back to a full frame record
 * at the start, after a drop, and when a delta doesn't fit.
 *
 * Not thread-safe: the owner serialises every call. */
struct zpt_capture_buffer {
    uint8_t *data;
    uint32_t capacity;
    uint32_t head;
    uint32_t used;
    /* Records stored, not counting gap markers. */
    uint32_t records;
    /* The number the next recorded record gets. */
    uint32_t next_sequence;
    /* The number of the first record still stored, before any gap. */
    uint32_t out_sequence;
    /* Records dropped since the last one stored. */
    uint32_t pending_gap;
    /* The previous frame stored, the base of the next delta. */
    uint64_t previous_us;
    uint32_t previous_span_us;
    uint8_t previous_flags;
    bool have_previous;
};

/* What a batch peeked from the buffer covers, for zpt_capture_buffer_consume. */
struct zpt_capture_take {
    uint32_t bytes;
    uint32_t records;
    uint32_t next_sequence;
};

void zpt_capture_buffer_init(struct zpt_capture_buffer *buffer, uint8_t *storage,
                             uint32_t capacity);

/* Empty the buffer and number records from zero: a new recording. */
void zpt_capture_buffer_reset(struct zpt_capture_buffer *buffer);

/* Number the record and store it. Returns false when the buffer has no
 * room: the record is dropped, still numbered, so the host sees the gap,
 * and the next frame is stored whole. */
bool zpt_capture_buffer_put(struct zpt_capture_buffer *buffer,
                            const struct zpt_capture_record *record);

/* Encode the oldest stored records into one capture batch payload of at
 * most capacity bytes, without removing them; records after a gap go in
 * the next batch. Returns the payload length, 0 when nothing is stored.
 * Reads only stored bytes and the bookkeeping, never writes: it may run on
 * a copy of the struct taken under the owner's lock, while puts continue,
 * as long as nothing consumes, discards, or resets until the batch is
 * consumed. */
size_t zpt_capture_buffer_peek(const struct zpt_capture_buffer *buffer, uint8_t *out,
                               size_t capacity, struct zpt_capture_take *take);

/* Remove what a peeked batch covers, once the link took it. */
void zpt_capture_buffer_consume(struct zpt_capture_buffer *buffer,
                                const struct zpt_capture_take *take);

/* Drop everything stored, for a link that went away; the next batch starts
 * after the gap. Returns the records dropped. */
uint32_t zpt_capture_buffer_discard(struct zpt_capture_buffer *buffer);

/* Add one tuning parameter's current value to a pipeline's settings hash:
 * the sum, modulo 2^32, of FNV-1a over stage stable id, NUL, key, NUL, and
 * the value as 4 little-endian bytes, for every published parameter of
 * every stage with a tuning target. Order doesn't matter. */
uint32_t zpt_capture_settings_hash_add(uint32_t hash, const char *stable_id, const char *key,
                                       int32_t value);

/* Encode the capture pipeline description (docs/protocol.md). Returns the
 * length, or -ENOSPC when it doesn't fit in capacity. */
int zpt_capture_describe_pipeline(const struct zpt_pipeline *pipeline, uint8_t pipeline_id,
                                  uint32_t settings_hash, uint8_t *out, size_t capacity);
