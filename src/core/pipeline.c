/* SPDX-License-Identifier: MIT */

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <zmk/pointing_tools/core/pipeline.h>

struct zpt_pipeline_operation {
    struct zpt_pipeline *pipeline;
    struct zpt_pipeline_result *result;
    uint32_t now_ms;
    uint32_t remaining_dispatches;
};

struct zpt_stage_context {
    struct zpt_pipeline_operation *operation;
    size_t stage_index;
};

static bool kind_accepted(uint32_t accepted, enum zpt_signal_kind kind) {
    return zpt_signal_kind_valid(kind) && (accepted & ZPT_SIGNAL_KIND_MASK(kind)) != 0U;
}

static bool time_reached(uint32_t now_ms, uint32_t deadline_ms) {
    return (int32_t)(now_ms - deadline_ms) >= 0;
}

static void clear_result(struct zpt_pipeline_result *result) {
    if (result != NULL) {
        *result = (struct zpt_pipeline_result){0};
    }
}

static int dispatch_at(struct zpt_pipeline_operation *operation, size_t stage_index,
                       const struct zpt_signal *signal) {
    if (operation->remaining_dispatches == 0U) {
        return -E2BIG;
    }
    operation->remaining_dispatches--;
    operation->result->dispatches++;

    struct zpt_pipeline *pipeline = operation->pipeline;
    if (stage_index == pipeline->stage_count) {
        if (!kind_accepted(pipeline->sink->api->accepted_kinds, signal->kind)) {
            return -EPROTOTYPE;
        }
        int ret = pipeline->sink->api->emit(pipeline->sink, signal);
        if (ret == 0) {
            operation->result->outputs++;
        }
        return ret;
    }

    struct zpt_stage *stage = pipeline->stages[stage_index];
    if (!kind_accepted(stage->api->accepted_kinds, signal->kind)) {
        return -EPROTOTYPE;
    }

    struct zpt_stage_context context = {
        .operation = operation,
        .stage_index = stage_index,
    };
    return stage->api->process(stage, signal, &context);
}

int zpt_stage_emit(struct zpt_stage_context *context, const struct zpt_signal *signal) {
    if (context == NULL || context->operation == NULL || signal == NULL ||
        context->stage_index >= context->operation->pipeline->stage_count) {
        return -EINVAL;
    }

    const struct zpt_stage *stage = context->operation->pipeline->stages[context->stage_index];
    if (signal->kind != stage->api->output_kind) {
        return -EPROTOTYPE;
    }
    return dispatch_at(context->operation, context->stage_index + 1U, signal);
}

int zpt_stage_schedule_flush(struct zpt_stage_context *context, uint32_t deadline_ms) {
    if (context == NULL || context->operation == NULL ||
        context->stage_index >= context->operation->pipeline->stage_count) {
        return -EINVAL;
    }

    struct zpt_stage *stage = context->operation->pipeline->stages[context->stage_index];
    if ((stage->api->flags & ZPT_STAGE_STATEFUL) == 0U || stage->api->flush == NULL) {
        return -ENOTSUP;
    }
    stage->deadline_ms = deadline_ms;
    stage->deadline_pending = true;
    return 0;
}

void zpt_stage_cancel_flush(struct zpt_stage_context *context) {
    if (context == NULL || context->operation == NULL ||
        context->stage_index >= context->operation->pipeline->stage_count) {
        return;
    }
    context->operation->pipeline->stages[context->stage_index]->deadline_pending = false;
}

uint32_t zpt_stage_now_ms(const struct zpt_stage_context *context) {
    return context != NULL && context->operation != NULL ? context->operation->now_ms : 0U;
}

