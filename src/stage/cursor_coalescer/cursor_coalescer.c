/* SPDX-License-Identifier: MIT */

#include <errno.h>
#include <limits.h>
#include <stddef.h>

#include <zmk/pointing_tools/core/fixed.h>
#include <zmk/pointing_tools/stage/cursor_coalescer.h>

static int32_t take_report_axis(int64_t *pending) {
    const int64_t value = *pending > INT16_MAX   ? INT16_MAX
                          : *pending < INT16_MIN ? INT16_MIN
                                                 : *pending;
    *pending -= value;
    return (int32_t)value;
}

static bool interval_elapsed(const struct zpt_cursor_coalescer_config *config,
                             const struct zpt_cursor_coalescer_state *state, uint32_t now_ms) {
    return !state->have_report || now_ms - state->last_report_ms >= config->report_interval_ms;
}

static int arm(struct zpt_cursor_coalescer_state *state, struct zpt_stage_context *context,
               uint32_t deadline_ms) {
    if (state->armed) {
        return 0;
    }
    int ret = zpt_stage_schedule_flush(context, deadline_ms);
    if (ret == 0) {
        state->armed = true;
    }
    return ret;
}

/* Send what the pending sum allows in one report, and arm a flush for any
 * remainder beyond the HID range. */
static int send_report(const struct zpt_cursor_coalescer_config *config,
                       struct zpt_cursor_coalescer_state *state, struct zpt_stage_context *context,
                       uint32_t now_ms) {
    struct zpt_signal output = {
        .kind = ZPT_SIGNAL_POINTER_DELTA,
        .metadata = state->metadata,
    };
    output.data.delta.x = take_report_axis(&state->pending_x);
    output.data.delta.y = take_report_axis(&state->pending_y);
    output.metadata.observed_at_ms = now_ms;
    if (state->pending_frames > 1U) {
        output.metadata.flags |= ZPT_SIGNAL_FLAG_COALESCED;
    }
    state->pending_frames = 0;
    state->last_report_ms = now_ms;
    state->have_report = true;

    if (state->pending_x != 0 || state->pending_y != 0) {
        int ret = arm(state, context, now_ms + config->report_interval_ms);
        if (ret < 0) {
            return ret;
        }
    }
    return zpt_stage_emit(context, &output);
}

static int cursor_coalescer_stage_activate(struct zpt_stage *stage, enum zpt_reset_reason reason) {
    (void)reason;
    return stage == NULL || stage->config == NULL || stage->state == NULL ? -EINVAL : 0;
}

static void cursor_coalescer_stage_reset(struct zpt_stage *stage, enum zpt_reset_reason reason) {
    (void)reason;
    if (stage == NULL || stage->state == NULL) {
        return;
    }
    struct zpt_cursor_coalescer_state *state = stage->state;
    *state = (struct zpt_cursor_coalescer_state){0};
}

static int cursor_coalescer_stage_process(struct zpt_stage *stage, const struct zpt_signal *signal,
                                          struct zpt_stage_context *context) {
    if (stage == NULL || signal == NULL || context == NULL || stage->config == NULL ||
        stage->state == NULL || signal->kind != ZPT_SIGNAL_POINTER_DELTA) {
        return -EINVAL;
    }

    const struct zpt_cursor_coalescer_config *config = stage->config;
    struct zpt_cursor_coalescer_state *state = stage->state;
    const uint32_t now = zpt_stage_now_ms(context);

    if (signal->data.delta.x != 0 || signal->data.delta.y != 0) {
        state->pending_x = zpt_fixed_saturating_add(state->pending_x, signal->data.delta.x);
        state->pending_y = zpt_fixed_saturating_add(state->pending_y, signal->data.delta.y);
        state->metadata = signal->metadata;
        state->pending_frames++;
    }
    /* Sub-unit frames from the quantizer carry nothing worth a report. */
    if (state->pending_x == 0 && state->pending_y == 0) {
        return 0;
    }
    if (!state->armed && interval_elapsed(config, state, now)) {
        return send_report(config, state, context, now);
    }
    return arm(state, context, state->last_report_ms + config->report_interval_ms);
}

static int cursor_coalescer_stage_flush(struct zpt_stage *stage, uint32_t now_ms,
                                        struct zpt_stage_context *context) {
    if (stage == NULL || context == NULL || stage->config == NULL || stage->state == NULL) {
        return -EINVAL;
    }
    struct zpt_cursor_coalescer_state *state = stage->state;
    state->armed = false;
    if (state->pending_x == 0 && state->pending_y == 0) {
        return 0;
    }
    return send_report(stage->config, state, context, now_ms);
}

const struct zpt_stage_api zpt_cursor_coalescer_stage_api = {
    .strategy_id = "cursor-coalescer",
    .accepted_kinds = ZPT_SIGNAL_KIND_MASK(ZPT_SIGNAL_POINTER_DELTA),
    .output_kind = ZPT_SIGNAL_POINTER_DELTA,
    .flags = ZPT_STAGE_STATEFUL,
    .process = cursor_coalescer_stage_process,
    .flush = cursor_coalescer_stage_flush,
    .activate = cursor_coalescer_stage_activate,
    .reset = cursor_coalescer_stage_reset,
};
