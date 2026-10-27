/* SPDX-License-Identifier: MIT */

#include <errno.h>
#include <limits.h>
#include <stddef.h>

#include <zmk/pointing_tools/core/fixed.h>
#include <zmk/pointing_tools/stage/cursor_acceleration.h>

#define POINTS ZPT_CURSOR_ACCELERATION_POINTS

/* Largest per-axis magnitude whose square still sums safely with another. */
#define ZPT_ACCELERATION_AXIS_LIMIT ((UINT64_C(1) << 31) - 1U)

/* Fastest speed the estimate holds, well past any curve point. */
#define ZPT_ACCELERATION_SPEED_LIMIT ((zpt_fixed_t)UINT16_MAX * ZPT_FIXED_ONE)

#define ZPT_ACCELERATION_RESTART_US (ZPT_CURSOR_ACCELERATION_RESTART_MS * 1000U)

/* Shortest interval a frame is judged over, the timestamp resolution. */
#define ZPT_ACCELERATION_MIN_INTERVAL_US 1000U

/* Fixed point with more fraction bits for the decay series. */
#define DECAY_FRACTION_BITS 24U
#define DECAY_ONE (UINT64_C(1) << DECAY_FRACTION_BITS)

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

/* Q16 percent to a Q16 ratio, rounded to nearest. */
static zpt_fixed_t percent_to_ratio(int64_t percent_fixed) { return (percent_fixed + 50) / 100; }

int zpt_cursor_acceleration_validate(const struct zpt_cursor_acceleration_config *config) {
    if (config == NULL || config->speed_rise_ms == 0U || config->speed_fall_ms == 0U) {
        return -EINVAL;
    }
    for (size_t i = 0; i < POINTS; i++) {
        if (config->curve_percent[i] == 0U ||
            config->curve_percent[i] > ZPT_CURSOR_ACCELERATION_MAX_PERCENT ||
            (i > 0U && config->curve_speed_mm_per_s[i] <= config->curve_speed_mm_per_s[i - 1U])) {
            return -EINVAL;
        }
    }
    return 0;
}

/* Tangent at control point i in Q16 percent per mm/s: the weighted harmonic
 * mean of the neighbouring secants (Fritsch-Carlson), which keeps every
 * segment monotone. It is zero at a local extremum, where the secants differ
 * in sign, and at both ends so the curve meets its flat ends smoothly. */
static int64_t point_tangent(const struct zpt_cursor_acceleration_config *config, size_t i) {
    if (i == 0U || i + 1U >= POINTS) {
        return 0;
    }
    const int64_t left_span =
        config->curve_speed_mm_per_s[i] - config->curve_speed_mm_per_s[i - 1U];
    const int64_t right_span =
        config->curve_speed_mm_per_s[i + 1U] - config->curve_speed_mm_per_s[i];
    const int64_t left_rise = (int64_t)config->curve_percent[i] - config->curve_percent[i - 1U];
    const int64_t right_rise = (int64_t)config->curve_percent[i + 1U] - config->curve_percent[i];
    if (left_rise == 0 || right_rise == 0 || (left_rise < 0) != (right_rise < 0)) {
        return 0;
    }
    /* (w1 + w2) / (w1 / left_slope + w2 / right_slope) with the slopes as
     * rise over span, cleared of fractions. Spans fit 16 bits and rises 10,
     * so neither product reaches 2^63. */
    const int64_t w1 = 2 * right_span + left_span;
    const int64_t w2 = right_span + 2 * left_span;
    const int64_t numerator = (w1 + w2) * left_rise * right_rise * ZPT_FIXED_ONE;
    const int64_t denominator = w1 * left_span * right_rise + w2 * right_span * left_rise;
    return numerator / denominator;
}

zpt_fixed_t zpt_cursor_acceleration_sensitivity(const struct zpt_cursor_acceleration_config *config,
                                                zpt_fixed_t speed) {
    const uint16_t *speeds = config->curve_speed_mm_per_s;
    const uint16_t *percents = config->curve_percent;
    if (speed <= (zpt_fixed_t)speeds[0] * ZPT_FIXED_ONE) {
        return percent_to_ratio((int64_t)percents[0] * ZPT_FIXED_ONE);
    }
    if (speed >= (zpt_fixed_t)speeds[POINTS - 1U] * ZPT_FIXED_ONE) {
        return percent_to_ratio((int64_t)percents[POINTS - 1U] * ZPT_FIXED_ONE);
    }
    size_t i = 0;
    while (speed >= (zpt_fixed_t)speeds[i + 1U] * ZPT_FIXED_ONE) {
        i++;
    }

    /* Cubic Hermite on the segment, with t its Q16 position. */
    const int64_t span = speeds[i + 1U] - speeds[i];
    const int64_t t = (speed - (zpt_fixed_t)speeds[i] * ZPT_FIXED_ONE) / span;
    const int64_t t2 = (t * t) >> ZPT_FIXED_FRACTION_BITS;
    const int64_t t3 = (t2 * t) >> ZPT_FIXED_FRACTION_BITS;
    const int64_t start = (int64_t)percents[i] * ZPT_FIXED_ONE;
    const int64_t end = (int64_t)percents[i + 1U] * ZPT_FIXED_ONE;
    const int64_t start_tangent = point_tangent(config, i) * span;
    const int64_t end_tangent = point_tangent(config, i + 1U) * span;
    int64_t value = start + ((end - start) * (3 * t2 - 2 * t3) + start_tangent * (t3 - 2 * t2 + t) +
                             end_tangent * (t3 - t2)) /
                                ZPT_FIXED_ONE;

    /* Rounding can't carry the curve past either end of its segment. */
    const int64_t low = start < end ? start : end;
    const int64_t high = start < end ? end : start;
    value = value < low ? low : (value > high ? high : value);
    return percent_to_ratio(value);
}

