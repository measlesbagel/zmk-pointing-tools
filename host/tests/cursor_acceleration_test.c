/* SPDX-License-Identifier: MIT */

#include <assert.h>
#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>

#include <zmk/pointing_tools/core/pipeline.h>
#include <zmk/pointing_tools/stage/cursor_acceleration.h>

#define MM(value) ((zpt_fixed_t)(value) * ZPT_FIXED_ONE)
#define POINTS ZPT_CURSOR_ACCELERATION_POINTS

struct capture_state {
    struct zpt_signal signal;
    uint32_t outputs;
};

static int capture_emit(struct zpt_sink *sink, const struct zpt_signal *signal) {
    struct capture_state *capture = sink->state;
    capture->signal = *signal;
    capture->outputs++;
    return 0;
}

static const struct zpt_sink_api capture_api = {
    .type_id = "normalized-capture",
    .accepted_kinds = ZPT_SIGNAL_KIND_MASK(ZPT_SIGNAL_NORMALIZED_MOTION),
    .emit = capture_emit,
};

struct fixture {
    struct zpt_cursor_acceleration_config config;
    struct zpt_cursor_acceleration_state state;
    struct zpt_stage stage;
    struct zpt_stage *stages[1];
    struct capture_state capture;
    struct zpt_sink sink;
    struct zpt_pipeline pipeline;
};

/* The binding's default curve, tuned for a 684 CPI trackball. */
static const struct zpt_cursor_acceleration_config trackball = {
    .curve_speed_mm_per_s = {0, 8, 20, 45, 90, 160, 260, 400},
    .curve_percent = {65, 66, 73, 99, 159, 225, 264, 283},
    .speed_rise_ms = 10,
    .speed_fall_ms = 5,
};

static void fixture_init(struct fixture *fixture,
                         const struct zpt_cursor_acceleration_config *config) {
    *fixture = (struct fixture){.config = *config};
    fixture->stage = (struct zpt_stage){
        .stable_id = "cursor-acceleration",
        .api = &zpt_cursor_acceleration_stage_api,
        .config = &fixture->config,
        .state = &fixture->state,
    };
    fixture->stages[0] = &fixture->stage;
    fixture->sink = (struct zpt_sink){
        .stable_id = "capture",
        .api = &capture_api,
        .state = &fixture->capture,
    };
    fixture->pipeline = (struct zpt_pipeline){
        .stable_id = "acceleration-test",
        .input_kind = ZPT_SIGNAL_NORMALIZED_MOTION,
        .stages = fixture->stages,
        .stage_count = 1,
        .sink = &fixture->sink,
        .dispatch_budget = 2,
    };
    assert(zpt_pipeline_validate(&fixture->pipeline) == 0);
    assert(zpt_pipeline_activate(&fixture->pipeline, ZPT_RESET_PIPELINE_ENTERED) == 0);
}

static const struct zpt_signal *push(struct fixture *fixture, uint32_t now_ms, zpt_fixed_t x,
                                     zpt_fixed_t y) {
    struct zpt_signal signal = {
        .kind = ZPT_SIGNAL_NORMALIZED_MOTION,
        .metadata = {.observed_at_ms = now_ms, .resolution_cpi = 684},
        .data.fixed_vector = {.x = x, .y = y},
    };
    struct zpt_pipeline_result result;
    assert(zpt_pipeline_push(&fixture->pipeline, &signal, &result) == 0);
    return &fixture->capture.signal;
}

static zpt_fixed_t observe(struct zpt_cursor_acceleration_state *state,
                           const struct zpt_cursor_acceleration_config *config, uint32_t now_ms,
                           uint32_t span_us, zpt_fixed_t distance) {
    const struct zpt_signal_metadata frame = {.observed_at_ms = now_ms, .sample_span_us = span_us};
    return zpt_cursor_acceleration_observe(state, config, distance, 0, &frame);
}

static int64_t absolute(int64_t value) { return value < 0 ? -value : value; }

