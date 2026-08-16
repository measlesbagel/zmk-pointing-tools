/* SPDX-License-Identifier: MIT */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zmk/pointing_tools/core/pipeline.h>

#define ZPT_ROUTER_NO_PIPELINE SIZE_MAX

struct zpt_router {
    const char *stable_id;
    enum zpt_signal_kind input_kind;
    struct zpt_pipeline *const *pipelines;
    size_t pipeline_count;
    size_t default_pipeline_index;

    /* Runtime-owned fields. Statically initialize them to zero. */
    size_t active_pipeline_index;
    bool validated;
    bool active;
};

int zpt_router_validate(struct zpt_router *router);
int zpt_router_activate(struct zpt_router *router, enum zpt_reset_reason reason);
int zpt_router_deactivate(struct zpt_router *router, uint32_t now_ms, enum zpt_reset_reason reason,
                          struct zpt_pipeline_result *result);
int zpt_router_select(struct zpt_router *router, size_t pipeline_index, uint32_t now_ms,
                      struct zpt_pipeline_result *result);

int zpt_router_push(struct zpt_router *router, const struct zpt_signal *signal,
                    struct zpt_pipeline_result *result);
int zpt_router_flush(struct zpt_router *router, uint32_t now_ms,
                     struct zpt_pipeline_result *result);
bool zpt_router_next_deadline(const struct zpt_router *router, uint32_t now_ms,
                              uint32_t *deadline_ms);

struct zpt_pipeline *zpt_router_active_pipeline(const struct zpt_router *router);