static int validate_structure(const struct zpt_pipeline *pipeline) {
    if (pipeline == NULL || pipeline->stable_id == NULL || pipeline->stable_id[0] == '\0' ||
        !zpt_signal_kind_valid(pipeline->input_kind) || pipeline->sink == NULL ||
        pipeline->sink->stable_id == NULL || pipeline->sink->api == NULL ||
        pipeline->sink->api->type_id == NULL || pipeline->sink->api->type_id[0] == '\0' ||
        pipeline->sink->api->emit == NULL || pipeline->dispatch_budget == 0U ||
        (pipeline->stage_count > 0U && pipeline->stages == NULL)) {
        return -EINVAL;
    }

    enum zpt_signal_kind current_kind = pipeline->input_kind;
    for (size_t index = 0; index < pipeline->stage_count; index++) {
        const struct zpt_stage *stage = pipeline->stages[index];
        if (stage == NULL || stage->stable_id == NULL || stage->stable_id[0] == '\0' ||
            stage->api == NULL || stage->api->strategy_id == NULL ||
            stage->api->strategy_id[0] == '\0' || stage->api->process == NULL ||
            !zpt_signal_kind_valid(stage->api->output_kind) ||
            !kind_accepted(stage->api->accepted_kinds, current_kind)) {
            return -EINVAL;
        }
        if ((stage->api->flags & ZPT_STAGE_STATEFUL) != 0U && stage->state == NULL) {
            return -EINVAL;
        }
        if (stage->owner != NULL && stage->owner != pipeline) {
            return -EBUSY;
        }
        for (size_t previous = 0; previous < index; previous++) {
            const struct zpt_stage *previous_stage = pipeline->stages[previous];
            if (strcmp(previous_stage->stable_id, stage->stable_id) == 0) {
                return -EEXIST;
            }
            if ((stage->api->flags & ZPT_STAGE_STATEFUL) != 0U &&
                previous_stage->state == stage->state) {
                return -EBUSY;
            }
        }
        current_kind = stage->api->output_kind;
    }

    if (!kind_accepted(pipeline->sink->api->accepted_kinds, current_kind)) {
        return -EPROTOTYPE;
    }
    for (size_t index = 0; index < pipeline->stage_count; index++) {
        if (strcmp(pipeline->stages[index]->stable_id, pipeline->sink->stable_id) == 0) {
            return -EEXIST;
        }
    }

    return 0;
}

int zpt_pipeline_validate(struct zpt_pipeline *pipeline) {
    if (pipeline != NULL && pipeline->validated) {
        return -EALREADY;
    }
    int ret = validate_structure(pipeline);
    if (ret < 0) {
        return ret;
    }

    for (size_t index = 0; index < pipeline->stage_count; index++) {
        pipeline->stages[index]->owner = pipeline;
        pipeline->stages[index]->deadline_pending = false;
    }
    pipeline->validated = true;
    pipeline->active = false;
    zpt_pipeline_reset(pipeline, ZPT_RESET_INITIALIZATION);
    return 0;
}

void zpt_pipeline_reset(struct zpt_pipeline *pipeline, enum zpt_reset_reason reason) {
    if (pipeline == NULL) {
        return;
    }
    for (size_t index = 0; index < pipeline->stage_count; index++) {
        struct zpt_stage *stage = pipeline->stages[index];
        stage->deadline_pending = false;
        if (stage->api != NULL && stage->api->reset != NULL) {
            stage->api->reset(stage, reason);
        }
    }
    if (pipeline->sink != NULL && pipeline->sink->api != NULL &&
        pipeline->sink->api->reset != NULL) {
        pipeline->sink->api->reset(pipeline->sink, reason);
    }
}

int zpt_pipeline_activate(struct zpt_pipeline *pipeline, enum zpt_reset_reason reason) {
    if (pipeline == NULL || !pipeline->validated) {
        return -EINVAL;
    }
    if (pipeline->active) {
        return -EALREADY;
    }

    zpt_pipeline_reset(pipeline, reason);
    for (size_t index = 0; index < pipeline->stage_count; index++) {
        struct zpt_stage *stage = pipeline->stages[index];
        if (stage->api->activate != NULL) {
            int ret = stage->api->activate(stage, reason);
            if (ret < 0) {
                zpt_pipeline_reset(pipeline, reason);
                return ret;
            }
        }
    }
    if (pipeline->sink->api->activate != NULL) {
        int ret = pipeline->sink->api->activate(pipeline->sink, reason);
        if (ret < 0) {
            zpt_pipeline_reset(pipeline, reason);
            return ret;
        }
    }
    pipeline->active = true;
    return 0;
}