static double sensitivity_at(const struct zpt_cursor_acceleration_config *config, double speed) {
    const zpt_fixed_t fixed = (zpt_fixed_t)llround(speed * (double)ZPT_FIXED_ONE);
    return (double)zpt_cursor_acceleration_sensitivity(config, fixed) / (double)ZPT_FIXED_ONE;
}

/* Floating-point Fritsch-Carlson PCHIP with flat ends, as the web tuner
 * draws it. */
static double reference_curve(const struct zpt_cursor_acceleration_config *config, double speed) {
    double x[POINTS];
    double y[POINTS];
    double tangent[POINTS] = {0};
    for (size_t i = 0; i < POINTS; i++) {
        x[i] = config->curve_speed_mm_per_s[i];
        y[i] = config->curve_percent[i] / 100.0;
    }
    if (speed <= x[0]) {
        return y[0];
    }
    if (speed >= x[POINTS - 1]) {
        return y[POINTS - 1];
    }
    for (size_t i = 1; i + 1 < POINTS; i++) {
        const double h0 = x[i] - x[i - 1];
        const double h1 = x[i + 1] - x[i];
        const double s0 = (y[i] - y[i - 1]) / h0;
        const double s1 = (y[i + 1] - y[i]) / h1;
        if (s0 * s1 > 0) {
            const double w1 = 2 * h1 + h0;
            const double w2 = h1 + 2 * h0;
            tangent[i] = (w1 + w2) / (w1 / s0 + w2 / s1);
        }
    }
    size_t i = 0;
    while (speed >= x[i + 1]) {
        i++;
    }
    const double h = x[i + 1] - x[i];
    const double t = (speed - x[i]) / h;
    return (2 * t * t * t - 3 * t * t + 1) * y[i] + (t * t * t - 2 * t * t + t) * h * tangent[i] +
           (-2 * t * t * t + 3 * t * t) * y[i + 1] + (t * t * t - t * t) * h * tangent[i + 1];
}

static void test_curve_passes_through_its_points(void) {
    for (size_t i = 0; i < POINTS; i++) {
        assert(zpt_cursor_acceleration_sensitivity(&trackball,
                                                   MM(trackball.curve_speed_mm_per_s[i])) ==
               (trackball.curve_percent[i] * ZPT_FIXED_ONE + 50) / 100);
    }
    /* Flat beyond the last point, and before the first when it isn't 0. */
    assert(zpt_cursor_acceleration_sensitivity(&trackball, MM(5000)) ==
           zpt_cursor_acceleration_sensitivity(&trackball, MM(400)));
    assert(zpt_cursor_acceleration_sensitivity(&trackball, INT64_MAX) ==
           zpt_cursor_acceleration_sensitivity(&trackball, MM(400)));
    struct zpt_cursor_acceleration_config shifted = trackball;
    shifted.curve_speed_mm_per_s[0] = 4;
    assert(zpt_cursor_acceleration_sensitivity(&shifted, 0) ==
           zpt_cursor_acceleration_sensitivity(&shifted, MM(4)));
    assert(zpt_cursor_acceleration_sensitivity(&shifted, MM(1)) ==
           zpt_cursor_acceleration_sensitivity(&shifted, MM(4)));
}

/* The firmware's integer curve is the curve the tuner draws. */
static void test_curve_matches_the_reference(void) {
    for (int step = 0; step <= 450 * 4; step++) {
        const double speed = step / 4.0;
        assert(fabs(sensitivity_at(&trackball, speed) - reference_curve(&trackball, speed)) <
               0.0005);
    }
}

/* Rising points give a curve that never dips, and a curve with a dip never
 * overshoots the points around it. */
