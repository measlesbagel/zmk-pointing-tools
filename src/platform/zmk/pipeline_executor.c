/* SPDX-License-Identifier: MIT */

#include <errno.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <zmk/pointing_tools/platform/zmk/pipeline_executor.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

static bool time_reached(uint32_t now_ms, uint32_t deadline_ms) {
    return (int32_t)(now_ms - deadline_ms) >= 0;
}

static int update_deadline_locked(struct zpt_zmk_pipeline_executor *executor, uint32_t now_ms) {
    uint32_t deadline_ms;
    if (!zpt_pipeline_next_deadline(executor->pipeline, now_ms, &deadline_ms)) {
        if (k_work_delayable_is_pending(&executor->deadline_work)) {
            int ret = k_work_cancel_delayable(&executor->deadline_work);
            if (ret < 0) {
                return ret;
            }
        }
        return 0;
    }

    uint32_t delay_ms = time_reached(now_ms, deadline_ms) ? 0U : deadline_ms - now_ms;
    int ret = k_work_reschedule(&executor->deadline_work, K_MSEC(delay_ms));
    return ret < 0 ? ret : 0;
}

static int lock_executor(struct zpt_zmk_pipeline_executor *executor) {
    if (executor == NULL || executor->pipeline == NULL) {
        return -EINVAL;
    }
    return k_mutex_lock(&executor->lock, K_FOREVER);
}

static int unlock_with_result(struct zpt_zmk_pipeline_executor *executor, int operation_result,
                              int deadline_result) {
    int unlock_result = k_mutex_unlock(&executor->lock);
    if (operation_result < 0) {
        return operation_result;
    }
    if (deadline_result < 0) {
        return deadline_result;
    }
    return unlock_result;
}

int zpt_zmk_pipeline_executor_activate(struct zpt_zmk_pipeline_executor *executor,
                                       enum zpt_reset_reason reason) {
    int ret = lock_executor(executor);
    if (ret < 0) {
        return ret;
    }
    int operation_result = zpt_pipeline_activate(executor->pipeline, reason);
    int deadline_result =
        operation_result < 0 ? 0 : update_deadline_locked(executor, k_uptime_get_32());
    return unlock_with_result(executor, operation_result, deadline_result);
}

int zpt_zmk_pipeline_executor_deactivate(struct zpt_zmk_pipeline_executor *executor,
                                         uint32_t now_ms, enum zpt_reset_reason reason,
                                         struct zpt_pipeline_result *result) {
    int ret = lock_executor(executor);
    if (ret < 0) {
        return ret;
    }
    int operation_result = zpt_pipeline_deactivate(executor->pipeline, now_ms, reason, result);
    int deadline_result = update_deadline_locked(executor, now_ms);
    return unlock_with_result(executor, operation_result, deadline_result);
}

int zpt_zmk_pipeline_executor_push(struct zpt_zmk_pipeline_executor *executor,
                                   const struct zpt_signal *signal,
                                   struct zpt_pipeline_result *result) {
    int ret = lock_executor(executor);
    if (ret < 0) {
        return ret;
    }
    int operation_result = zpt_pipeline_push(executor->pipeline, signal, result);
    int deadline_result = update_deadline_locked(executor, k_uptime_get_32());
    return unlock_with_result(executor, operation_result, deadline_result);
}

int zpt_zmk_pipeline_executor_flush(struct zpt_zmk_pipeline_executor *executor, uint32_t now_ms,
                                    struct zpt_pipeline_result *result) {
    int ret = lock_executor(executor);
    if (ret < 0) {
        return ret;
    }
    int operation_result = zpt_pipeline_flush(executor->pipeline, now_ms, result);
    int deadline_result = update_deadline_locked(executor, now_ms);
    return unlock_with_result(executor, operation_result, deadline_result);
}

int zpt_zmk_pipeline_executor_reset(struct zpt_zmk_pipeline_executor *executor,
                                    enum zpt_reset_reason reason) {
    int ret = lock_executor(executor);
    if (ret < 0) {
        return ret;
    }
    zpt_pipeline_reset(executor->pipeline, reason);
    int deadline_result = update_deadline_locked(executor, k_uptime_get_32());
    return unlock_with_result(executor, 0, deadline_result);
}

static void deadline_work_handler(struct k_work *work) {
    struct k_work_delayable *deadline_work = k_work_delayable_from_work(work);
    struct zpt_zmk_pipeline_executor *executor =
        CONTAINER_OF(deadline_work, struct zpt_zmk_pipeline_executor, deadline_work);

    struct zpt_pipeline_result result;
    int ret = zpt_zmk_pipeline_executor_flush(executor, k_uptime_get_32(), &result);
    if (ret < 0 && ret != -EACCES) {
        LOG_ERR("Motion pipeline %s deadline flush failed: %d", executor->pipeline->stable_id, ret);
    }
}

int zpt_zmk_pipeline_executor_init(struct zpt_zmk_pipeline_executor *executor,
                                   struct zpt_pipeline *pipeline) {
    if (executor == NULL || pipeline == NULL) {
        return -EINVAL;
    }

    *executor = (struct zpt_zmk_pipeline_executor){0};
    executor->pipeline = pipeline;
    k_mutex_init(&executor->lock);
    k_work_init_delayable(&executor->deadline_work, deadline_work_handler);
    return zpt_pipeline_validate(pipeline);
}
