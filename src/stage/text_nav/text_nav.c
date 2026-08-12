/* SPDX-License-Identifier: MIT */

#include <errno.h>
#include <limits.h>
#include <stddef.h>

#include <zmk/pointing_tools/stage/axis_intent.h>
#include <zmk/pointing_tools/stage/text_nav.h>

static int64_t saturating_add_i64(int64_t left, int64_t right) {
    if (right > 0 && left > INT64_MAX - right) {
        return INT64_MAX;
    }
    if (right < 0 && left < INT64_MIN - right) {
        return INT64_MIN;
    }
    return left + right;
}

static uint64_t magnitude(int64_t value) {
    if (value >= 0) {
        return (uint64_t)value;
    }
    return (uint64_t)(-(value + 1)) + 1U;
}

static bool dominates(uint64_t major, uint64_t minor, uint16_t ratio_percent) {
    if (minor != 0U && ratio_percent != 0U && minor > UINT64_MAX / ratio_percent) {
        return true;
    }
    uint64_t product = minor * ratio_percent;
    uint64_t required = product / 100U + (product % 100U != 0U ? 1U : 0U);
    return major >= required;
}

static int text_nav_stage_activate(struct zpt_stage *stage, enum zpt_reset_reason reason) {
    (void)reason;
    if (stage == NULL || stage->config == NULL || stage->state == NULL) {
        return -EINVAL;
    }
    const struct zpt_text_nav_config *config = stage->config;
    return config->horizontal_threshold <= 0 || config->vertical_threshold <= 0 ||
                   config->idle_timeout_ms == 0U || config->activation_distance <= 0 ||
                   config->engage_ratio_percent == 0U
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
        stage->state == NULL || signal->kind != ZPT_SIGNAL_RAW_MOTION) {
        return -EINVAL;
    }

    const struct zpt_text_nav_config *config = stage->config;
    struct zpt_text_nav_state *state = stage->state;
    uint32_t now = zpt_stage_now_ms(context);
    uint32_t elapsed = state->have_last_frame ? now - state->last_frame_ms : 0U;
    bool continuing_gesture = state->have_last_frame;

    if (!continuing_gesture || elapsed >= config->idle_timeout_ms) {
        state->accumulated_x = 0;
        state->accumulated_y = 0;
        state->intent = ZPT_AXIS_INTENT_UNDECIDED;
    }
    state->last_frame_ms = now;
    state->have_last_frame = true;

    int64_t x = signal->data.raw_motion.x_counts;
    int64_t y = signal->data.raw_motion.y_counts;

    if (state->intent == ZPT_AXIS_INTENT_UNDECIDED) {
        state->accumulated_x = saturating_add_i64(state->accumulated_x, x);
        state->accumulated_y = saturating_add_i64(state->accumulated_y, y);

        uint64_t horizontal = magnitude(state->accumulated_x);
        uint64_t vertical = magnitude(state->accumulated_y);
        if (horizontal + vertical < (uint64_t)config->activation_distance) {
            state->last_direction = ZPT_TEXT_NAV_NONE;
            return 0;
        }

        if (dominates(horizontal, vertical, config->engage_ratio_percent)) {
            state->intent = ZPT_AXIS_INTENT_HORIZONTAL;
            state->accumulated_y = 0;
        } else if (dominates(vertical, horizontal, config->engage_ratio_percent)) {
            state->intent = ZPT_AXIS_INTENT_VERTICAL;
            state->accumulated_x = 0;
        } else {
            state->last_direction = ZPT_TEXT_NAV_NONE;
            return 0;
        }
    } else if (state->intent == ZPT_AXIS_INTENT_HORIZONTAL) {
        state->accumulated_x = saturating_add_i64(state->accumulated_x, x);
    } else {
        state->accumulated_y = saturating_add_i64(state->accumulated_y, y);
    }

    int64_t *movement;
    int64_t threshold;
    enum zpt_text_nav_direction negative_direction;
    enum zpt_text_nav_direction positive_direction;
    if (state->intent == ZPT_AXIS_INTENT_HORIZONTAL) {
        movement = &state->accumulated_x;
        threshold = config->horizontal_threshold;
        negative_direction = ZPT_TEXT_NAV_LEFT;
        positive_direction = ZPT_TEXT_NAV_RIGHT;
    } else {
        movement = &state->accumulated_y;
        threshold = config->vertical_threshold;
        negative_direction = ZPT_TEXT_NAV_UP;
        positive_direction = ZPT_TEXT_NAV_DOWN;
    }

    if (magnitude(*movement) < (uint64_t)threshold) {
        state->last_direction = ZPT_TEXT_NAV_NONE;
        return 0;
    }

    enum zpt_text_nav_direction direction = *movement < 0 ? negative_direction : positive_direction;
    *movement += *movement < 0 ? threshold : -threshold;
    state->last_direction = direction;

    struct zpt_signal output = {0};
    output.kind = ZPT_SIGNAL_ACTION;
    output.metadata.observed_at_ms = now;
    output.data.action.id = (uint32_t)direction;
    output.data.action.value = 1;
    zpt_stage_notify(context, ZPT_STAGE_EVENT_ACTION, (int64_t)direction);
    return zpt_stage_emit(context, &output);
}

const struct zpt_stage_api zpt_text_nav_stage_api = {
    .strategy_id = "text-nav",
    .accepted_kinds = ZPT_SIGNAL_KIND_MASK(ZPT_SIGNAL_RAW_MOTION),
    .output_kind = ZPT_SIGNAL_ACTION,
    .flags = ZPT_STAGE_STATEFUL,
    .process = text_nav_stage_process,
    .activate = text_nav_stage_activate,
    .reset = text_nav_stage_reset,
};
