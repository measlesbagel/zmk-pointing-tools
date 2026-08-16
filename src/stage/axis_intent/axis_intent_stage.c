/* SPDX-License-Identifier: MIT */

#include <errno.h>
#include <limits.h>
#include <stddef.h>

#include <zmk/pointing_tools/stage/axis_intent.h>

static uint64_t magnitude(int64_t value) {
    if (value >= 0) {
        return (uint64_t)value;
    }
    return (uint64_t)(-(value + 1)) + 1U;
}

static uint64_t saturating_add_u64(uint64_t left, uint64_t right) {
    return UINT64_MAX - left < right ? UINT64_MAX : left + right;
}

/* Manhattan magnitude of the frame per second, in the input units (Q16
 * millimetres for normalized motion, counts for raw motion). */
static zpt_fixed_t axis_speed_per_second(int64_t x, int64_t y, uint32_t elapsed_ms) {
    if (elapsed_ms == 0U) {
        return 0;
    }
    uint64_t magnitude_total = saturating_add_u64(magnitude(x), magnitude(y));
    if (magnitude_total > (uint64_t)INT64_MAX / 1000U) {
        return INT64_MAX;
    }
    return (zpt_fixed_t)((magnitude_total * 1000U) / elapsed_ms);
}

static int axis_intent_stage_activate(struct zpt_stage *stage, enum zpt_reset_reason reason) {
    (void)reason;
    if (stage == NULL || stage->config == NULL || stage->state == NULL) {
        return -EINVAL;
    }
    const struct zpt_axis_intent_stage_config *config = stage->config;
    return zpt_axis_intent_validate(&config->settings);
}

static void axis_intent_stage_reset(struct zpt_stage *stage, enum zpt_reset_reason reason) {
    (void)reason;
    if (stage == NULL || stage->state == NULL) {
        return;
    }
    struct zpt_axis_intent_stage_state *state = stage->state;
    zpt_axis_intent_reset(&state->estimator);
    state->have_last_frame = false;
    /* Sentinel: the first processed frame always reports its intent. */
    state->last_notified_intent = UINT8_MAX;
}

static bool suppression_active(const struct zpt_axis_intent_stage_config *config,
                               const struct zpt_signal *signal, uint32_t now) {
    return config->suppression != NULL && config->suppression->is_suppressed != NULL &&
           config->suppression->is_suppressed(config->suppression->context, signal, now);
}

static int axis_intent_stage_process(struct zpt_stage *stage, const struct zpt_signal *signal,
                                     struct zpt_stage_context *context) {
    if (stage == NULL || signal == NULL || context == NULL || stage->config == NULL ||
        stage->state == NULL) {
        return -EINVAL;
    }

    const struct zpt_axis_intent_stage_config *config = stage->config;
    struct zpt_axis_intent_stage_state *state = stage->state;
    uint32_t now = zpt_stage_now_ms(context);
    uint32_t elapsed = state->have_last_frame ? now - state->last_frame_ms : 0U;

    bool suppressed = suppression_active(config, signal, now);
    if (suppressed || !state->have_last_frame ||
        (config->idle_timeout_ms != 0U && elapsed >= config->idle_timeout_ms)) {
        zpt_axis_intent_reset(&state->estimator);
        if (!suppressed && state->last_notified_intent != ZPT_AXIS_INTENT_UNDECIDED) {
            zpt_stage_notify(context, ZPT_STAGE_EVENT_INTENT_CHANGED, ZPT_AXIS_INTENT_UNDECIDED);
        }
        state->last_notified_intent = ZPT_AXIS_INTENT_UNDECIDED;
    }
    state->last_frame_ms = now;
    state->have_last_frame = true;
    if (suppressed) {
        /* Emit a zero-valued frame so downstream stages observe the same
         * suppression and clear their buffered state without ever seeing
         * suppressed motion values. */
        zpt_stage_notify(context, ZPT_STAGE_EVENT_SUPPRESSED, 0);
        struct zpt_signal output = *signal;
        output.data.fixed_vector.x = 0;
        output.data.fixed_vector.y = 0;
        return zpt_stage_emit(context, &output);
    }

    int64_t x = signal->kind == ZPT_SIGNAL_RAW_MOTION ? signal->data.raw_motion.x_counts
                                                      : signal->data.fixed_vector.x;
    int64_t y = signal->kind == ZPT_SIGNAL_RAW_MOTION ? signal->data.raw_motion.y_counts
                                                      : signal->data.fixed_vector.y;
    enum zpt_axis_intent intent = zpt_axis_intent_estimate(&state->estimator, &config->settings,
                                                           config->policy, x, y, elapsed);

    struct zpt_signal output = *signal;
    output.annotations.axis_intent = (uint8_t)intent;
    output.annotations.axis_confidence_percent = config->policy == ZPT_AXIS_POLICY_ADAPTIVE
                                                     ? zpt_axis_intent_confidence(&state->estimator)
                                                     : 100U;
    output.annotations.speed_per_second = axis_speed_per_second(x, y, elapsed);
    if (state->last_notified_intent != (uint8_t)intent) {
        zpt_stage_notify(context, ZPT_STAGE_EVENT_INTENT_CHANGED, (int64_t)intent);
        state->last_notified_intent = (uint8_t)intent;
    }
    return zpt_stage_emit(context, &output);
}

const struct zpt_stage_api zpt_axis_intent_stage_api = {
    .strategy_id = "axis-intent",
    .accepted_kinds = ZPT_SIGNAL_KIND_MASK(ZPT_SIGNAL_NORMALIZED_MOTION),
    .output_kind = ZPT_SIGNAL_NORMALIZED_MOTION,
    .flags = ZPT_STAGE_STATEFUL,
    .process = axis_intent_stage_process,
    .activate = axis_intent_stage_activate,
    .reset = axis_intent_stage_reset,
};

const struct zpt_stage_api zpt_axis_intent_raw_stage_api = {
    .strategy_id = "axis-intent",
    .accepted_kinds = ZPT_SIGNAL_KIND_MASK(ZPT_SIGNAL_RAW_MOTION),
    .output_kind = ZPT_SIGNAL_RAW_MOTION,
    .flags = ZPT_STAGE_STATEFUL,
    .process = axis_intent_stage_process,
    .activate = axis_intent_stage_activate,
    .reset = axis_intent_stage_reset,
};