static void test_curve_stays_within_its_points(void) {
    double previous = 0;
    for (int step = 0; step <= 450 * 8; step++) {
        const double value = sensitivity_at(&trackball, step / 8.0);
        assert(value >= previous);
        previous = value;
    }

    const struct zpt_cursor_acceleration_config dip = {
        .curve_speed_mm_per_s = {0, 10, 20, 30, 40, 50, 60, 1000},
        .curve_percent = {100, 300, 50, 50, 400, 1, 1000, 1000},
        .speed_rise_ms = 10,
        .speed_fall_ms = 5,
    };
    for (size_t i = 0; i + 1 < POINTS; i++) {
        const double low =
            (dip.curve_percent[i] < dip.curve_percent[i + 1] ? dip.curve_percent[i]
                                                             : dip.curve_percent[i + 1]) /
            100.0;
        const double high =
            (dip.curve_percent[i] > dip.curve_percent[i + 1] ? dip.curve_percent[i]
                                                             : dip.curve_percent[i + 1]) /
            100.0;
        for (int step = dip.curve_speed_mm_per_s[i] * 8;
             step <= dip.curve_speed_mm_per_s[i + 1] * 8; step++) {
            const double speed = step / 8.0;
            const double value = sensitivity_at(&dip, speed);
            assert(value >= low - 0.00001 && value <= high + 0.00001);
            assert(fabs(value - reference_curve(&dip, speed)) < 0.001);
        }
    }
}

static void test_validation(void) {
    assert(zpt_cursor_acceleration_validate(&trackball) == 0);
    struct zpt_cursor_acceleration_config bad = trackball;
    bad.curve_speed_mm_per_s[4] = bad.curve_speed_mm_per_s[3];
    assert(zpt_cursor_acceleration_validate(&bad) == -EINVAL);
    bad = trackball;
    bad.curve_speed_mm_per_s[7] = 100;
    assert(zpt_cursor_acceleration_validate(&bad) == -EINVAL);
    bad = trackball;
    bad.curve_percent[2] = 0;
    assert(zpt_cursor_acceleration_validate(&bad) == -EINVAL);
    bad = trackball;
    bad.curve_percent[7] = ZPT_CURSOR_ACCELERATION_MAX_PERCENT + 1U;
    assert(zpt_cursor_acceleration_validate(&bad) == -EINVAL);
    bad = trackball;
    bad.speed_rise_ms = 0;
    assert(zpt_cursor_acceleration_validate(&bad) == -EINVAL);
    bad = trackball;
    bad.speed_fall_ms = 0;
    assert(zpt_cursor_acceleration_validate(&bad) == -EINVAL);
    assert(zpt_cursor_acceleration_validate(NULL) == -EINVAL);
}

/* Every point at 100 percent passes motion through bit for bit. */
static void test_flat_curve_is_identity(void) {
    struct zpt_cursor_acceleration_config off = trackball;
    for (size_t i = 0; i < POINTS; i++) {
        off.curve_percent[i] = 100;
    }
    struct fixture fixture;
    fixture_init(&fixture, &off);
    const zpt_fixed_t values[] = {1, -1, 12345, -98765, MM(3), -MM(40)};
    for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
        const struct zpt_signal *out = push(&fixture, (uint32_t)i * 4U, values[i], -values[i]);
        assert(out->data.fixed_vector.x == values[i]);
        assert(out->data.fixed_vector.y == -values[i]);
    }
}

/* The estimate settles on the true speed for steady motion, whether frames
 * arrive evenly or with ragged millisecond timing. */
static void test_speed_estimate_tracks_steady_motion(void) {
    struct zpt_cursor_acceleration_state state = {0};
    zpt_fixed_t speed = 0;
    /* 100 mm/s at 8 ms frames is 0.8 mm per frame. */
    for (uint32_t t = 0; t <= 200; t += 8) {
        speed = observe(&state, &trackball, t, 0, MM(8) / 10);
    }
    assert(absolute(speed - MM(100)) < MM(1) / 10);

    state = (struct zpt_cursor_acceleration_state){0};
    uint32_t now = 0;
    for (int frame = 0; frame < 80; frame++) {
        const uint32_t step = frame % 2 == 0 ? 1U : 7U;
        now += step;
        /* Each frame carries the distance its own interval covered. */
        speed = observe(&state, &trackball, now, 0, MM(step) / 10);
    }
    assert(absolute(speed - MM(100)) < MM(1) / 10);
}

/* Frames relayed from a split half arrive in bursts. Their sample spans
 * still time them exactly. */
