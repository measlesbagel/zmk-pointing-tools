/* SPDX-License-Identifier: MIT */

#include <errno.h>
#include <limits.h>
#include <stddef.h>

#include <zmk/pointing_tools/stage/cursor_quantizer.h>

/* Q16 by Q16 multiply with saturation: (left * right) >> 16. */
static int64_t fixed_multiply(int64_t left, int64_t right) {
    if (left == 0 || right == 0) {
        return 0;
    }
    if (left > 0 && right > 0) {
        if (left > INT64_MAX / right) {
            return INT64_MAX;
        }
        return (left * right) >> ZPT_FIXED_FRACTION_BITS;
    }
    if (left < 0 && right < 0) {
        if (left < INT64_MAX / right) {
            return INT64_MAX;
        }
        return (left * right) >> ZPT_FIXED_FRACTION_BITS;
    }
    if (left > 0) {
        if (right < INT64_MIN / left) {
            return INT64_MIN;
        }
    } else if (right > INT64_MAX / -left) {
        return INT64_MIN;
    }
    return (left * right) >> ZPT_FIXED_FRACTION_BITS;
}

static int64_t saturating_add_i64(int64_t left, int64_t right) {
    if (right > 0 && left > INT64_MAX - right) {
        return INT64_MAX;
    }
    if (right < 0 && left < INT64_MIN - right) {
        return INT64_MIN;
    }
    return left + right;
}

static int cursor_quantizer_stage_activate(struct zpt_stage *stage, enum zpt_reset_reason reason) {
    (void)reason;
    if (stage == NULL || stage->config == NULL || stage->state == NULL) {
        return -EINVAL;
    }
    const struct zpt_cursor_quantizer_config *config = stage->config;
    return config->units_per_meter <= 0 ? -EINVAL : 0;
}

static void cursor_quantizer_stage_reset(struct zpt_stage *stage, enum zpt_reset_reason reason) {
    (void)reason;
    if (stage == NULL || stage->state == NULL) {
        return;
    }
    struct zpt_cursor_quantizer_state *state = stage->state;
    state->remainder_x = 0;
    state->remainder_y = 0;
}

static int cursor_quantizer_stage_process(struct zpt_stage *stage, const struct zpt_signal *signal,
                                          struct zpt_stage_context *context) {
    if (stage == NULL || signal == NULL || context == NULL || stage->config == NULL ||
        stage->state == NULL || signal->kind != ZPT_SIGNAL_NORMALIZED_MOTION) {
        return -EINVAL;
    }

    const struct zpt_cursor_quantizer_config *config = stage->config;
    struct zpt_cursor_quantizer_state *state = stage->state;

    int64_t units_x = saturating_add_i64(
        fixed_multiply(signal->data.fixed_vector.x, config->units_per_meter), state->remainder_x);
    int64_t units_y = saturating_add_i64(
        fixed_multiply(signal->data.fixed_vector.y, config->units_per_meter), state->remainder_y);
    int64_t integer_x = units_x / ZPT_FIXED_ONE;
    int64_t integer_y = units_y / ZPT_FIXED_ONE;
    state->remainder_x = units_x - integer_x * ZPT_FIXED_ONE;
    state->remainder_y = units_y - integer_y * ZPT_FIXED_ONE;

    struct zpt_signal output = *signal;
    output.kind = ZPT_SIGNAL_POINTER_DELTA;
    output.data.fixed_vector.x = integer_x * ZPT_FIXED_ONE;
    output.data.fixed_vector.y = integer_y * ZPT_FIXED_ONE;
    return zpt_stage_emit(context, &output);
}

const struct zpt_stage_api zpt_cursor_quantizer_stage_api = {
    .strategy_id = "cursor-quantizer",
    .accepted_kinds = ZPT_SIGNAL_KIND_MASK(ZPT_SIGNAL_NORMALIZED_MOTION),
    .output_kind = ZPT_SIGNAL_POINTER_DELTA,
    .flags = ZPT_STAGE_STATEFUL,
    .process = cursor_quantizer_stage_process,
    .activate = cursor_quantizer_stage_activate,
    .reset = cursor_quantizer_stage_reset,
};
