/* SPDX-License-Identifier: MIT */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include <zmk/pointing_tools/core/pipeline.h>
#include <zmk/pointing_tools/stage/cursor_coalescer.h>

#define MAX_REPORTS 16

struct capture_state {
    struct zpt_signal reports[MAX_REPORTS];
    uint32_t count;
};

static int capture_emit(struct zpt_sink *sink, const struct zpt_signal *signal) {
    struct capture_state *capture = sink->state;
    assert(capture->count < MAX_REPORTS);
    capture->reports[capture->count++] = *signal;
    return 0;
}

static const struct zpt_sink_api capture_api = {
    .type_id = "pointer-capture",
    .accepted_kinds = ZPT_SIGNAL_KIND_MASK(ZPT_SIGNAL_POINTER_DELTA),
    .emit = capture_emit,
};

struct fixture {
    struct zpt_cursor_coalescer_config config;
    struct zpt_cursor_coalescer_state state;
    struct zpt_stage stage;
    struct zpt_stage *stages[1];
    struct capture_state capture;
    struct zpt_sink sink;
    struct zpt_pipeline pipeline;
};

static void fixture_init(struct fixture *fixture, uint16_t interval_ms) {
    *fixture = (struct fixture){.config = {.report_interval_ms = interval_ms}};
    fixture->stage = (struct zpt_stage){
        .stable_id = "cursor-coalescer",
        .api = &zpt_cursor_coalescer_stage_api,
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
        .stable_id = "coalescer-test",
        .input_kind = ZPT_SIGNAL_POINTER_DELTA,
        .stages = fixture->stages,
        .stage_count = 1,
        .sink = &fixture->sink,
        .dispatch_budget = 2,
    };
    assert(zpt_pipeline_validate(&fixture->pipeline) == 0);
    assert(zpt_pipeline_activate(&fixture->pipeline, ZPT_RESET_PIPELINE_ENTERED) == 0);
}

static void push(struct fixture *fixture, uint32_t now_ms, int32_t x, int32_t y) {
    struct zpt_signal signal = {
        .kind = ZPT_SIGNAL_POINTER_DELTA,
        .metadata = {.observed_at_ms = now_ms, .source_id = 3, .resolution_cpi = 684},
        .data.delta = {.x = x, .y = y},
    };
    struct zpt_pipeline_result result;
    assert(zpt_pipeline_push(&fixture->pipeline, &signal, &result) == 0);
}

/* Run the pipeline's deadline, if any is due by now_ms. */
static void advance(struct fixture *fixture, uint32_t now_ms) {
    struct zpt_pipeline_result result;
    assert(zpt_pipeline_flush(&fixture->pipeline, now_ms, &result) == 0);
}

static void test_leading_frame_goes_out_at_once(void) {
    struct fixture fixture;
    fixture_init(&fixture, 8);
    push(&fixture, 100, 3, -2);
    assert(fixture.capture.count == 1);
    assert(fixture.capture.reports[0].data.delta.x == 3);
    assert(fixture.capture.reports[0].data.delta.y == -2);
    assert(fixture.capture.reports[0].metadata.source_id == 3);
    assert((fixture.capture.reports[0].metadata.flags & ZPT_SIGNAL_FLAG_COALESCED) == 0);
}

static void test_frames_within_an_interval_are_summed(void) {
    struct fixture fixture;
    fixture_init(&fixture, 8);
    push(&fixture, 100, 1, 0);
    push(&fixture, 101, 2, 1);
    push(&fixture, 103, 4, -3);
    assert(fixture.capture.count == 1);

    uint32_t deadline;
    assert(zpt_pipeline_next_deadline(&fixture.pipeline, 103, &deadline));
    assert(deadline == 108);
    advance(&fixture, 107);
    assert(fixture.capture.count == 1);
    advance(&fixture, 108);
    assert(fixture.capture.count == 2);
    assert(fixture.capture.reports[1].data.delta.x == 6);
    assert(fixture.capture.reports[1].data.delta.y == -2);
    assert(fixture.capture.reports[1].metadata.observed_at_ms == 108);
    assert(fixture.capture.reports[1].metadata.flags & ZPT_SIGNAL_FLAG_COALESCED);

    /* After a quiet interval the next frame is a leading frame again. */
    advance(&fixture, 116);
    push(&fixture, 130, 5, 5);
    assert(fixture.capture.count == 3);
    assert(fixture.capture.reports[2].data.delta.x == 5);
}