static struct zpt_pipeline_operation begin_operation(struct zpt_pipeline *pipeline, uint32_t now_ms,
                                                     struct zpt_pipeline_result *result) {
    *result = (struct zpt_pipeline_result){0};
    return (struct zpt_pipeline_operation){
        .pipeline = pipeline,
        .result = result,
        .now_ms = now_ms,
        .remaining_dispatches = pipeline->dispatch_budget,
    };
}

int zpt_pipeline_push(struct zpt_pipeline *pipeline, const struct zpt_signal *signal,
                      struct zpt_pipeline_result *result) {
    clear_result(result);
    if (pipeline == NULL || signal == NULL || result == NULL || !pipeline->validated) {
        return -EINVAL;
    }
    if (!pipeline->active) {
        return -EACCES;
    }
    if (signal->kind != pipeline->input_kind) {
        return -EPROTOTYPE;
    }

    struct zpt_pipeline_operation operation =
        begin_operation(pipeline, signal->metadata.observed_at_ms, result);
    return dispatch_at(&operation, 0U, signal);
}

int zpt_pipeline_flush(struct zpt_pipeline *pipeline, uint32_t now_ms,
                       struct zpt_pipeline_result *result) {
    clear_result(result);
    if (pipeline == NULL || result == NULL || !pipeline->validated) {
        return -EINVAL;
    }
    if (!pipeline->active) {
        return -EACCES;
    }

    struct zpt_pipeline_operation operation = begin_operation(pipeline, now_ms, result);
    for (size_t index = 0; index < pipeline->stage_count; index++) {
        struct zpt_stage *stage = pipeline->stages[index];
        if (!stage->deadline_pending || !time_reached(now_ms, stage->deadline_ms)) {
            continue;
        }

        stage->deadline_pending = false;
        struct zpt_stage_context context = {
            .operation = &operation,
            .stage_index = index,
        };
        int ret = stage->api->flush(stage, now_ms, &context);
        if (ret < 0) {
            return ret;
        }
    }
    return 0;
}

bool zpt_pipeline_next_deadline(const struct zpt_pipeline *pipeline, uint32_t now_ms,
                                uint32_t *deadline_ms) {
    if (pipeline == NULL || deadline_ms == NULL || !pipeline->validated || !pipeline->active) {
        return false;
    }

    bool found = false;
    uint32_t best_delta = UINT32_MAX;
    uint32_t best_deadline = 0U;
    for (size_t index = 0; index < pipeline->stage_count; index++) {
        const struct zpt_stage *stage = pipeline->stages[index];
        if (!stage->deadline_pending) {
            continue;
        }
        uint32_t delta =
            time_reached(now_ms, stage->deadline_ms) ? 0U : stage->deadline_ms - now_ms;
        if (!found || delta < best_delta) {
            found = true;
            best_delta = delta;
            best_deadline = stage->deadline_ms;
        }
    }
    if (found) {
        *deadline_ms = best_deadline;
    }
    return found;
}

int zpt_pipeline_deactivate(struct zpt_pipeline *pipeline, uint32_t now_ms,
                            enum zpt_reset_reason reason, struct zpt_pipeline_result *result) {
    clear_result(result);
    if (pipeline == NULL || result == NULL || !pipeline->validated) {
        return -EINVAL;
    }
    if (!pipeline->active) {
        return -EALREADY;
    }

    struct zpt_pipeline_operation operation = begin_operation(pipeline, now_ms, result);
    int first_error = 0;
    for (size_t index = 0; index < pipeline->stage_count; index++) {
        struct zpt_stage *stage = pipeline->stages[index];
        stage->deadline_pending = false;
        if (stage->api->deactivate == NULL) {
            continue;
        }
        struct zpt_stage_context context = {
            .operation = &operation,
            .stage_index = index,
        };
        int ret = stage->api->deactivate(stage, now_ms, reason, &context);
        if (ret < 0 && first_error == 0) {
            first_error = ret;
        }
    }
    if (pipeline->sink->api->deactivate != NULL) {
        int ret = pipeline->sink->api->deactivate(pipeline->sink, reason);
        if (ret < 0 && first_error == 0) {
            first_error = ret;
        }
    }

    pipeline->active = false;
    zpt_pipeline_reset(pipeline, reason);
    return first_error;
}
