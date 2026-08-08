/* SPDX-License-Identifier: MIT */
#pragma once

#include <stdint.h>

#include <zephyr/kernel.h>

#include <zmk/pointing_tools/core/pipeline.h>

/* Thread-safe Zephyr execution and deadline scheduling for one pipeline. */
struct zpt_zmk_pipeline_executor {
    struct k_mutex lock;
    struct k_work_delayable deadline_work;
    struct zpt_pipeline *pipeline;
};

int zpt_zmk_pipeline_executor_init(struct zpt_zmk_pipeline_executor *executor,
                                   struct zpt_pipeline *pipeline);
int zpt_zmk_pipeline_executor_activate(struct zpt_zmk_pipeline_executor *executor,
                                       enum zpt_reset_reason reason);
int zpt_zmk_pipeline_executor_deactivate(struct zpt_zmk_pipeline_executor *executor,
                                         uint32_t now_ms, enum zpt_reset_reason reason,
                                         struct zpt_pipeline_result *result);
int zpt_zmk_pipeline_executor_push(struct zpt_zmk_pipeline_executor *executor,
                                   const struct zpt_signal *signal,
                                   struct zpt_pipeline_result *result);
int zpt_zmk_pipeline_executor_flush(struct zpt_zmk_pipeline_executor *executor, uint32_t now_ms,
                                    struct zpt_pipeline_result *result);
int zpt_zmk_pipeline_executor_reset(struct zpt_zmk_pipeline_executor *executor,
                                    enum zpt_reset_reason reason);
