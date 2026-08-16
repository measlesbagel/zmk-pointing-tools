/* SPDX-License-Identifier: MIT */

#include <assert.h>
#include <errno.h>
#include <stdio.h>

#include <zmk/pointing_tools/core/router.h>

#define ARRAY_SIZE(values) (sizeof(values) / sizeof((values)[0]))

struct sink_state {
    uint32_t outputs;
    uint32_t activates;
    uint32_t deactivates;
    uint32_t resets;
    bool fail_activation;
};

static int sink_emit(struct zpt_sink *sink, const struct zpt_signal *signal) {
    (void)signal;
    ((struct sink_state *)sink->state)->outputs++;
    return 0;
}

static int sink_activate(struct zpt_sink *sink, enum zpt_reset_reason reason) {
    (void)reason;
    struct sink_state *state = sink->state;
    state->activates++;
    return state->fail_activation ? -EIO : 0;
}

static int sink_deactivate(struct zpt_sink *sink, enum zpt_reset_reason reason) {
    (void)reason;
    ((struct sink_state *)sink->state)->deactivates++;
    return 0;
}

static void sink_reset(struct zpt_sink *sink, enum zpt_reset_reason reason) {
    (void)reason;
    ((struct sink_state *)sink->state)->resets++;
}

static const struct zpt_sink_api raw_sink_api = {
    .type_id = "raw-capture",
    .accepted_kinds = ZPT_SIGNAL_KIND_MASK(ZPT_SIGNAL_RAW_MOTION),
    .emit = sink_emit,
    .activate = sink_activate,
    .deactivate = sink_deactivate,
    .reset = sink_reset,
};

struct buffered_state {
    bool pending;
    uint32_t resets;
};

static int buffered_process(struct zpt_stage *stage, const struct zpt_signal *signal,
                            struct zpt_stage_context *context) {
    (void)signal;
    struct buffered_state *state = stage->state;
    state->pending = true;
    return zpt_stage_schedule_flush(context, zpt_stage_now_ms(context) + 20U);
}

static int buffered_flush(struct zpt_stage *stage, uint32_t now_ms,
                          struct zpt_stage_context *context) {
    (void)now_ms;
    (void)context;
    ((struct buffered_state *)stage->state)->pending = false;
    return 0;
}

static void buffered_reset(struct zpt_stage *stage, enum zpt_reset_reason reason) {
    (void)reason;
    struct buffered_state *state = stage->state;
    state->pending = false;
    state->resets++;
}

static const struct zpt_stage_api buffered_api = {
    .strategy_id = "test-buffer",
    .accepted_kinds = ZPT_SIGNAL_KIND_MASK(ZPT_SIGNAL_RAW_MOTION),
    .output_kind = ZPT_SIGNAL_RAW_MOTION,
    .flags = ZPT_STAGE_STATEFUL,
    .process = buffered_process,
    .flush = buffered_flush,
    .reset = buffered_reset,
};

struct router_fixture {
    struct sink_state sink_states[2];
    struct zpt_sink sinks[2];
    struct zpt_pipeline pipeline_storage[2];
    struct zpt_pipeline *pipelines[2];
    struct zpt_router router;
};

static void router_fixture_init(struct router_fixture *fixture) {
    *fixture = (struct router_fixture){0};
    fixture->sinks[0] = (struct zpt_sink){
        .stable_id = "first-sink",
        .api = &raw_sink_api,
        .state = &fixture->sink_states[0],
    };
    fixture->sinks[1] = (struct zpt_sink){
        .stable_id = "second-sink",
        .api = &raw_sink_api,
        .state = &fixture->sink_states[1],
    };
    fixture->pipeline_storage[0] = (struct zpt_pipeline){
        .stable_id = "first",
        .input_kind = ZPT_SIGNAL_RAW_MOTION,
        .sink = &fixture->sinks[0],
        .dispatch_budget = 4,
    };
    fixture->pipeline_storage[1] = (struct zpt_pipeline){
        .stable_id = "second",
        .input_kind = ZPT_SIGNAL_RAW_MOTION,
        .sink = &fixture->sinks[1],
        .dispatch_budget = 4,
    };
    fixture->pipelines[0] = &fixture->pipeline_storage[0];
    fixture->pipelines[1] = &fixture->pipeline_storage[1];
    fixture->router = (struct zpt_router){
        .stable_id = "right-router",
        .input_kind = ZPT_SIGNAL_RAW_MOTION,
        .pipelines = fixture->pipelines,
        .pipeline_count = ARRAY_SIZE(fixture->pipelines),
        .default_pipeline_index = 0,
    };
}

static struct zpt_signal raw_signal(uint32_t now_ms) {
    return (struct zpt_signal){
        .kind = ZPT_SIGNAL_RAW_MOTION,
        .metadata = {.observed_at_ms = now_ms},
        .data.raw_motion = {.x_counts = 1, .y_counts = -1},
    };
}

