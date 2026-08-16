/* SPDX-License-Identifier: MIT */

#include <errno.h>
#include <stddef.h>

#include <zmk/pointing_tools/stage/motion_gate/coherent_displacement.h>

static bool has_pending(const struct zpt_coherent_displacement_state *state) {
    return state->sample_count != 0U;
}

static void clear_pending_evidence(struct zpt_coherent_displacement_stage_state *state) {
    state->pending_evidence = (struct zpt_signal){0};
    state->have_pending_evidence = false;
}

static void accumulate_pending_evidence(struct zpt_coherent_displacement_stage_state *state,
                                        const struct zpt_signal *signal) {
    if (!state->have_pending_evidence) {
        state->pending_evidence = *signal;
        state->have_pending_evidence = true;
        return;
    }

    uint32_t previous_span = state->pending_evidence.metadata.sample_span_us;
    uint32_t next_span = signal->metadata.sample_span_us;
    state->pending_evidence.metadata.sample_span_us =
        UINT32_MAX - previous_span < next_span ? UINT32_MAX : previous_span + next_span;
    if (UINT32_MAX - previous_span < next_span) {
        state->pending_evidence.metadata.flags |= ZPT_SIGNAL_FLAG_SAMPLE_SPAN_CLIPPED;
    }
    state->pending_evidence.metadata.flags |= signal->metadata.flags;
    state->pending_evidence.metadata.observed_at_ms = signal->metadata.observed_at_ms;
    state->pending_evidence.metadata.sequence = signal->metadata.sequence;
    state->pending_evidence.annotations = signal->annotations;
}

static int schedule_next_deadline(struct zpt_stage_context *context,
                                  const struct zpt_coherent_displacement_settings *settings,
                                  const struct zpt_coherent_displacement_state *state) {
    if (!settings->enabled || (!state->active && !has_pending(state))) {
        zpt_stage_cancel_flush(context);
        return 0;
    }

    uint32_t deadline = state->last_frame_ms + settings->idle_timeout_ms;
    if (!state->active && has_pending(state)) {
        uint32_t qualification = state->pending_started_ms + settings->qualification_timeout_ms;
        uint32_t now = zpt_stage_now_ms(context);
        if ((uint32_t)(qualification - now) < (uint32_t)(deadline - now)) {
            deadline = qualification;
        }
    }
    return zpt_stage_schedule_flush(context, deadline);
}

static int coherent_stage_activate(struct zpt_stage *stage, enum zpt_reset_reason reason) {
    (void)reason;
    if (stage == NULL || stage->config == NULL || stage->state == NULL) {
        return -EINVAL;
    }
    const struct zpt_coherent_displacement_stage_config *config = stage->config;
    return zpt_coherent_displacement_validate(&config->settings);
}

static void coherent_stage_reset(struct zpt_stage *stage, enum zpt_reset_reason reason) {
    (void)reason;
    if (stage == NULL || stage->state == NULL) {
        return;
    }
    struct zpt_coherent_displacement_stage_state *state = stage->state;
    zpt_coherent_displacement_reset(&state->strategy);
    clear_pending_evidence(state);
}

