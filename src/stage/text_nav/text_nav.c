/* SPDX-License-Identifier: MIT */

#include <errno.h>
#include <limits.h>
#include <stddef.h>

#include <zmk/pointing_tools/core/fixed.h>
#include <zmk/pointing_tools/stage/axis_intent.h>
#include <zmk/pointing_tools/stage/text_nav.h>

static int text_nav_stage_activate(struct zpt_stage *stage, enum zpt_reset_reason reason) {
    (void)reason;
    if (stage == NULL || stage->config == NULL || stage->state == NULL) {
        return -EINVAL;
    }
    const struct zpt_text_nav_config *config = stage->config;
    return config->horizontal_threshold <= 0 || config->vertical_threshold <= 0 ||
                   config->idle_timeout_ms == 0U
               ? -EINVAL
               : 0;
}

static void text_nav_stage_reset(struct zpt_stage *stage, enum zpt_reset_reason reason) {
    (void)reason;
    if (stage == NULL || stage->state == NULL) {
        return;
    }
    struct zpt_text_nav_state *state = stage->state;
    *state = (struct zpt_text_nav_state){.last_direction = ZPT_TEXT_NAV_NONE};
}

static int text_nav_stage_process(struct zpt_stage *stage, const struct zpt_signal *signal,
                                  struct zpt_stage_context *context) {
    if (stage == NULL || signal == NULL || context == NULL || stage->config == NULL ||
        stage->state == NULL || signal->kind != ZPT_SIGNAL_NORMALIZED_MOTION) {
        return -EINVAL;
    }

    const struct zpt_text_nav_config *config = stage->config;
    struct zpt_text_nav_state *state = stage->state;
    uint32_t now = zpt_stage_now_ms(context);
    uint32_t elapsed = state->have_last_frame ? now - state->last_frame_ms : 0U;

    if (!state->have_last_frame || elapsed >= config->idle_timeout_ms) {
        state->accumulated_x = 0;
        state->accumulated_y = 0;
    }
    state->last_frame_ms = now;
    state->have_last_frame = true;

    int64_t x = signal->data.fixed_vector.x;
    int64_t y = signal->data.fixed_vector.y;

    int64_t accumulated;
    int64_t threshold;
    enum zpt_text_nav_direction negative_direction;
    enum zpt_text_nav_direction positive_direction;
    switch (signal->annotations.axis_intent) {
    case ZPT_AXIS_INTENT_HORIZONTAL:
        accumulated = zpt_fixed_saturating_add(state->accumulated_x, x);
        threshold = config->horizontal_threshold;
        negative_direction = ZPT_TEXT_NAV_LEFT;
        positive_direction = ZPT_TEXT_NAV_RIGHT;
        break;
    case ZPT_AXIS_INTENT_VERTICAL:
        accumulated = zpt_fixed_saturating_add(state->accumulated_y, y);
        threshold = config->vertical_threshold;
        negative_direction = ZPT_TEXT_NAV_UP;
        positive_direction = ZPT_TEXT_NAV_DOWN;
        break;
    default:
        state->last_direction = ZPT_TEXT_NAV_NONE;
        return 0;
    }

    if (zpt_fixed_magnitude(accumulated) < (uint64_t)threshold) {
        if (signal->annotations.axis_intent == ZPT_AXIS_INTENT_HORIZONTAL) {
            state->accumulated_x = accumulated;
        } else {
            state->accumulated_y = accumulated;
        }
        state->last_direction = ZPT_TEXT_NAV_NONE;
        return 0;
    }

    enum zpt_text_nav_direction direction =
        accumulated < 0 ? negative_direction : positive_direction;
    accumulated += accumulated < 0 ? threshold : -threshold;
    if (signal->annotations.axis_intent == ZPT_AXIS_INTENT_HORIZONTAL) {
        state->accumulated_x = accumulated;
    } else {
        state->accumulated_y = accumulated;
    }
    state->last_direction = direction;

    struct zpt_signal output = {0};
    output.kind = ZPT_SIGNAL_ACTION;
    output.metadata.observed_at_ms = now;
    output.data.action.id = (uint32_t)direction;
    zpt_stage_notify(context, ZPT_STAGE_EVENT_ACTION, (int64_t)direction);
    return zpt_stage_emit(context, &output);
}

const struct zpt_stage_api zpt_text_nav_stage_api = {
    .strategy_id = "text-nav",
    .accepted_kinds = ZPT_SIGNAL_KIND_MASK(ZPT_SIGNAL_NORMALIZED_MOTION),
    .output_kind = ZPT_SIGNAL_ACTION,
    .flags = ZPT_STAGE_STATEFUL,
    .process = text_nav_stage_process,
    .activate = text_nav_stage_activate,
    .reset = text_nav_stage_reset,
};