/* Steady fast motion produces one report per interval and loses nothing. */
static void test_steady_motion_is_rate_limited_without_loss(void) {
    struct fixture fixture;
    fixture_init(&fixture, 8);
    int64_t sent_x = 0;
    for (uint32_t now = 0; now < 64; now++) {
        advance(&fixture, now);
        push(&fixture, now, 2, 0);
    }
    advance(&fixture, 72);
    for (uint32_t i = 0; i < fixture.capture.count; i++) {
        sent_x += fixture.capture.reports[i].data.delta.x;
        if (i > 0) {
            assert(fixture.capture.reports[i].metadata.observed_at_ms -
                       fixture.capture.reports[i - 1].metadata.observed_at_ms >=
                   8);
        }
    }
    assert(sent_x == 128);
    assert(fixture.capture.count == 9);
}

/* A sum beyond the HID range is sent over several reports. */
static void test_large_sums_split_across_reports(void) {
    struct fixture fixture;
    fixture_init(&fixture, 8);
    push(&fixture, 0, 1, 1);
    push(&fixture, 2, 30000, -30000);
    push(&fixture, 4, 30000, -30000);
    advance(&fixture, 8);
    advance(&fixture, 16);
    assert(fixture.capture.count == 3);
    assert(fixture.capture.reports[1].data.delta.x == INT16_MAX);
    assert(fixture.capture.reports[1].data.delta.y == INT16_MIN);
    assert(fixture.capture.reports[2].data.delta.x == 60000 - INT16_MAX);
    assert(fixture.capture.reports[2].data.delta.y == -60000 - INT16_MIN);
    assert(fixture.state.pending_x == 0 && fixture.state.pending_y == 0);
}

static void test_zero_frames_send_nothing(void) {
    struct fixture fixture;
    fixture_init(&fixture, 8);
    push(&fixture, 0, 0, 0);
    push(&fixture, 20, 0, 0);
    uint32_t deadline;
    assert(fixture.capture.count == 0);
    assert(!zpt_pipeline_next_deadline(&fixture.pipeline, 20, &deadline));
}

static void test_zero_interval_passes_every_frame(void) {
    struct fixture fixture;
    fixture_init(&fixture, 0);
    push(&fixture, 0, 1, 0);
    push(&fixture, 0, 2, 0);
    push(&fixture, 1, 3, 0);
    assert(fixture.capture.count == 3);
    assert(fixture.capture.reports[2].data.delta.x == 3);
}

static void test_reset_drops_pending_motion(void) {
    struct fixture fixture;
    fixture_init(&fixture, 8);
    push(&fixture, 0, 1, 0);
    push(&fixture, 2, 9, 0);
    zpt_pipeline_reset(&fixture.pipeline, ZPT_RESET_PIPELINE_LEFT);
    assert(fixture.state.pending_x == 0 && !fixture.state.have_report && !fixture.state.armed);
    push(&fixture, 3, 4, 0);
    assert(fixture.capture.count == 2);
    assert(fixture.capture.reports[1].data.delta.x == 4);
}

int main(void) {
    test_leading_frame_goes_out_at_once();
    test_frames_within_an_interval_are_summed();
    test_steady_motion_is_rate_limited_without_loss();
    test_large_sums_split_across_reports();
    test_zero_frames_send_nothing();
    test_zero_interval_passes_every_frame();
    test_reset_drops_pending_motion();
    puts("cursor coalescer tests passed");
    return 0;
}
