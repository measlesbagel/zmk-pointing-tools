/* SPDX-License-Identifier: MIT */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zmk/pointing_tools/core/signal.h>

struct zpt_pipeline;
struct zpt_stage;
struct zpt_stage_context;
struct zpt_sink;

enum zpt_reset_reason {
    ZPT_RESET_INITIALIZATION = 0,
    ZPT_RESET_IDLE,
    ZPT_RESET_PIPELINE_ENTERED,
    ZPT_RESET_PIPELINE_LEFT,
    ZPT_RESET_SOURCE_DISCONNECTED,
    ZPT_RESET_SOURCE_RECONNECTED,
    ZPT_RESET_TRANSPORT_DISCONTINUITY,
    ZPT_RESET_EXTERNAL_SUPPRESSION,
    ZPT_RESET_SETTINGS_CHANGED,
    ZPT_RESET_SETTINGS_DEFAULTED,
    ZPT_RESET_ADMINISTRATIVE,
};

enum zpt_stage_flag {
    ZPT_STAGE_STATEFUL = UINT32_C(1) << 0,
};

struct zpt_pipeline_result {
    uint32_t dispatches;
    uint32_t outputs;
};

typedef int (*zpt_stage_process_t)(struct zpt_stage *stage, const struct zpt_signal *signal,
                                   struct zpt_stage_context *context);
typedef int (*zpt_stage_flush_t)(struct zpt_stage *stage, uint32_t now_ms,
                                 struct zpt_stage_context *context);
typedef int (*zpt_stage_activate_t)(struct zpt_stage *stage, enum zpt_reset_reason reason);
typedef int (*zpt_stage_deactivate_t)(struct zpt_stage *stage, uint32_t now_ms,
                                      enum zpt_reset_reason reason,
                                      struct zpt_stage_context *context);
typedef void (*zpt_stage_reset_t)(struct zpt_stage *stage, enum zpt_reset_reason reason);

struct zpt_stage_api {
    const char *strategy_id;
    uint32_t accepted_kinds;
    enum zpt_signal_kind output_kind;
    uint32_t flags;
    zpt_stage_process_t process;
    zpt_stage_flush_t flush;
    zpt_stage_activate_t activate;
    zpt_stage_deactivate_t deactivate;
    zpt_stage_reset_t reset;
};

struct zpt_stage {
    const char *stable_id;
    const struct zpt_stage_api *api;
    const void *config;
    void *state;

    /* Runtime-owned fields. Statically initialize them to zero. */
    struct zpt_pipeline *owner;
    uint32_t deadline_ms;
    bool deadline_pending;
};

typedef int (*zpt_sink_emit_t)(struct zpt_sink *sink, const struct zpt_signal *signal);
typedef int (*zpt_sink_activate_t)(struct zpt_sink *sink, enum zpt_reset_reason reason);
typedef int (*zpt_sink_deactivate_t)(struct zpt_sink *sink, enum zpt_reset_reason reason);
typedef void (*zpt_sink_reset_t)(struct zpt_sink *sink, enum zpt_reset_reason reason);

struct zpt_sink_api {
    const char *type_id;
    uint32_t accepted_kinds;
    zpt_sink_emit_t emit;
    zpt_sink_activate_t activate;
    zpt_sink_deactivate_t deactivate;
    zpt_sink_reset_t reset;
};

struct zpt_sink {
    const char *stable_id;
    const struct zpt_sink_api *api;
    const void *config;
    void *state;
};

struct zpt_pipeline {
    const char *stable_id;
    enum zpt_signal_kind input_kind;
    struct zpt_stage *stages;
    size_t stage_count;
    struct zpt_sink *sink;
    uint32_t dispatch_budget;

    bool validated;
    bool active;
};

int zpt_pipeline_validate(struct zpt_pipeline *pipeline);
int zpt_pipeline_activate(struct zpt_pipeline *pipeline, enum zpt_reset_reason reason);
int zpt_pipeline_deactivate(struct zpt_pipeline *pipeline, uint32_t now_ms,
                            enum zpt_reset_reason reason, struct zpt_pipeline_result *result);
void zpt_pipeline_reset(struct zpt_pipeline *pipeline, enum zpt_reset_reason reason);
int zpt_pipeline_push(struct zpt_pipeline *pipeline, const struct zpt_signal *signal,
                      struct zpt_pipeline_result *result);
int zpt_pipeline_flush(struct zpt_pipeline *pipeline, uint32_t now_ms,
                       struct zpt_pipeline_result *result);
bool zpt_pipeline_next_deadline(const struct zpt_pipeline *pipeline, uint32_t now_ms,
                                uint32_t *deadline_ms);

int zpt_stage_emit(struct zpt_stage_context *context, const struct zpt_signal *signal);
int zpt_stage_schedule_flush(struct zpt_stage_context *context, uint32_t deadline_ms);
void zpt_stage_cancel_flush(struct zpt_stage_context *context);
uint32_t zpt_stage_now_ms(const struct zpt_stage_context *context);