static void test_sample_spans_time_bunched_frames(void) {
    struct zpt_cursor_acceleration_state state = {0};
    zpt_fixed_t speed = 0;
    for (uint32_t burst = 0; burst < 20; burst++) {
        /* Three 4 ms samples land together every 12 ms at 50 mm/s. */
        for (int sample = 0; sample < 3; sample++) {
            speed = observe(&state, &trackball, burst * 12U, 4000, MM(2) / 10);
        }
    }
    assert(absolute(speed - MM(50)) < MM(1) / 10);
}

/* One frame covering exactly one rise time closes 1 - 1/e of the gap. */
static void test_estimate_follows_with_its_time_constants(void) {
    struct zpt_cursor_acceleration_state state = {0};
    /* 1 mm in 10 ms is 100 mm/s. */
    zpt_fixed_t speed = observe(&state, &trackball, 10, 10000, MM(1));
    assert(absolute(speed - (zpt_fixed_t)(100.0 * (1.0 - exp(-1.0)) * ZPT_FIXED_ONE)) <
           MM(1) / 100);

    /* A fall covering one fall time closes the same share of the drop. */
    state.speed = MM(100);
    speed = observe(&state, &trackball, 15, 5000, 0);
    assert(absolute(speed - (zpt_fixed_t)(100.0 * exp(-1.0) * ZPT_FIXED_ONE)) < MM(1) / 100);
}

/* A flick at 1 ms frames is recognized within a couple of rise times, and
 * the estimate drops as soon as the ball slows to aim. */
static void test_flicks_register_quickly_and_stop_cleanly(void) {
    struct zpt_cursor_acceleration_state state = {0};
    uint32_t now = 0;
    observe(&state, &trackball, now, 0, MM(1) / 100);
    uint32_t reached_ms = 0;
    for (now = 1; now <= 60; now++) {
        /* 300 mm/s is 0.3 mm per millisecond. */
        if (observe(&state, &trackball, now, 0, MM(3) / 10) >= MM(270) && reached_ms == 0U) {
            reached_ms = now;
        }
    }
    /* Ninety percent takes ln(10) rise times, 23 ms. */
    assert(reached_ms >= 20U && reached_ms <= 25U);

    uint32_t settled_ms = 0;
    for (uint32_t t = 1; t <= 40; t++) {
        /* 10 mm/s while aiming. */
        if (observe(&state, &trackball, now + t, 0, MM(1) / 100) <= MM(20) && settled_ms == 0U) {
            settled_ms = t;
        }
    }
    assert(settled_ms > 0U && settled_ms <= 17U);
}

/* The first frame after a pause is judged slow instead of by one frame's
 * distance over an unknown interval. */
static void test_motion_after_a_pause_starts_slow(void) {
    struct fixture fixture;
    fixture_init(&fixture, &trackball);
    for (uint32_t t = 0; t <= 200; t += 8) {
        push(&fixture, t, MM(4), 0);
    }
    /* 500 mm/s is past the last point: 283 percent. */
    assert(fixture.capture.signal.data.fixed_vector.x ==
           (MM(4) * ((283 * ZPT_FIXED_ONE + 50) / 100) + ZPT_FIXED_ONE / 2) / ZPT_FIXED_ONE);

    const struct zpt_signal *out = push(&fixture, 2000, MM(1) / 4, 0);
    /* 0.25 mm over the 40 ms restart interval is 6.25 mm/s. */
    assert(out->annotations.speed_per_second == MM(25) / 4);
    const zpt_fixed_t expected = zpt_cursor_acceleration_sensitivity(&trackball, MM(25) / 4);
    assert(out->data.fixed_vector.x == (MM(1) / 4 * expected + ZPT_FIXED_ONE / 2) / ZPT_FIXED_ONE);
}

