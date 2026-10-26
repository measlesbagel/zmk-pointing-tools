/* SPDX-License-Identifier: MIT */

#include <errno.h>
#include <limits.h>
#include <stddef.h>

#include <zmk/pointing_tools/core/fixed.h>
#include <zmk/pointing_tools/stage/cursor_acceleration.h>

/* Largest per-axis magnitude whose square still sums safely with another. */
#define ZPT_ACCELERATION_AXIS_LIMIT ((UINT64_C(1) << 31) - 1U)

static uint64_t integer_sqrt(uint64_t value) {
    uint64_t root = 0;
    uint64_t bit = UINT64_C(1) << 62;
    while (bit > value) {
        bit >>= 2;
    }
    while (bit != 0U) {
        if (value >= root + bit) {
            value -= root + bit;
            root = (root >> 1) + bit;
        } else {
            root >>= 1;
        }
        bit >>= 2;
    }
    return root;
}

static uint64_t euclidean_magnitude(zpt_fixed_t x, zpt_fixed_t y) {
    uint64_t abs_x = zpt_fixed_magnitude(x);
    uint64_t abs_y = zpt_fixed_magnitude(y);
    abs_x = abs_x > ZPT_ACCELERATION_AXIS_LIMIT ? ZPT_ACCELERATION_AXIS_LIMIT : abs_x;
    abs_y = abs_y > ZPT_ACCELERATION_AXIS_LIMIT ? ZPT_ACCELERATION_AXIS_LIMIT : abs_y;
    return integer_sqrt(abs_x * abs_x + abs_y * abs_y);
}

static zpt_fixed_t percent_to_fixed(uint16_t percent) {
    return (zpt_fixed_t)percent * ZPT_FIXED_ONE / 100;
}

int zpt_cursor_acceleration_validate(const struct zpt_cursor_acceleration_config *config) {
    if (config == NULL || config->slow_speed < 0 || config->fast_speed <= config->slow_speed ||
        config->slow_gain_percent == 0U || config->fast_gain_percent == 0U ||
        config->speed_window_ms == 0U) {
        return -EINVAL;
    }
    return 0;
}

zpt_fixed_t zpt_cursor_acceleration_gain(const struct zpt_cursor_acceleration_config *config,
                                         zpt_fixed_t speed) {
    const zpt_fixed_t slow_gain = percent_to_fixed(config->slow_gain_percent);
    const zpt_fixed_t fast_gain = percent_to_fixed(config->fast_gain_percent);
    if (speed <= config->slow_speed) {
        return slow_gain;
    }
    if (speed >= config->fast_speed) {
        return fast_gain;
    }
    /* Both factors are bounded: the speed span by fast_speed and the gain
     * span by the 16-bit percentages, so the product fits in 64 bits for
     * any speed a uint16 mm/s devicetree value can express. */
    const int64_t span = config->fast_speed - config->slow_speed;
    const int64_t progress = speed - config->slow_speed;
    return slow_gain + (fast_gain - slow_gain) * progress / span;
}

zpt_fixed_t zpt_cursor_acceleration_observe(struct zpt_cursor_acceleration_state *state,
                                            const struct zpt_cursor_acceleration_config *config,
                                            zpt_fixed_t x, zpt_fixed_t y, uint32_t now_ms) {
    const uint32_t window_ms = config->speed_window_ms;
    const uint32_t elapsed_ms = state->have_last_frame ? now_ms - state->last_frame_ms : window_ms;
    uint64_t distance = (uint64_t)state->window_distance;
    /* Linear decay: in steady motion the window holds speed x window. */
    distance = elapsed_ms >= window_ms
                   ? 0U
                   : distance / window_ms * (window_ms - elapsed_ms) +
                         distance % window_ms * (window_ms - elapsed_ms) / window_ms;
    distance = zpt_fixed_saturating_add_u64(distance, euclidean_magnitude(x, y));
    distance = distance > (uint64_t)INT64_MAX / 1000U ? (uint64_t)INT64_MAX / 1000U : distance;

    state->window_distance = (zpt_fixed_t)distance;
    state->last_frame_ms = now_ms;
    state->have_last_frame = true;
    return (zpt_fixed_t)(distance * 1000U / window_ms);
}

