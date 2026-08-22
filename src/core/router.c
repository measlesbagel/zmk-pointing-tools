/* SPDX-License-Identifier: MIT */

#include <errno.h>
#include <string.h>

#include <zmk/pointing_tools/core/router.h>

static void clear_result(struct zpt_pipeline_result *result) {
    if (result != NULL) {
        *result = (struct zpt_pipeline_result){0};
    }
}

static int validate_router_pipeline(const struct zpt_router *router, size_t index) {
    const struct zpt_pipeline *pipeline = router->pipelines[index];
    if (pipeline == NULL || pipeline->stable_id == NULL || pipeline->stable_id[0] == '\0' ||
        pipeline->input_kind != router->input_kind) {
        return -EINVAL;
    }
    if (pipeline->validated) {
        return -EBUSY;
    }
    for (size_t previous = 0; previous < index; previous++) {
        if (strcmp(router->pipelines[previous]->stable_id, pipeline->stable_id) == 0) {
            return -EEXIST;
        }
    }
    return 0;
}

static int validate_structure(const struct zpt_router *router) {
    if (router == NULL || router->stable_id == NULL || router->stable_id[0] == '\0' ||
        !zpt_signal_kind_valid(router->input_kind) || router->pipelines == NULL ||
        router->pipeline_count == 0U || router->default_pipeline_index >= router->pipeline_count) {
        return -EINVAL;
    }

    for (size_t index = 0; index < router->pipeline_count; index++) {
        int ret = validate_router_pipeline(router, index);
        if (ret < 0) {
            return ret;
        }
    }
    return 0;
}

int zpt_router_validate(struct zpt_router *router) {
    if (router != NULL && router->validated) {
        return -EALREADY;
    }
    int ret = validate_structure(router);
    if (ret < 0) {
        return ret;
    }

    for (size_t index = 0; index < router->pipeline_count; index++) {
        ret = zpt_pipeline_validate(router->pipelines[index]);
        if (ret < 0) {
            return ret;
        }
    }
    router->active_pipeline_index = ZPT_ROUTER_NO_PIPELINE;
    router->validated = true;
    router->active = false;
    return 0;
}

struct zpt_pipeline *zpt_router_active_pipeline(const struct zpt_router *router) {
    if (router == NULL || !router->validated || !router->active ||
        router->active_pipeline_index >= router->pipeline_count) {
        return NULL;
    }
    return router->pipelines[router->active_pipeline_index];
}

int zpt_router_activate(struct zpt_router *router, enum zpt_reset_reason reason) {
    if (router == NULL || !router->validated) {
        return -EINVAL;
    }
    if (router->active) {
        return -EALREADY;
    }

    struct zpt_pipeline *pipeline = router->pipelines[router->default_pipeline_index];
    int ret = zpt_pipeline_activate(pipeline, reason);
    if (ret < 0) {
        return ret;
    }
    router->active_pipeline_index = router->default_pipeline_index;
    router->active = true;
    return 0;
}

int zpt_router_select(struct zpt_router *router, size_t pipeline_index, uint32_t now_ms,
                      struct zpt_pipeline_result *result) {
    clear_result(result);
    if (router == NULL || result == NULL || !router->validated ||
        pipeline_index >= router->pipeline_count) {
        return -EINVAL;
    }
    if (!router->active) {
        return -EACCES;
    }
    if (router->active_pipeline_index == pipeline_index) {
        return 0;
    }

    size_t previous_index = router->active_pipeline_index;
    struct zpt_pipeline *outgoing = zpt_router_active_pipeline(router);
    int first_error = 0;
    if (outgoing != NULL) {
        int ret = zpt_pipeline_deactivate(outgoing, now_ms, ZPT_RESET_PIPELINE_LEFT, result);
        if (ret < 0) {
            first_error = ret;
        }
    }
    router->active_pipeline_index = ZPT_ROUTER_NO_PIPELINE;

    struct zpt_pipeline *incoming = router->pipelines[pipeline_index];
    int ret = zpt_pipeline_activate(incoming, ZPT_RESET_PIPELINE_ENTERED);
    if (ret < 0) {
        /* Roll back to the previous pipeline so input never goes unhandled
         * after an activation failure; stale input is never sent through a
         * pipeline that has already been deactivated. */
        if (outgoing != NULL) {
            int restore_ret = zpt_pipeline_activate(outgoing, ZPT_RESET_PIPELINE_ENTERED);
            if (restore_ret < 0) {
                router->active_pipeline_index = ZPT_ROUTER_NO_PIPELINE;
                return restore_ret;
            }
            router->active_pipeline_index = previous_index;
        }
        return ret;
    }
    router->active_pipeline_index = pipeline_index;
    return first_error;
}

int zpt_router_deactivate(struct zpt_router *router, uint32_t now_ms, enum zpt_reset_reason reason,
                          struct zpt_pipeline_result *result) {
    clear_result(result);
    if (router == NULL || result == NULL || !router->validated) {
        return -EINVAL;
    }
    if (!router->active) {
        return -EALREADY;
    }

    struct zpt_pipeline *pipeline = zpt_router_active_pipeline(router);
    int ret = pipeline == NULL ? 0 : zpt_pipeline_deactivate(pipeline, now_ms, reason, result);
    router->active_pipeline_index = ZPT_ROUTER_NO_PIPELINE;
    router->active = false;
    return ret;
}

int zpt_router_push(struct zpt_router *router, const struct zpt_signal *signal,
                    struct zpt_pipeline_result *result) {
    clear_result(result);
    if (router == NULL || signal == NULL || result == NULL || !router->validated) {
        return -EINVAL;
    }
    if (signal->kind != router->input_kind) {
        return -EPROTOTYPE;
    }
    struct zpt_pipeline *pipeline = zpt_router_active_pipeline(router);
    if (pipeline == NULL) {
        return -EACCES;
    }
    return zpt_pipeline_push(pipeline, signal, result);
}

int zpt_router_flush(struct zpt_router *router, uint32_t now_ms,
                     struct zpt_pipeline_result *result) {
    clear_result(result);
    if (router == NULL || result == NULL || !router->validated) {
        return -EINVAL;
    }
    struct zpt_pipeline *pipeline = zpt_router_active_pipeline(router);
    if (pipeline == NULL) {
        return -EACCES;
    }
    return zpt_pipeline_flush(pipeline, now_ms, result);
}

bool zpt_router_next_deadline(const struct zpt_router *router, uint32_t now_ms,
                              uint32_t *deadline_ms) {
    struct zpt_pipeline *pipeline = zpt_router_active_pipeline(router);
    return pipeline != NULL && zpt_pipeline_next_deadline(pipeline, now_ms, deadline_ms);
}
