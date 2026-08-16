/* SPDX-License-Identifier: MIT */

#include <errno.h>
#include <limits.h>
#include <stddef.h>

#include <zmk/pointing_tools/core/fixed.h>
#include <zmk/pointing_tools/stage/axis_constraint.h>
#include <zmk/pointing_tools/stage/axis_intent.h>

static bool suppression_active(const struct zpt_axis_constraint_config *config,
                               const struct zpt_signal *signal, uint32_t now) {
    return config->suppression != NULL && config->suppression->is_suppressed != NULL &&
           config->suppression->is_suppressed(config->suppression->context, signal, now);
}

static void clear_undecided(struct zpt_axis_constraint_state *state) {
    state->undecided_x = 0;
    state->undecided_y = 0;
    state->have_undecided = false;
}

static int axis_constraint_stage_activate(struct zpt_stage *stage, enum zpt_reset_reason reason) {
    (void)reason;
    return stage == NULL || stage->config == NULL || stage->state == NULL ? -EINVAL : 0;
}

static void axis_constraint_stage_reset(struct zpt_stage *stage, enum zpt_reset_reason reason) {
    (void)reason;
    if (stage == NULL || stage->state == NULL) {
        return;
    }
    struct zpt_axis_constraint_state *state = stage->state;
    clear_undecided(state);
    state->previous_intent = ZPT_AXIS_INTENT_UNDECIDED;
    state->have_last_frame = false;
}

static void schedule_undecided_expiry(struct zpt_axis_constraint_state *state,
                                      const struct zpt_axis_constraint_config *config,
                                      struct zpt_stage_context *context, uint32_t now) {
    uint32_t interval =
        config->fold_interval_ms != 0U ? config->fold_interval_ms : config->idle_timeout_ms;
    if (interval == 0U) {
        return;
    }
    if (state->have_undecided) {
        zpt_stage_schedule_flush(context, now + interval);
    } else {
        zpt_stage_cancel_flush(context);
    }
}

static int axis_constraint_stage_process(struct zpt_stage *stage, const struct zpt_signal *signal,
                                         struct zpt_stage_context *context) {
    if (stage == NULL || signal == NULL || context == NULL || stage->config == NULL ||
        stage->state == NULL || signal->kind != ZPT_SIGNAL_NORMALIZED_MOTION) {
        return -EINVAL;
    }

    const struct zpt_axis_constraint_config *config = stage->config;
    struct zpt_axis_constraint_state *state = stage->state;
    uint32_t now = zpt_stage_now_ms(context);
    uint32_t elapsed = state->have_last_frame ? now - state->last_frame_ms : 0U;

    bool suppressed = suppression_active(config, signal, now);
    if (suppressed) {
        clear_undecided(state);
        zpt_stage_notify(context, ZPT_STAGE_EVENT_SUPPRESSED, 0);
        /* Emit a zero-valued frame so downstream stages observe the same
         * suppression and clear their buffered state without ever seeing
         * suppressed motion values. */
        struct zpt_signal output = *signal;
        output.data.fixed_vector.x = 0;
        output.data.fixed_vector.y = 0;
        return zpt_stage_emit(context, &output);
    }
    if (!state->have_last_frame ||
        (config->idle_timeout_ms != 0U && elapsed >= config->idle_timeout_ms)) {
        clear_undecided(state);
        state->previous_intent = ZPT_AXIS_INTENT_UNDECIDED;
    }
    state->last_frame_ms = now;
    state->have_last_frame = true;

    int64_t x = signal->data.fixed_vector.x;
    int64_t y = signal->data.fixed_vector.y;
    uint8_t intent = signal->annotations.axis_intent;

    if (intent == ZPT_AXIS_INTENT_UNDECIDED) {
        state->undecided_x = zpt_fixed_saturating_add(state->undecided_x, x);
        state->undecided_y = zpt_fixed_saturating_add(state->undecided_y, y);
        state->have_undecided = true;
        schedule_undecided_expiry(state, config, context, now);
        /* Emit a zeroed frame so downstream stages observe the frame and arm
         * their report deadlines. */
        struct zpt_signal output = *signal;
        output.data.fixed_vector.x = 0;
        output.data.fixed_vector.y = 0;
        return zpt_stage_emit(context, &output);
    }

    int64_t output_x = 0;
    int64_t output_y = 0;
    if (state->previous_intent == ZPT_AXIS_INTENT_UNDECIDED && state->have_undecided) {
        /* Fold buffered unclassified motion, filtered by the new intent. */
        if (intent != ZPT_AXIS_INTENT_VERTICAL) {
            output_x = zpt_fixed_saturating_add(output_x, state->undecided_x);
        }
        if (intent != ZPT_AXIS_INTENT_HORIZONTAL) {
            output_y = zpt_fixed_saturating_add(output_y, state->undecided_y);
        }
        clear_undecided(state);
        schedule_undecided_expiry(state, config, context, now);
    }
    if (intent != ZPT_AXIS_INTENT_VERTICAL) {
        output_x = zpt_fixed_saturating_add(output_x, x);
    }
    if (intent != ZPT_AXIS_INTENT_HORIZONTAL) {
        output_y = zpt_fixed_saturating_add(output_y, y);
    }
    state->previous_intent = intent;

    struct zpt_signal output = *signal;
    output.data.fixed_vector.x = output_x;
    output.data.fixed_vector.y = output_y;
    return zpt_stage_emit(context, &output);
}

static int axis_constraint_stage_flush(struct zpt_stage *stage, uint32_t now_ms,
                                       struct zpt_stage_context *context) {
    (void)now_ms;
    if (stage == NULL || context == NULL || stage->config == NULL || stage->state == NULL) {
        return -EINVAL;
    }
    const struct zpt_axis_constraint_config *config = stage->config;
    struct zpt_axis_constraint_state *state = stage->state;
    if (!state->have_undecided) {
        return 0;
    }
    if (!config->discard_unclassified) {
        struct zpt_signal output = {0};
        output.kind = ZPT_SIGNAL_NORMALIZED_MOTION;
        output.metadata.observed_at_ms = stage->deadline_ms;
        output.data.fixed_vector.x = state->undecided_x;
        output.data.fixed_vector.y = state->undecided_y;
        int64_t folded_x = state->undecided_x;
        int64_t folded_y = state->undecided_y;
        clear_undecided(state);
        zpt_stage_notify(context, ZPT_STAGE_EVENT_FLUSHED,
                         (folded_x > 0 ? folded_x : -folded_x) +
                             (folded_y > 0 ? folded_y : -folded_y));
        return zpt_stage_emit(context, &output);
    }
    clear_undecided(state);
    zpt_stage_notify(context, ZPT_STAGE_EVENT_DISCARDED, 0);
    return 0;
}

const struct zpt_stage_api zpt_axis_constraint_stage_api = {
    .strategy_id = "axis-constraint",
    .accepted_kinds = ZPT_SIGNAL_KIND_MASK(ZPT_SIGNAL_NORMALIZED_MOTION),
    .output_kind = ZPT_SIGNAL_NORMALIZED_MOTION,
    .flags = ZPT_STAGE_STATEFUL,
    .process = axis_constraint_stage_process,
    .flush = axis_constraint_stage_flush,
    .activate = axis_constraint_stage_activate,
    .reset = axis_constraint_stage_reset,
};