/* A span too long for the split codec restarts the estimate too. */
static void test_clipped_span_restarts(void) {
    struct zpt_cursor_acceleration_state state = {0};
    for (uint32_t t = 0; t <= 100; t += 4) {
        observe(&state, &trackball, t, 4000, MM(1));
    }
    const struct zpt_signal_metadata frame = {
        .observed_at_ms = 104,
        .sample_span_us = 30000,
        .flags = ZPT_SIGNAL_FLAG_SAMPLE_SPAN_CLIPPED,
    };
    /* 2 mm over the 40 ms restart interval is 50 mm/s. */
    assert(zpt_cursor_acceleration_observe(&state, &trackball, MM(2), 0, &frame) == MM(50));
}

/* One sensitivity scales both axes, so a diagonal keeps its direction and
 * accelerates like cardinal motion at the same speed. */
static void test_diagonals_accelerate_like_cardinals(void) {
    struct fixture cardinal;
    struct fixture diagonal;
    fixture_init(&cardinal, &trackball);
    fixture_init(&diagonal, &trackball);
    /* 1 mm per 8 ms frame along x, and the same length at 45 degrees
     * (0.7071 mm on each axis). */
    const zpt_fixed_t leg = 46341;
    for (uint32_t t = 0; t <= 160; t += 8) {
        push(&cardinal, t, MM(1), 0);
        push(&diagonal, t, leg, -leg);
    }
    const struct zpt_signal *c = &cardinal.capture.signal;
    const struct zpt_signal *d = &diagonal.capture.signal;
    assert(d->data.fixed_vector.x == -d->data.fixed_vector.y);
    const int64_t cardinal_gain = c->data.fixed_vector.x * 1000 / MM(1);
    const int64_t diagonal_gain = d->data.fixed_vector.x * 1000 / leg;
    assert(absolute(cardinal_gain - diagonal_gain) <= 2);
    assert(absolute(c->annotations.speed_per_second - d->annotations.speed_per_second) < MM(1));
}

static void test_negative_motion_scales_symmetrically(void) {
    struct fixture positive;
    struct fixture negative;
    fixture_init(&positive, &trackball);
    fixture_init(&negative, &trackball);
    for (uint32_t t = 0; t <= 96; t += 8) {
        push(&positive, t, 54321, 777);
        push(&negative, t, -54321, -777);
        assert(positive.capture.signal.data.fixed_vector.x ==
               -negative.capture.signal.data.fixed_vector.x);
        assert(positive.capture.signal.data.fixed_vector.y ==
               -negative.capture.signal.data.fixed_vector.y);
    }
}

static void test_saturation_is_flagged(void) {
    struct fixture fixture;
    fixture_init(&fixture, &trackball);
    const struct zpt_signal *out = push(&fixture, 0, INT64_MAX / 2, INT64_MIN / 2);
    assert(out->metadata.flags & ZPT_SIGNAL_FLAG_CLIPPED);
    assert(out->data.fixed_vector.x == INT64_MAX);
    assert(out->data.fixed_vector.y == INT64_MIN);
    assert(out->annotations.speed_per_second == (zpt_fixed_t)UINT16_MAX * ZPT_FIXED_ONE);
}

static void test_reset_forgets_speed(void) {
    struct fixture fixture;
    fixture_init(&fixture, &trackball);
    for (uint32_t t = 0; t <= 96; t += 8) {
        push(&fixture, t, MM(3), 0);
    }
    zpt_pipeline_reset(&fixture.pipeline, ZPT_RESET_PIPELINE_LEFT);
    assert(fixture.state.speed == 0 && !fixture.state.have_last_frame);
}

int main(void) {
    test_curve_passes_through_its_points();
    test_curve_matches_the_reference();
    test_curve_stays_within_its_points();
    test_validation();
    test_flat_curve_is_identity();
    test_speed_estimate_tracks_steady_motion();
    test_sample_spans_time_bunched_frames();
    test_estimate_follows_with_its_time_constants();
    test_flicks_register_quickly_and_stop_cleanly();
    test_motion_after_a_pause_starts_slow();
    test_clipped_span_restarts();
    test_diagonals_accelerate_like_cardinals();
    test_negative_motion_scales_symmetrically();
    test_saturation_is_flagged();
    test_reset_forgets_speed();
    puts("cursor acceleration tests passed");
    return 0;
}
