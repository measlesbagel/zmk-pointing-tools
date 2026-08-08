/* SPDX-License-Identifier: MIT */

#include <assert.h>
#include <stdio.h>

#include <zmk/pointing_tools/legacy_processor/text_nav.h>

static const struct zpt_text_nav_settings settings = {
    .horizontal_threshold = 25,
    .vertical_threshold = 50,
    .idle_timeout_ms = 120,
    .activation_distance = 12,
    .engage_ratio_percent = 150,
};

static void test_independent_thresholds(void) {
    struct zpt_text_nav_state state;
    zpt_text_nav_reset(&state);

    assert(zpt_text_nav_update(&state, &settings, 12, 1, 0, false) == ZPT_TEXT_NAV_NONE);
    assert(zpt_text_nav_update(&state, &settings, 13, 0, 8, true) == ZPT_TEXT_NAV_RIGHT);
    assert(zpt_text_nav_update(&state, &settings, 50, 0, 8, true) == ZPT_TEXT_NAV_RIGHT);
    /* At most one step is returned per physical frame; the remainder survives. */
    assert(zpt_text_nav_update(&state, &settings, 0, 0, 8, true) == ZPT_TEXT_NAV_RIGHT);

    zpt_text_nav_reset(&state);
    assert(zpt_text_nav_update(&state, &settings, 1, -49, 0, false) == ZPT_TEXT_NAV_NONE);
    assert(zpt_text_nav_update(&state, &settings, 0, -1, 8, true) == ZPT_TEXT_NAV_UP);
}

static void test_axis_lock_and_idle_reset(void) {
    struct zpt_text_nav_state state;
    zpt_text_nav_reset(&state);

    assert(zpt_text_nav_update(&state, &settings, -25, 2, 0, false) == ZPT_TEXT_NAV_LEFT);
    assert(state.intent == ZPT_AXIS_INTENT_HORIZONTAL);
    assert(zpt_text_nav_update(&state, &settings, 0, 100, 8, true) == ZPT_TEXT_NAV_NONE);
    assert(state.intent == ZPT_AXIS_INTENT_HORIZONTAL);

    assert(zpt_text_nav_update(&state, &settings, 0, 50, settings.idle_timeout_ms, true) ==
           ZPT_TEXT_NAV_DOWN);
    assert(state.intent == ZPT_AXIS_INTENT_VERTICAL);
}

static void test_diagonal_waits_for_intent(void) {
    struct zpt_text_nav_state state;
    zpt_text_nav_reset(&state);

    assert(zpt_text_nav_update(&state, &settings, 10, 9, 0, false) == ZPT_TEXT_NAV_NONE);
    assert(state.intent == ZPT_AXIS_INTENT_UNDECIDED);
    assert(zpt_text_nav_update(&state, &settings, 20, 1, 8, true) == ZPT_TEXT_NAV_RIGHT);
    assert(state.intent == ZPT_AXIS_INTENT_HORIZONTAL);
}

int main(void) {
    test_independent_thresholds();
    test_axis_lock_and_idle_reset();
    test_diagonal_waits_for_intent();
    puts("text navigation tests passed");
    return 0;
}
