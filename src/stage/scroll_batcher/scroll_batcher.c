/* SPDX-License-Identifier: MIT */

#include <errno.h>
#include <limits.h>
#include <stddef.h>

#include <zmk/pointing_tools/stage/scroll_batcher.h>

static int64_t saturating_add_i64(int64_t left, int64_t right) {
    if (right > 0 && left > INT64_MAX - right) {
        return INT64_MAX;
    }
    if (right < 0 && left < INT64_MIN - right) {
        return INT64_MIN;
    }
    return left + right;
}

static int16_t take_scaled(int64_t *pending, int64_t *remainder, uint16_t multiplier,
                           uint16_t divisor) {
    int64_t numerator = *pending * multiplier + *remainder;
    int64_t scaled = numerator / divisor;
    int16_t output =
        scaled > INT16_MAX ? INT16_MAX : (scaled < INT16_MIN ? INT16_MIN : (int16_t)scaled);

    /* Keep both fractional and HID-range overflow for a later report. */
    int64_t remaining = numerator - ((int64_t)output * divisor);
    *remainder =
        remaining > INT32_MAX ? INT32_MAX : (remaining < INT32_MIN ? INT32_MIN : remaining);
    *pending = 0;
    return output;
}

static bool suppression_active(const struct zpt_scroll_batcher_config *config,
                               const struct zpt_signal *signal, uint32_t now) {
    return config->suppression != NULL && config->suppression->is_suppressed != NULL &&
           config->suppression->is_suppressed(config->suppression->context, signal, now);
}

static int scroll_batcher_stage_activate(struct zpt_stage *stage, enum zpt_reset_reason reason) {
    (void)reason;
    if (stage == NULL || stage->config == NULL || stage->state == NULL) {
        return -EINVAL;
    }
    const struct zpt_scroll_batcher_config *config = stage->config;
    return config->scale_multiplier == 0U || config->scale_divisor == 0U ||
                   config->report_interval_ms == 0U
               ? -EINVAL
               : 0;
}

static void scroll_batcher_stage_reset(struct zpt_stage *stage, enum zpt_reset_reason reason) {
    (void)reason;
    if (stage == NULL || stage->state == NULL) {
        return;
    }
    struct zpt_scroll_batcher_state *state = stage->state;
    /* Lifecycle resets also clear the fractional remainder; unlike the
     * legacy suppression path, a route change starts the batcher fresh. */
    *state = (struct zpt_scroll_batcher_state){0};
}

static int scroll_batcher_stage_process(struct zpt_stage *stage, const struct zpt_signal *signal,
                                        struct zpt_stage_context *context) {
    if (stage == NULL || signal == NULL || context == NULL || stage->config == NULL ||
        stage->state == NULL || signal->kind != ZPT_SIGNAL_RAW_MOTION) {
        return -EINVAL;
    }

    const struct zpt_scroll_batcher_config *config = stage->config;
    struct zpt_scroll_batcher_state *state = stage->state;
    uint32_t now = zpt_stage_now_ms(context);

    if (suppression_active(config, signal, now)) {
        state->pending_x = 0;
        state->pending_y = 0;
        zpt_stage_notify(context, ZPT_STAGE_EVENT_SUPPRESSED, 0);
        return 0;
    }

    state->pending_x = saturating_add_i64(state->pending_x, signal->data.raw_motion.x_counts);
    state->pending_y = saturating_add_i64(state->pending_y, signal->data.raw_motion.y_counts);

    if (!state->armed) {
        int ret = zpt_stage_schedule_flush(context, now + config->report_interval_ms);
        if (ret < 0) {
            return ret;
        }
        state->armed = true;
    }
    return 0;
}

static int scroll_batcher_stage_flush(struct zpt_stage *stage, uint32_t now_ms,
                                      struct zpt_stage_context *context) {
    (void)now_ms;
    if (stage == NULL || context == NULL || stage->config == NULL || stage->state == NULL) {
        return -EINVAL;
    }
    const struct zpt_scroll_batcher_config *config = stage->config;
    struct zpt_scroll_batcher_state *state = stage->state;
    state->armed = false;

    int16_t horizontal = take_scaled(&state->pending_x, &state->remainder_x,
                                     config->scale_multiplier, config->scale_divisor);
    int16_t vertical = take_scaled(&state->pending_y, &state->remainder_y, config->scale_multiplier,
                                   config->scale_divisor);
    if (horizontal == 0 && vertical == 0) {
        return 0;
    }

    struct zpt_signal output = {0};
    output.kind = ZPT_SIGNAL_SCROLL_STEPS;
    output.metadata.observed_at_ms = stage->deadline_ms;
    output.data.delta.x = horizontal;
    output.data.delta.y = vertical;
    zpt_stage_notify(context, ZPT_STAGE_EVENT_FLUSHED,
                     (int64_t)(horizontal > 0 ? horizontal : -horizontal) +
                         (vertical > 0 ? vertical : -vertical));
    return zpt_stage_emit(context, &output);
}

const struct zpt_stage_api zpt_scroll_batcher_stage_api = {
    .strategy_id = "scroll-batcher",
    .accepted_kinds = ZPT_SIGNAL_KIND_MASK(ZPT_SIGNAL_RAW_MOTION),
    .output_kind = ZPT_SIGNAL_SCROLL_STEPS,
    .flags = ZPT_STAGE_STATEFUL,
    .process = scroll_batcher_stage_process,
    .flush = scroll_batcher_stage_flush,
    .activate = scroll_batcher_stage_activate,
    .reset = scroll_batcher_stage_reset,
};
