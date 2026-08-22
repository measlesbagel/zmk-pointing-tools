/* SPDX-License-Identifier: MIT */

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>

#include <zmk/pointing_tools/source/device_caps.h>

static void test_null_and_unsettable_are_rejected(void) {
    uint16_t effective = 0;

    assert(zpt_cpi_validate(NULL, 800, &effective) == -ENOSYS);

    const struct zpt_cpi_capabilities read_only = {.settable = false};
    assert(zpt_cpi_validate(&read_only, 800, &effective) == -ENOSYS);
}

static void test_malformed_capabilities_are_rejected(void) {
    const uint16_t no_values[1] = {800};
    const struct zpt_cpi_capabilities empty_list = {
        .settable = true,
        .discrete = true,
        .list_values = NULL,
        .list_count = 1,
    };
    const struct zpt_cpi_capabilities zero_length_list = {
        .settable = true,
        .discrete = true,
        .list_values = no_values,
        .list_count = 0,
    };
    const struct zpt_cpi_capabilities inverted_range = {
        .settable = true,
        .discrete = false,
        .range_min = 1200,
        .range_max = 400,
        .range_step = 100,
    };
    const struct zpt_cpi_capabilities zero_step_range = {
        .settable = true,
        .discrete = false,
        .range_min = 400,
        .range_max = 1200,
        .range_step = 0,
    };
    uint16_t effective = 0;

    assert(zpt_cpi_validate(&empty_list, 800, &effective) == -EINVAL);
    assert(zpt_cpi_validate(&zero_length_list, 800, &effective) == -EINVAL);
    assert(zpt_cpi_validate(&inverted_range, 800, &effective) == -EINVAL);
    assert(zpt_cpi_validate(&zero_step_range, 800, &effective) == -EINVAL);
}

static void test_discrete_exact_matches(void) {
    static const uint16_t values[] = {200, 400, 800, 1600};
    const struct zpt_cpi_capabilities caps = {
        .settable = true,
        .discrete = true,
        .list_values = values,
        .list_count = 4,
    };
    uint16_t effective = 0;

    for (size_t i = 0; i < 4; i++) {
        assert(zpt_cpi_validate(&caps, values[i], &effective) == 0);
        assert(effective == values[i]);
    }
}

static void test_discrete_snaps_to_nearest_with_lower_tiebreak(void) {
    static const uint16_t values[] = {200, 400, 800, 1600};
    const struct zpt_cpi_capabilities caps = {
        .settable = true,
        .discrete = true,
        .list_values = values,
        .list_count = 4,
    };
    uint16_t effective = 0;

    /* Below the first value. */
    assert(zpt_cpi_validate(&caps, 1, &effective) == 1);
    assert(effective == 200);
    /* Between entries: closer to the upper one. */
    assert(zpt_cpi_validate(&caps, 700, &effective) == 1);
    assert(effective == 800);
    /* Exact midpoint snaps toward the lower value. */
    assert(zpt_cpi_validate(&caps, 300, &effective) == 1);
    assert(effective == 200);
    /* Above the last value. */
    assert(zpt_cpi_validate(&caps, UINT16_MAX, &effective) == 1);
    assert(effective == 1600);
}

static void test_range_exact_matches_on_lattice_points(void) {
    const struct zpt_cpi_capabilities caps = {
        .settable = true,
        .discrete = false,
        .range_min = 100,
        .range_max = 1200,
        .range_step = 100,
    };
    uint16_t effective = 0;

    for (uint16_t cpi = 100; cpi <= 1200; cpi += 100) {
        assert(zpt_cpi_validate(&caps, cpi, &effective) == 0);
        assert(effective == cpi);
    }
}

static void test_range_clamps_and_snaps_off_lattice_requests(void) {
    const struct zpt_cpi_capabilities caps = {
        .settable = true,
        .discrete = false,
        .range_min = 100,
        .range_max = 1200,
        .range_step = 100,
    };
    uint16_t effective = 0;

    /* Below the minimum clamps up. */
    assert(zpt_cpi_validate(&caps, 1, &effective) == 1);
    assert(effective == 100);
    /* Above the maximum clamps down. */
    assert(zpt_cpi_validate(&caps, UINT16_MAX, &effective) == 1);
    assert(effective == 1200);
    /* Interior off-lattice requests snap to the nearer point. */
    assert(zpt_cpi_validate(&caps, 149, &effective) == 1);
    assert(effective == 100);
    assert(zpt_cpi_validate(&caps, 151, &effective) == 1);
    assert(effective == 200);
    /* Midpoint between lattice points snaps down. */
    assert(zpt_cpi_validate(&caps, 550, &effective) == 1);
    assert(effective == 500);
}

static void test_single_point_range_is_always_exact(void) {
    const struct zpt_cpi_capabilities caps = {
        .settable = true,
        .discrete = false,
        .range_min = 800,
        .range_max = 800,
        .range_step = 1,
    };
    uint16_t effective = 0;

    assert(zpt_cpi_validate(&caps, 800, &effective) == 0);
    assert(effective == 800);
    assert(zpt_cpi_validate(&caps, 500, &effective) == 1);
    assert(effective == 800);
}

int main(void) {
    test_null_and_unsettable_are_rejected();
    test_malformed_capabilities_are_rejected();
    test_discrete_exact_matches();
    test_discrete_snaps_to_nearest_with_lower_tiebreak();
    test_range_exact_matches_on_lattice_points();
    test_range_clamps_and_snaps_off_lattice_requests();
    test_single_point_range_is_always_exact();
    puts("device capability tests passed");
    return 0;
}
