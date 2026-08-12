/* SPDX-License-Identifier: MIT */

#include <errno.h>
#include <limits.h>
#include <stddef.h>

#include <zmk/pointing_tools/stage/cursor_transfer.h>

static int64_t saturating_scale(int64_t value, uint16_t multiplier, uint16_t divisor,
                                bool *clipped) {
    if (multiplier == 0U) {
        return 0;
    }
    if (value > INT64_MAX / multiplier) {
        *clipped = true;
        return INT64_MAX;
    }
    if (value < INT64_MIN / multiplier) {
        *clipped = true;
        return INT64_MIN;
    }
    return value * multiplier / divisor;
}

static int cursor_transfer_stage_activate(struct zpt_stage *stage, enum zpt_reset_reason reason) {
    (void)reason;
    if (stage == NULL || stage->config == NULL) {
        return -EINVAL;
    }
    const struct zpt_cursor_transfer_config *config = stage->config;
    return config->scale_divisor == 0U ? -EINVAL : 0;
}

static int cursor_transfer_stage_process(struct zpt_stage *stage, const struct zpt_signal *signal,
                                         struct zpt_stage_context *context) {
    if (stage == NULL || signal == NULL || context == NULL || stage->config == NULL ||
        signal->kind != ZPT_SIGNAL_NORMALIZED_MOTION) {
        return -EINVAL;
    }

    const struct zpt_cursor_transfer_config *config = stage->config;
    bool clipped = false;
    struct zpt_signal output = *signal;
    output.data.fixed_vector.x = saturating_scale(
        signal->data.fixed_vector.x, config->scale_multiplier, config->scale_divisor, &clipped);
    output.data.fixed_vector.y = saturating_scale(
        signal->data.fixed_vector.y, config->scale_multiplier, config->scale_divisor, &clipped);
    if (clipped) {
        output.metadata.flags |= ZPT_SIGNAL_FLAG_CLIPPED;
    }
    return zpt_stage_emit(context, &output);
}

const struct zpt_stage_api zpt_cursor_transfer_stage_api = {
    .strategy_id = "cursor-transfer",
    .accepted_kinds = ZPT_SIGNAL_KIND_MASK(ZPT_SIGNAL_NORMALIZED_MOTION),
    .output_kind = ZPT_SIGNAL_NORMALIZED_MOTION,
    .process = cursor_transfer_stage_process,
    .activate = cursor_transfer_stage_activate,
};