/* exp(-elapsed / time constant) in Q16. The exponent is cut into sixteenths,
 * each small enough for four Taylor terms, and the result squared back up. */
static zpt_fixed_t decay(uint32_t elapsed_us, uint16_t time_constant_ms) {
    const uint64_t y =
        ((uint64_t)elapsed_us << (DECAY_FRACTION_BITS - 4U)) / ((uint64_t)time_constant_ms * 1000U);
    /* Beyond eight time constants less than 0.04 percent remains. */
    if (y >= DECAY_ONE / 2U) {
        return 0;
    }
    /* 1 - y(1 - y/2 (1 - y/3 (1 - y/4))) */
    uint64_t series = DECAY_ONE - y / 4U;
    series = DECAY_ONE - ((y * series) >> DECAY_FRACTION_BITS) / 3U;
    series = DECAY_ONE - ((y * series) >> DECAY_FRACTION_BITS) / 2U;
    series = DECAY_ONE - ((y * series) >> DECAY_FRACTION_BITS);
    for (int square = 0; square < 4; square++) {
        series = (series * series) >> DECAY_FRACTION_BITS;
    }
    return (zpt_fixed_t)((series + (UINT64_C(1) << 7)) >> (DECAY_FRACTION_BITS - 16U));
}

/* The time a frame's displacement covers. The sensor's own span is exact; a
 * frame without one is timed from the previous frame's arrival. A clipped
 * span, a first frame, or a long gap all count as a restart. */
static uint32_t frame_interval_us(const struct zpt_cursor_acceleration_state *state,
                                  const struct zpt_signal_metadata *frame) {
    uint32_t interval_us = ZPT_ACCELERATION_RESTART_US;
    if ((frame->flags & ZPT_SIGNAL_FLAG_SAMPLE_SPAN_CLIPPED) != 0U) {
        return interval_us;
    }
    if (frame->sample_span_us != 0U) {
        interval_us = frame->sample_span_us;
    } else if (state->have_last_frame) {
        const uint32_t elapsed_ms = frame->observed_at_ms - state->last_frame_ms;
        if (elapsed_ms < ZPT_CURSOR_ACCELERATION_RESTART_MS) {
            interval_us = elapsed_ms * 1000U;
        }
    }
    if (interval_us < ZPT_ACCELERATION_MIN_INTERVAL_US) {
        return ZPT_ACCELERATION_MIN_INTERVAL_US;
    }
    return interval_us > ZPT_ACCELERATION_RESTART_US ? ZPT_ACCELERATION_RESTART_US : interval_us;
}

zpt_fixed_t zpt_cursor_acceleration_observe(struct zpt_cursor_acceleration_state *state,
                                            const struct zpt_cursor_acceleration_config *config,
                                            zpt_fixed_t x, zpt_fixed_t y,
                                            const struct zpt_signal_metadata *frame) {
    const uint32_t interval_us = frame_interval_us(state, frame);
    /* The magnitude is below 2^32, so the product stays below 2^52. */
    uint64_t instant = euclidean_magnitude(x, y) * UINT64_C(1000000) / interval_us;
    instant = instant > (uint64_t)ZPT_ACCELERATION_SPEED_LIMIT
                  ? (uint64_t)ZPT_ACCELERATION_SPEED_LIMIT
                  : instant;

    if (interval_us >= ZPT_ACCELERATION_RESTART_US) {
        state->speed = (zpt_fixed_t)instant;
    } else {
        const zpt_fixed_t difference = (zpt_fixed_t)instant - state->speed;
        const uint16_t time_constant =
            difference > 0 ? config->speed_rise_ms : config->speed_fall_ms;
        const zpt_fixed_t weight = ZPT_FIXED_ONE - decay(interval_us, time_constant);
        state->speed += difference * weight / ZPT_FIXED_ONE;
    }
    state->last_frame_ms = frame->observed_at_ms;
    state->have_last_frame = true;
    return state->speed;
}

/* Scale a signed Q16 value by a Q16 factor, rounding the magnitude to
 * nearest so positive and negative motion scale symmetrically. */
static zpt_fixed_t scale(zpt_fixed_t value, zpt_fixed_t factor, bool *clipped) {
    const uint64_t magnitude = zpt_fixed_magnitude(value);
    const uint64_t multiplier = (uint64_t)factor;
    if (magnitude != 0U && magnitude > (UINT64_MAX - (ZPT_FIXED_ONE / 2)) / multiplier) {
        *clipped = true;
        return value < 0 ? INT64_MIN : INT64_MAX;
    }
    uint64_t scaled = (magnitude * multiplier + (ZPT_FIXED_ONE / 2)) >> ZPT_FIXED_FRACTION_BITS;
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
    const zpt_fixed_t speed = zpt_cursor_acceleration_observe(
        state, config, signal->data.fixed_vector.x, signal->data.fixed_vector.y, &signal->metadata);
    const zpt_fixed_t sensitivity = zpt_cursor_acceleration_sensitivity(config, speed);

    bool clipped = false;
    struct zpt_signal output = *signal;
    output.data.fixed_vector.x = scale(signal->data.fixed_vector.x, sensitivity, &clipped);
    output.data.fixed_vector.y = scale(signal->data.fixed_vector.y, sensitivity, &clipped);
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
