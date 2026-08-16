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

/* Manhattan magnitude of the frame expressed as Q16 millimetres per second. */
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
}

static int axis_intent_stage_process(struct zpt_stage *stage, const struct zpt_signal *signal,
                                     struct zpt_stage_context *context) {
    if (stage == NULL || signal == NULL || context == NULL || stage->config == NULL ||
        stage->state == NULL || signal->kind != ZPT_SIGNAL_NORMALIZED_MOTION) {
        return -EINVAL;
    }

    const struct zpt_axis_intent_stage_config *config = stage->config;
    struct zpt_axis_intent_stage_state *state = stage->state;
    uint32_t now = zpt_stage_now_ms(context);
    uint32_t elapsed = state->have_last_frame ? now - state->last_frame_ms : 0U;
    state->last_frame_ms = now;
    state->have_last_frame = true;

    enum zpt_axis_intent intent =
        zpt_axis_intent_estimate(&state->estimator, &config->settings, config->policy,
                                 signal->data.fixed_vector.x, signal->data.fixed_vector.y, elapsed);

    struct zpt_signal output = *signal;
    output.annotations.axis_intent = (uint8_t)intent;
    output.annotations.axis_confidence_percent = config->policy == ZPT_AXIS_POLICY_ADAPTIVE
                                                     ? zpt_axis_intent_confidence(&state->estimator)
                                                     : 100U;
    output.annotations.speed_per_second =
        axis_speed_per_second(signal->data.fixed_vector.x, signal->data.fixed_vector.y, elapsed);
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
