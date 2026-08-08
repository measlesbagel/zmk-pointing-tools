/* SPDX-License-Identifier: MIT */

#include <assert.h>
#include <stdio.h>

#include <zmk/pointing_tools/legacy_processor/axis_intent.h>

static const struct zpt_axis_intent_config config = {
    .engage_ratio_percent = 220,
    .release_ratio_percent = 150,
    .activation_distance = 16,
    .window_ms = 64,
};

static void test_forced_policies(void) {
    struct zpt_axis_intent_state state = {0};
    assert(zpt_axis_intent_update(&state, &config, ZPT_AXIS_POLICY_FREE, 20, 1, 8) ==
           ZPT_AXIS_INTENT_FREE);
    assert(zpt_axis_intent_update(&state, &config, ZPT_AXIS_POLICY_HORIZONTAL, 0, 20, 8) ==
           ZPT_AXIS_INTENT_HORIZONTAL);
    assert(zpt_axis_intent_update(&state, &config, ZPT_AXIS_POLICY_VERTICAL, 20, 0, 8) ==
           ZPT_AXIS_INTENT_VERTICAL);
}

static void test_cardinal_and_diagonal_classification(void) {
    struct zpt_axis_intent_state state;
    zpt_axis_intent_reset(&state);

    assert(zpt_axis_intent_update(&state, &config, ZPT_AXIS_POLICY_ADAPTIVE, 10, 2, 8) ==
           ZPT_AXIS_INTENT_UNDECIDED);
    assert(zpt_axis_intent_update(&state, &config, ZPT_AXIS_POLICY_ADAPTIVE, 10, 2, 8) ==
           ZPT_AXIS_INTENT_HORIZONTAL);

    zpt_axis_intent_reset(&state);
    assert(zpt_axis_intent_update(&state, &config, ZPT_AXIS_POLICY_ADAPTIVE, 4, 12, 8) ==
           ZPT_AXIS_INTENT_VERTICAL);

    zpt_axis_intent_reset(&state);
    assert(zpt_axis_intent_update(&state, &config, ZPT_AXIS_POLICY_ADAPTIVE, 9, 8, 8) ==
           ZPT_AXIS_INTENT_FREE);
}

static void test_hysteresis_and_turning(void) {
    struct zpt_axis_intent_state state;
    zpt_axis_intent_reset(&state);

    zpt_axis_intent_update(&state, &config, ZPT_AXIS_POLICY_ADAPTIVE, 20, 2, 8);
    assert(state.intent == ZPT_AXIS_INTENT_HORIZONTAL);

    /* One noisy frame does not immediately break a stable horizontal lock. */
    zpt_axis_intent_update(&state, &config, ZPT_AXIS_POLICY_ADAPTIVE, 7, 6, 8);
    assert(state.intent == ZPT_AXIS_INTENT_HORIZONTAL);

    /* A sustained turn eventually releases and then selects vertical. */
    for (int i = 0; i < 8; i++) {
        zpt_axis_intent_update(&state, &config, ZPT_AXIS_POLICY_ADAPTIVE, 1, 14, 8);
    }
    assert(state.intent == ZPT_AXIS_INTENT_VERTICAL);
}

static void test_window_expires(void) {
    struct zpt_axis_intent_state state;
    zpt_axis_intent_reset(&state);
    zpt_axis_intent_update(&state, &config, ZPT_AXIS_POLICY_ADAPTIVE, 20, 1, 8);
    assert(state.intent == ZPT_AXIS_INTENT_HORIZONTAL);

    zpt_axis_intent_reset(&state);
    assert(zpt_axis_intent_update(&state, &config, ZPT_AXIS_POLICY_ADAPTIVE, 1, 20, 80) ==
           ZPT_AXIS_INTENT_VERTICAL);
}

int main(void) {
    test_forced_policies();
    test_cardinal_and_diagonal_classification();
    test_hysteresis_and_turning();
    test_window_expires();
    puts("axis intent tests passed");
    return 0;
}
