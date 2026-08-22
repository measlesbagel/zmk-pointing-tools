/* SPDX-License-Identifier: MIT */

/* Tests for the devicetree-declared physical pointing device table
 * (src/platform/zmk/device_table.c). The registry is fully const after
 * boot, so these cases are order-independent within the shared zpt_unit
 * suite: nothing mutates process-wide state. */

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/ztest.h>

#include <zmk/pointing_tools/platform/zmk/device_table.h>
#include <zmk/pointing_tools/source/device_caps.h>

ZTEST(zpt_unit, device_table_discovers_all_entries_in_dt_order) {
    zassert_equal(zpt_device_table_count(), 3);

    const struct zpt_pointing_device *local = zpt_device_table_at(0);
    const struct zpt_pointing_device *remote = zpt_device_table_at(1);
    const struct zpt_pointing_device *fixed = zpt_device_table_at(2);

    /* Dense ids in devicetree order, matching the overlay. */
    zassert_equal(local->id, 0);
    zassert_equal(remote->id, 1);
    zassert_equal(fixed->id, 2);

    zassert_str_equal(local->stable_id, "test-local-trackball");
    zassert_str_equal(remote->stable_id, "test-remote-trackball");
    zassert_str_equal(fixed->stable_id, "test-fixed-numpad");

    /* Out-of-bounds reads are NULL. */
    zassert_is_null(zpt_device_table_at(3));
}

ZTEST(zpt_unit, device_table_finds_entries_by_stable_id) {
    zassert_equal(zpt_device_table_find("test-local-trackball")->id, 0);
    zassert_equal(zpt_device_table_find("test-remote-trackball")->id, 1);
    zassert_equal(zpt_device_table_find("test-fixed-numpad")->id, 2);
    zassert_is_null(zpt_device_table_find("unknown-device"));
    zassert_is_null(zpt_device_table_find(NULL));
}

ZTEST(zpt_unit, device_local_entry_reports_discrete_capabilities) {
    const struct zpt_pointing_device *device = zpt_device_table_find("test-local-trackball");

    zassert_equal(device->location, 0);
    zassert_equal(device->default_cpi, 800);
    zassert_true(device->caps.settable);
    zassert_true(device->caps.discrete);
    zassert_equal(device->caps.list_count, 4);
    zassert_equal(device->caps.list_values[0], 200);
    zassert_equal(device->caps.list_values[1], 400);
    zassert_equal(device->caps.list_values[2], 800);
    zassert_equal(device->caps.list_values[3], 1600);
}

ZTEST(zpt_unit, device_remote_entry_reports_range_capabilities) {
    const struct zpt_pointing_device *device = zpt_device_table_find("test-remote-trackball");

    /* Peripheral one owns this entry. */
    zassert_equal(device->location, 1);
    zassert_equal(device->default_cpi, 400);
    zassert_true(device->caps.settable);
    zassert_false(device->caps.discrete);
    zassert_equal(device->caps.range_min, 100);
    zassert_equal(device->caps.range_max, 1200);
    zassert_equal(device->caps.range_step, 100);
}

ZTEST(zpt_unit, device_without_capabilities_is_read_only) {
    const struct zpt_pointing_device *device = zpt_device_table_find("test-fixed-numpad");

    zassert_false(device->caps.settable);
    uint16_t effective = 0;
    zassert_equal(zpt_cpi_validate(&device->caps, device->default_cpi, &effective), -ENOSYS);
}

ZTEST(zpt_unit, device_entry_validates_previews_against_capabilities) {
    const struct zpt_pointing_device *local = zpt_device_table_find("test-local-trackball");
    uint16_t effective = 0;

    zassert_equal(zpt_cpi_validate(&local->caps, 800, &effective), 0);
    zassert_equal(effective, 800);
    /* 700 is nearer 800 than 400 on the declared list. */
    zassert_equal(zpt_cpi_validate(&local->caps, 700, &effective), 1);
    zassert_equal(effective, 800);

    const struct zpt_pointing_device *remote = zpt_device_table_find("test-remote-trackball");
    /* 450 is equidistant from 400 and 500; ties snap toward the lower one. */
    zassert_equal(zpt_cpi_validate(&remote->caps, 450, &effective), 1);
    zassert_equal(effective, 400);
}

/* The cases below mutate control state; each restores what it changed so
 * suite order stays irrelevant even though they sort last. */

ZTEST(zpt_unit, zz_device_control_get_reports_compiled_defaults) {
    uint16_t cpi = 0;

    zassert_ok(zpt_device_control_get(zpt_device_table_find("test-local-trackball"), &cpi));
    zassert_equal(cpi, 800);
    zassert_ok(zpt_device_control_get(zpt_device_table_find("test-remote-trackball"), &cpi));
    zassert_equal(cpi, 400);
    /* Read-only devices still report their compiled value. */
    zassert_ok(zpt_device_control_get(zpt_device_table_find("test-fixed-numpad"), &cpi));
    zassert_equal(cpi, 1200);

    zassert_equal(zpt_device_control_get(NULL, &cpi), -EINVAL);
    zassert_equal(zpt_device_control_get(zpt_device_table_at(99), &cpi), -EINVAL);
    zassert_equal(zpt_device_control_get(zpt_device_table_find("test-local-trackball"), NULL),
                  -EINVAL);
}

ZTEST(zpt_unit, zz_device_control_previews_snap_persist_and_reset) {
    const struct zpt_pointing_device *local = zpt_device_table_find("test-local-trackball");
    const struct zpt_pointing_device *fixed = zpt_device_table_find("test-fixed-numpad");
    const struct zpt_pointing_device *remote = zpt_device_table_find("test-remote-trackball");
    uint16_t effective = 0;
    uint16_t cpi = 0;

    /* Exact request applies as-is. */
    zassert_equal(zpt_device_control_preview(local, 400, &effective), 0);
    zassert_equal(effective, 400);
    zassert_ok(zpt_device_control_get(local, &cpi));
    zassert_equal(cpi, 400);

    /* Off-list requests snap to the nearest supported value. */
    zassert_equal(zpt_device_control_preview(local, 1000, &effective), 1);
    zassert_equal(effective, 800);
    zassert_ok(zpt_device_control_get(local, &cpi));
    zassert_equal(cpi, 800);

    /* Range entries clamp to their bounds. */
    zassert_equal(zpt_device_control_preview(remote, UINT16_MAX, &effective), 1);
    zassert_equal(effective, 1200);

    /* Read-only entries reject previews but keep reporting their default. */
    zassert_equal(zpt_device_control_preview(fixed, 800, &effective), -ENOSYS);
    zassert_ok(zpt_device_control_get(fixed, &cpi));
    zassert_equal(cpi, 1200);

    /* Reset restores exactly one entry's compiled value. */
    zassert_ok(zpt_device_control_reset(local));
    zassert_ok(zpt_device_control_get(local, &cpi));
    zassert_equal(cpi, 800);
    zassert_ok(zpt_device_control_reset(remote));
    zassert_ok(zpt_device_control_get(remote, &cpi));
    zassert_equal(cpi, 400);

    zassert_equal(zpt_device_control_reset(fixed), -ENOSYS);
    zassert_equal(zpt_device_control_reset(NULL), -EINVAL);
}
