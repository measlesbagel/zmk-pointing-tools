/* SPDX-License-Identifier: MIT */

#include <assert.h>
#include <stdio.h>

#include <zmk/pointing_tools/legacy_processor/noise_filter.h>

static const struct zpt_noise_filter_settings settings = {
    .enabled = true,
    .activation_distance = 6,
    .coherence_percent = 60,
    .qualification_timeout_ms = 160,
    .idle_timeout_ms = 120,
    .suppress_after_keypress_ms = 40,
};

static void test_fine_motion_accumulates(void) {
    struct zpt_noise_filter_state state = {0};
    zpt_noise_filter_reset(&state);
    struct zpt_noise_filter_result result;
    for (int index = 0; index < 5; index++) {
        result = zpt_noise_filter_update(&state, &settings, 1, 0, index * 8, false);
        assert(result.x == 0 && result.y == 0);
        assert(result.phase == ZPT_NOISE_FILTER_PENDING);
    }
    result = zpt_noise_filter_update(&state, &settings, 1, 0, 40, false);
    assert(result.qualified);
    assert(result.x == 6 && result.y == 0);
    result = zpt_noise_filter_update(&state, &settings, 1, -1, 48, false);
    assert(result.x == 1 && result.y == -1);
}

static void test_jitter_and_keypress_are_suppressed(void) {
    struct zpt_noise_filter_state state = {0};
    zpt_noise_filter_reset(&state);
    for (int index = 0; index < 12; index++) {
        int32_t x = index % 2 == 0 ? 2 : -2;
        struct zpt_noise_filter_result result =
            zpt_noise_filter_update(&state, &settings, x, 0, index * 8, false);
        assert(result.x == 0 && result.y == 0);
        assert(!result.qualified);
    }

    struct zpt_noise_filter_result result =
        zpt_noise_filter_update(&state, &settings, 4, 1, 100, true);
    assert(result.suppressed);
    assert(result.discarded);
    assert(state.sample_count == 0);
}

static void test_diagonal_is_unbiased(void) {
    struct zpt_noise_filter_state state = {0};
    zpt_noise_filter_reset(&state);
    struct zpt_noise_filter_result result;
    for (int index = 0; index < 4; index++) {
        result = zpt_noise_filter_update(&state, &settings, 1, 1, index * 8, false);
        assert(!result.qualified);
    }
    result = zpt_noise_filter_update(&state, &settings, 1, 1, 32, false);
    assert(result.qualified);
    assert(result.x == 5 && result.y == 5);
}

static void test_idle_requalifies_and_disabled_bypasses(void) {
    struct zpt_noise_filter_state state = {0};
    zpt_noise_filter_reset(&state);
    struct zpt_noise_filter_result result =
        zpt_noise_filter_update(&state, &settings, 6, 0, 0, false);
    assert(result.qualified);
    result = zpt_noise_filter_update(&state, &settings, 2, 0, 8, false);
    assert(result.x == 2);
    result = zpt_noise_filter_update(&state, &settings, 2, 0, 200, false);
    assert(result.reset_for_idle);
    assert(result.x == 0);

    struct zpt_noise_filter_settings disabled = settings;
    disabled.enabled = false;
    result = zpt_noise_filter_update(&state, &disabled, -2, 3, 208, false);
    assert(result.phase == ZPT_NOISE_FILTER_BYPASS);
    assert(result.x == -2 && result.y == 3);
}

int main(void) {
    test_fine_motion_accumulates();
    test_jitter_and_keypress_are_suppressed();
    test_diagonal_is_unbiased();
    test_idle_requalifies_and_disabled_bypasses();
    puts("noise filter tests passed");
    return 0;
}