static void test_routes_input_and_applies_lifecycle(void) {
    struct router_fixture fixture;
    router_fixture_init(&fixture);
    assert(zpt_router_validate(&fixture.router) == 0);
    assert(zpt_router_validate(&fixture.router) == -EALREADY);
    assert(zpt_router_activate(&fixture.router, ZPT_RESET_PIPELINE_ENTERED) == 0);
    assert(zpt_router_active_pipeline(&fixture.router) == &fixture.pipeline_storage[0]);
    assert(fixture.sink_states[0].activates == 1);

    struct zpt_pipeline_result result;
    struct zpt_signal signal = raw_signal(10);
    assert(zpt_router_push(&fixture.router, &signal, &result) == 0);
    assert(result.outputs == 1 && fixture.sink_states[0].outputs == 1);

    assert(zpt_router_select(&fixture.router, 1, 20, &result) == 0);
    assert(zpt_router_active_pipeline(&fixture.router) == &fixture.pipeline_storage[1]);
    assert(fixture.sink_states[0].deactivates == 1);
    assert(fixture.sink_states[1].activates == 1);
    assert(zpt_router_select(&fixture.router, 1, 21, &result) == 0);
    assert(fixture.sink_states[1].activates == 1);

    signal = raw_signal(22);
    assert(zpt_router_push(&fixture.router, &signal, &result) == 0);
    assert(fixture.sink_states[1].outputs == 1);
    assert(zpt_router_deactivate(&fixture.router, 30, ZPT_RESET_ADMINISTRATIVE, &result) == 0);
    assert(zpt_router_active_pipeline(&fixture.router) == NULL);
    assert(zpt_router_push(&fixture.router, &signal, &result) == -EACCES);
}

static void test_route_change_cancels_inactive_deadlines(void) {
    struct router_fixture fixture;
    router_fixture_init(&fixture);
    struct buffered_state buffer = {0};
    struct zpt_stage buffered = {
        .stable_id = "buffer",
        .api = &buffered_api,
        .state = &buffer,
    };
    struct zpt_stage *stages[] = {&buffered};
    fixture.pipeline_storage[0].stages = stages;
    fixture.pipeline_storage[0].stage_count = ARRAY_SIZE(stages);

    assert(zpt_router_validate(&fixture.router) == 0);
    assert(zpt_router_activate(&fixture.router, ZPT_RESET_PIPELINE_ENTERED) == 0);
    struct zpt_pipeline_result result;
    struct zpt_signal signal = raw_signal(100);
    assert(zpt_router_push(&fixture.router, &signal, &result) == 0);
    uint32_t deadline;
    assert(zpt_router_next_deadline(&fixture.router, 100, &deadline) && deadline == 120);
    assert(buffer.pending);

    assert(zpt_router_select(&fixture.router, 1, 110, &result) == 0);
    assert(!buffer.pending);
    assert(!zpt_router_next_deadline(&fixture.router, 110, &deadline));
}

static void test_failed_route_can_recover(void) {
    struct router_fixture fixture;
    router_fixture_init(&fixture);
    fixture.sink_states[1].fail_activation = true;
    assert(zpt_router_validate(&fixture.router) == 0);
    assert(zpt_router_activate(&fixture.router, ZPT_RESET_PIPELINE_ENTERED) == 0);

    struct zpt_pipeline_result result;
    assert(zpt_router_select(&fixture.router, 1, 20, &result) == -EIO);
    assert(zpt_router_active_pipeline(&fixture.router) == NULL);
    fixture.sink_states[1].fail_activation = false;
    assert(zpt_router_select(&fixture.router, 1, 21, &result) == 0);
    assert(zpt_router_active_pipeline(&fixture.router) == &fixture.pipeline_storage[1]);
}

static void test_validation_rejects_invalid_composition(void) {
    struct router_fixture fixture;
    router_fixture_init(&fixture);
    fixture.pipeline_storage[1].stable_id = "first";
    assert(zpt_router_validate(&fixture.router) == -EEXIST);

    router_fixture_init(&fixture);
    fixture.pipeline_storage[1].input_kind = ZPT_SIGNAL_NORMALIZED_MOTION;
    assert(zpt_router_validate(&fixture.router) == -EINVAL);

    router_fixture_init(&fixture);
    assert(zpt_pipeline_validate(&fixture.pipeline_storage[0]) == 0);
    assert(zpt_router_validate(&fixture.router) == -EBUSY);
}

int main(void) {
    test_routes_input_and_applies_lifecycle();
    test_route_change_cancels_inactive_deadlines();
    test_failed_route_can_recover();
    test_validation_rejects_invalid_composition();
    puts("router tests passed");
    return 0;
}
