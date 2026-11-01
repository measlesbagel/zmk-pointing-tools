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

#define ZPT_CAPTURE_SCHEMA_VERSION 1
/* A batch starts with the first record's sequence (u32) and a count (u8). */
#define ZPT_CAPTURE_BATCH_HEADER_SIZE 5
/* Two notifications at the 65-byte ATT MTU ZMK negotiates, less the frame
 * header, so a batch fits the default Bluetooth credit budget. */
#define ZPT_CAPTURE_BATCH_MAX_PAYLOAD 119
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

/* Encode one record without its sequence; out must hold
 * ZPT_CAPTURE_RECORD_MAX_SIZE bytes. Returns the encoded length. */
size_t zpt_capture_record_encode(const struct zpt_capture_record *record, uint8_t *out);

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

/* Records packed into one capture batch message. */
struct zpt_capture_batch {
    uint8_t payload[ZPT_CAPTURE_BATCH_MAX_PAYLOAD];
    uint16_t length;
    uint8_t count;
    uint32_t next_sequence;
};

void zpt_capture_batch_clear(struct zpt_capture_batch *batch);

/* Append a record. Returns false, leaving the batch unchanged, when the
 * record doesn't fit or doesn't continue the batch's sequence; send the
 * batch, clear it, and add the record again. An empty batch takes any
 * record. */
bool zpt_capture_batch_add(struct zpt_capture_batch *batch,
                           const struct zpt_capture_record *record);

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
