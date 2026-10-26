/* SPDX-License-Identifier: MIT */

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>

#include <zmk/pointing_tools/core/pipeline.h>
#include <zmk/pointing_tools/stage/cursor_acceleration.h>

#define MM(value) ((zpt_fixed_t)(value) * ZPT_FIXED_ONE)

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

static const struct zpt_cursor_acceleration_config accelerating = {
    .slow_speed = MM(20),
    .fast_speed = MM(220),
    .slow_gain_percent = 50,
    .fast_gain_percent = 250,
    .speed_window_ms = 32,
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

static int64_t absolute(int64_t value) { return value < 0 ? -value : value; }

static void test_gain_follows_the_ramp(void) {
    const zpt_fixed_t half = ZPT_FIXED_ONE / 2;
    assert(zpt_cursor_acceleration_gain(&accelerating, 0) == half);
    assert(zpt_cursor_acceleration_gain(&accelerating, MM(20)) == half);
    /* Halfway through the speed span is halfway between the gains. */
    assert(zpt_cursor_acceleration_gain(&accelerating, MM(120)) == ZPT_FIXED_ONE * 3 / 2);
    assert(zpt_cursor_acceleration_gain(&accelerating, MM(220)) == ZPT_FIXED_ONE * 5 / 2);
    assert(zpt_cursor_acceleration_gain(&accelerating, INT64_MAX) == ZPT_FIXED_ONE * 5 / 2);
}

static void test_validation(void) {
    assert(zpt_cursor_acceleration_validate(&accelerating) == 0);
    struct zpt_cursor_acceleration_config bad = accelerating;
    bad.fast_speed = bad.slow_speed;
    assert(zpt_cursor_acceleration_validate(&bad) == -EINVAL);
    bad = accelerating;
    bad.slow_gain_percent = 0;
    assert(zpt_cursor_acceleration_validate(&bad) == -EINVAL);
    bad = accelerating;
    bad.speed_window_ms = 0;
    assert(zpt_cursor_acceleration_validate(&bad) == -EINVAL);
    assert(zpt_cursor_acceleration_validate(NULL) == -EINVAL);
}

/* Equal gains of 100 percent pass motion through bit for bit. */
static void test_unit_gains_are_identity(void) {
    struct zpt_cursor_acceleration_config off = accelerating;
    off.slow_gain_percent = 100;
    off.fast_gain_percent = 100;
    struct fixture fixture;
    fixture_init(&fixture, &off);
    const zpt_fixed_t values[] = {1, -1, 12345, -98765, MM(3), -MM(40)};
    for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
        const struct zpt_signal *out = push(&fixture, (uint32_t)i * 4U, values[i], -values[i]);
        assert(out->data.fixed_vector.x == values[i]);
        assert(out->data.fixed_vector.y == -values[i]);
    }
}

/* The windowed estimate settles on the true speed for steady motion,
 * whether frames arrive evenly or with ragged millisecond timing. */
static void test_speed_estimate_tracks_steady_motion(void) {
    struct zpt_cursor_acceleration_state state = {0};
    zpt_fixed_t speed = 0;
    /* 100 mm/s at 8 ms frames is 0.8 mm per frame. */
    for (uint32_t t = 0; t <= 400; t += 8) {
        speed = zpt_cursor_acceleration_observe(&state, &accelerating, MM(8) / 10, 0, t);
    }
    assert(absolute(speed - MM(100)) < MM(3));

    state = (struct zpt_cursor_acceleration_state){0};
    uint32_t now = 0;
    for (int frame = 0; frame < 80; frame++) {
        const uint32_t step = frame % 2 == 0 ? 1U : 7U;
        now += step;
        /* Each frame carries the distance its own interval covered. */
        speed = zpt_cursor_acceleration_observe(&state, &accelerating, MM(step) / 10, 0, now);
    }
    assert(absolute(speed - MM(100)) < MM(5));
}

/* The first frame after a pause is judged slow instead of by one frame's
 * distance over an unknown interval. */
static void test_motion_after_a_pause_starts_slow(void) {
    struct fixture fixture;
    fixture_init(&fixture, &accelerating);
    for (uint32_t t = 0; t <= 200; t += 8) {
        push(&fixture, t, MM(2), 0);
    }
    assert(fixture.capture.signal.data.fixed_vector.x == MM(5)); /* fast gain 250 % */

    const struct zpt_signal *out = push(&fixture, 2000, MM(1) / 4, 0);
    /* 0.25 mm over a 32 ms window is about 8 mm/s, below the slow speed. */
    assert(out->data.fixed_vector.x == MM(1) / 8);
}

/* One gain scales both axes, so a diagonal keeps its direction and gets the
 * same gain as cardinal motion at the same speed. */
static void test_diagonals_accelerate_like_cardinals(void) {
    struct fixture cardinal;
    struct fixture diagonal;
    fixture_init(&cardinal, &accelerating);
    fixture_init(&diagonal, &accelerating);
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
    fixture_init(&positive, &accelerating);
    fixture_init(&negative, &accelerating);
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
    fixture_init(&fixture, &accelerating);
    const struct zpt_signal *out = push(&fixture, 0, INT64_MAX / 2, INT64_MIN / 2);
    assert(out->metadata.flags & ZPT_SIGNAL_FLAG_CLIPPED);
    assert(out->data.fixed_vector.x == INT64_MAX);
    assert(out->data.fixed_vector.y == INT64_MIN);
}

static void test_reset_forgets_speed(void) {
    struct fixture fixture;
    fixture_init(&fixture, &accelerating);
    for (uint32_t t = 0; t <= 96; t += 8) {
        push(&fixture, t, MM(3), 0);
    }
    zpt_pipeline_reset(&fixture.pipeline, ZPT_RESET_PIPELINE_LEFT);
    assert(fixture.state.window_distance == 0 && !fixture.state.have_last_frame);
}

int main(void) {
    test_gain_follows_the_ramp();
    test_validation();
    test_unit_gains_are_identity();
    test_speed_estimate_tracks_steady_motion();
    test_motion_after_a_pause_starts_slow();
    test_diagonals_accelerate_like_cardinals();
    test_negative_motion_scales_symmetrically();
    test_saturation_is_flagged();
    test_reset_forgets_speed();
    puts("cursor acceleration tests passed");
    return 0;
}