/* Scale a signed Q16 value by a Q16 gain, rounding the magnitude to nearest
 * so positive and negative motion scale symmetrically. */
static zpt_fixed_t scale(zpt_fixed_t value, zpt_fixed_t gain, bool *clipped) {
    const uint64_t magnitude = zpt_fixed_magnitude(value);
    const uint64_t factor = (uint64_t)gain;
    if (magnitude != 0U && magnitude > (UINT64_MAX - (ZPT_FIXED_ONE / 2)) / factor) {
        *clipped = true;
        return value < 0 ? INT64_MIN : INT64_MAX;
    }
    uint64_t scaled = (magnitude * factor + (ZPT_FIXED_ONE / 2)) >> ZPT_FIXED_FRACTION_BITS;
    if (scaled > (uint64_t)INT64_MAX) {
        *clipped = true;
        scaled = (uint64_t)INT64_MAX;
    }
    return value < 0 ? -(zpt_fixed_t)scaled : (zpt_fixed_t)scaled;
}

static int cursor_acceleration_stage_activate(struct zpt_stage *stage,
                                              enum zpt_reset_reason reason) {
    (void)reason;
    if (stage == NULL || stage->config == NULL || stage->state == NULL) {
        return -EINVAL;
    }
    return zpt_cursor_acceleration_validate(stage->config);
}

static void cursor_acceleration_stage_reset(struct zpt_stage *stage, enum zpt_reset_reason reason) {
    (void)reason;
    if (stage == NULL || stage->state == NULL) {
        return;
    }
    struct zpt_cursor_acceleration_state *state = stage->state;
    *state = (struct zpt_cursor_acceleration_state){0};
}

static int cursor_acceleration_stage_process(struct zpt_stage *stage,
                                             const struct zpt_signal *signal,
                                             struct zpt_stage_context *context) {
    if (stage == NULL || signal == NULL || context == NULL || stage->config == NULL ||
        stage->state == NULL || signal->kind != ZPT_SIGNAL_NORMALIZED_MOTION) {
        return -EINVAL;
    }

    const struct zpt_cursor_acceleration_config *config = stage->config;
    struct zpt_cursor_acceleration_state *state = stage->state;
    const zpt_fixed_t speed =
        zpt_cursor_acceleration_observe(state, config, signal->data.fixed_vector.x,
                                        signal->data.fixed_vector.y, zpt_stage_now_ms(context));
    const zpt_fixed_t gain = zpt_cursor_acceleration_gain(config, speed);

    bool clipped = false;
    struct zpt_signal output = *signal;
    output.data.fixed_vector.x = scale(signal->data.fixed_vector.x, gain, &clipped);
    output.data.fixed_vector.y = scale(signal->data.fixed_vector.y, gain, &clipped);
    output.annotations.speed_per_second = speed;
    if (clipped) {
        output.metadata.flags |= ZPT_SIGNAL_FLAG_CLIPPED;
    }
    return zpt_stage_emit(context, &output);
}

const struct zpt_stage_api zpt_cursor_acceleration_stage_api = {
    .strategy_id = "cursor-acceleration",
    .accepted_kinds = ZPT_SIGNAL_KIND_MASK(ZPT_SIGNAL_NORMALIZED_MOTION),
    .output_kind = ZPT_SIGNAL_NORMALIZED_MOTION,
    .flags = ZPT_STAGE_STATEFUL,
    .process = cursor_acceleration_stage_process,
    .activate = cursor_acceleration_stage_activate,
    .reset = cursor_acceleration_stage_reset,
};
