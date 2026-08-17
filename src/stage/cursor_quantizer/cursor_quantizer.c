/* SPDX-License-Identifier: MIT */

#include <errno.h>
#include <limits.h>
#include <stddef.h>

#include <zmk/pointing_tools/core/fixed.h>
#include <zmk/pointing_tools/stage/cursor_quantizer.h>

static int cursor_quantizer_stage_activate(struct zpt_stage *stage, enum zpt_reset_reason reason) {
    (void)reason;
    /* No config: the output factor is derived from each frame's CPI. */
    return (stage == NULL || stage->state == NULL) ? -EINVAL : 0;
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
    if (stage == NULL || signal == NULL || context == NULL || stage->state == NULL ||
        signal->kind != ZPT_SIGNAL_NORMALIZED_MOTION || signal->metadata.resolution_cpi == 0) {
        return -EINVAL;
    }

    struct zpt_cursor_quantizer_state *state = stage->state;
    zpt_fixed_t units_per_millimeter =
        ZPT_CPI_TO_FIXED_PER_MILLIMETER(signal->metadata.resolution_cpi);

    int64_t units_x = zpt_fixed_saturating_add(
        zpt_fixed_multiply(signal->data.fixed_vector.x, units_per_millimeter),
        state->remainder_x);
    int64_t units_y = zpt_fixed_saturating_add(
        zpt_fixed_multiply(signal->data.fixed_vector.y, units_per_millimeter),
        state->remainder_y);
    /* Defer only the fractional part of the truncated value; whole units
     * beyond the signed 16-bit HID movement range are dropped by the clamp,
     * so a clamped frame cannot poison the next frame's remainder. */
    int32_t integer_x = zpt_fixed_to_int32(units_x);
    int32_t integer_y = zpt_fixed_to_int32(units_y);
    state->remainder_x = units_x - (int64_t)integer_x * ZPT_FIXED_ONE;
    state->remainder_y = units_y - (int64_t)integer_y * ZPT_FIXED_ONE;
    integer_x = integer_x > INT16_MAX ? INT16_MAX : (integer_x < INT16_MIN ? INT16_MIN : integer_x);
    integer_y = integer_y > INT16_MAX ? INT16_MAX : (integer_y < INT16_MIN ? INT16_MIN : integer_y);

    struct zpt_signal output = *signal;
    output.kind = ZPT_SIGNAL_POINTER_DELTA;
    output.data.delta.x = integer_x;
    output.data.delta.y = integer_y;
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