static int coherent_stage_process(struct zpt_stage *stage, const struct zpt_signal *signal,
                                  struct zpt_stage_context *context) {
    if (stage == NULL || signal == NULL || context == NULL || stage->config == NULL ||
        stage->state == NULL || (signal->kind != ZPT_SIGNAL_NORMALIZED_MOTION &&
                                 signal->kind != ZPT_SIGNAL_RAW_MOTION)) {
        return -EINVAL;
    }

    const struct zpt_coherent_displacement_stage_config *config = stage->config;
    struct zpt_coherent_displacement_stage_state *state = stage->state;
    bool had_pending = has_pending(&state->strategy);
    bool suppress = config->suppression != NULL && config->suppression->is_suppressed != NULL &&
                    config->suppression->is_suppressed(config->suppression->context, signal,
                                                       zpt_stage_now_ms(context));
    int64_t x = signal->kind == ZPT_SIGNAL_RAW_MOTION ? signal->data.raw_motion.x_counts
                                                      : signal->data.fixed_vector.x;
    int64_t y = signal->kind == ZPT_SIGNAL_RAW_MOTION ? signal->data.raw_motion.y_counts
                                                      : signal->data.fixed_vector.y;
    struct zpt_coherent_displacement_result result =
        zpt_coherent_displacement_update(&state->strategy, &config->settings, x, y,
                                         zpt_stage_now_ms(context), suppress);

    if (result.suppressed || result.phase == ZPT_MOTION_GATE_BYPASS || result.reset_for_idle ||
        result.reset_for_timeout || !had_pending) {
        clear_pending_evidence(state);
    }
    if (result.suppressed) {
        zpt_stage_notify(context, ZPT_STAGE_EVENT_SUPPRESSED, 0);
    } else if (result.discarded && (result.reset_for_idle || result.reset_for_timeout)) {
        zpt_stage_notify(context, ZPT_STAGE_EVENT_DISCARDED, 0);
    }

    if (result.phase == ZPT_MOTION_GATE_PENDING || result.qualified) {
        accumulate_pending_evidence(state, signal);
    }

    int ret = schedule_next_deadline(context, &config->settings, &state->strategy);
    if (ret < 0) {
        return ret;
    }

    if (result.phase == ZPT_MOTION_GATE_BYPASS) {
        return zpt_stage_emit(context, signal);
    }
    if (result.qualified) {
        struct zpt_signal output = state->pending_evidence;
        output.data.fixed_vector.x = result.x;
        output.data.fixed_vector.y = result.y;
        if (result.clipped) {
            output.metadata.flags |= ZPT_SIGNAL_FLAG_CLIPPED;
        }
        if (had_pending) {
            output.metadata.flags |= ZPT_SIGNAL_FLAG_COALESCED;
        }
        clear_pending_evidence(state);
        zpt_stage_notify(context, ZPT_STAGE_EVENT_QUALIFIED, 0);
        return zpt_stage_emit(context, &output);
    }
    if (result.phase == ZPT_MOTION_GATE_ACTIVE) {
        return zpt_stage_emit(context, signal);
    }
    return 0;
}

static int coherent_stage_flush(struct zpt_stage *stage, uint32_t now_ms,
                                struct zpt_stage_context *context) {
    if (stage == NULL || context == NULL || stage->config == NULL || stage->state == NULL) {
        return -EINVAL;
    }
    const struct zpt_coherent_displacement_stage_config *config = stage->config;
    struct zpt_coherent_displacement_stage_state *state = stage->state;
    struct zpt_coherent_displacement_result result =
        zpt_coherent_displacement_expire(&state->strategy, &config->settings, now_ms);
    if (result.reset_for_idle || result.reset_for_timeout) {
        clear_pending_evidence(state);
    }
    return schedule_next_deadline(context, &config->settings, &state->strategy);
}

const struct zpt_stage_api zpt_coherent_displacement_stage_api = {
    .strategy_id = "coherent-displacement",
    .accepted_kinds = ZPT_SIGNAL_KIND_MASK(ZPT_SIGNAL_NORMALIZED_MOTION),
    .output_kind = ZPT_SIGNAL_NORMALIZED_MOTION,
    .flags = ZPT_STAGE_STATEFUL,
    .process = coherent_stage_process,
    .flush = coherent_stage_flush,
    .activate = coherent_stage_activate,
    .reset = coherent_stage_reset,
};

const struct zpt_stage_api zpt_coherent_displacement_raw_stage_api = {
    .strategy_id = "coherent-displacement",
    .accepted_kinds = ZPT_SIGNAL_KIND_MASK(ZPT_SIGNAL_RAW_MOTION),
    .output_kind = ZPT_SIGNAL_RAW_MOTION,
    .flags = ZPT_STAGE_STATEFUL,
    .process = coherent_stage_process,
    .flush = coherent_stage_flush,
    .activate = coherent_stage_activate,
    .reset = coherent_stage_reset,
};
