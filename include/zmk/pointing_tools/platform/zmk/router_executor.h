/* SPDX-License-Identifier: MIT */
#pragma once

#include <stdint.h>

#include <zephyr/kernel.h>

#include <zmk/pointing_tools/core/router.h>

/* Thread-safe Zephyr execution and deadline scheduling for one router. */
struct zpt_zmk_router_executor {
    struct k_mutex lock;
    struct k_work_delayable deadline_work;
    struct zpt_router *router;
};

int zpt_zmk_router_executor_init(struct zpt_zmk_router_executor *executor,
                                 struct zpt_router *router);
int zpt_zmk_router_executor_activate(struct zpt_zmk_router_executor *executor,
                                     enum zpt_reset_reason reason);
int zpt_zmk_router_executor_deactivate(struct zpt_zmk_router_executor *executor, uint32_t now_ms,
                                       enum zpt_reset_reason reason,
                                       struct zpt_pipeline_result *result);
int zpt_zmk_router_executor_select(struct zpt_zmk_router_executor *executor, size_t pipeline_index,
                                   uint32_t now_ms, struct zpt_pipeline_result *result);
int zpt_zmk_router_executor_push(struct zpt_zmk_router_executor *executor,
                                 const struct zpt_signal *signal,
                                 struct zpt_pipeline_result *result);
int zpt_zmk_router_executor_flush(struct zpt_zmk_router_executor *executor, uint32_t now_ms,
                                  struct zpt_pipeline_result *result);
int zpt_zmk_router_executor_reset(struct zpt_zmk_router_executor *executor,
                                  enum zpt_reset_reason reason);
