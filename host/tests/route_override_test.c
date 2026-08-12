/* SPDX-License-Identifier: MIT */

#include <assert.h>
#include <errno.h>
#include <stdio.h>

#include <zmk/pointing_tools/core/route_override.h>

#define ARRAY_SIZE(values) (sizeof(values) / sizeof((values)[0]))

static void test_press_selects_and_release_restores(void) {
    struct zpt_route_override storage[4];
    struct zpt_route_override_table table;
    zpt_route_override_table_init(&table, storage, ARRAY_SIZE(storage));

    size_t pipeline_index;
    assert(!zpt_route_override_selected(&table, &pipeline_index));

    assert(zpt_route_override_press(&table, 1, 2) == 0);
    assert(zpt_route_override_selected(&table, &pipeline_index));
    assert(pipeline_index == 2);

    assert(zpt_route_override_release(&table, 1) == 0);
    assert(!zpt_route_override_selected(&table, &pipeline_index));
}

static void test_newest_press_wins_and_release_reveals_previous(void) {
    struct zpt_route_override storage[4];
    struct zpt_route_override_table table;
    zpt_route_override_table_init(&table, storage, ARRAY_SIZE(storage));

    size_t pipeline_index;
    assert(zpt_route_override_press(&table, 1, 2) == 0);
    assert(zpt_route_override_press(&table, 5, 3) == 0);
    assert(zpt_route_override_selected(&table, &pipeline_index));
    assert(pipeline_index == 3);

    assert(zpt_route_override_release(&table, 5) == 0);
    assert(zpt_route_override_selected(&table, &pipeline_index));
    assert(pipeline_index == 2);

    assert(zpt_route_override_release(&table, 1) == 0);
    assert(!zpt_route_override_selected(&table, &pipeline_index));
}

static void test_release_without_override_is_ok(void) {
    struct zpt_route_override storage[4];
    struct zpt_route_override_table table;
    zpt_route_override_table_init(&table, storage, ARRAY_SIZE(storage));

    assert(zpt_route_override_release(&table, 7) == 0);
}

static void test_press_replaces_same_position_and_refreshes_order(void) {
    struct zpt_route_override storage[4];
    struct zpt_route_override_table table;
    zpt_route_override_table_init(&table, storage, ARRAY_SIZE(storage));

    size_t pipeline_index;
    assert(zpt_route_override_press(&table, 1, 2) == 0);
    assert(zpt_route_override_press(&table, 1, 3) == 0);
    assert(zpt_route_override_selected(&table, &pipeline_index));
    assert(pipeline_index == 3);

    assert(zpt_route_override_release(&table, 1) == 0);
    assert(!zpt_route_override_selected(&table, &pipeline_index));
}

static void test_capacity_exhausted_returns_enospc(void) {
    struct zpt_route_override storage[2];
    struct zpt_route_override_table table;
    zpt_route_override_table_init(&table, storage, ARRAY_SIZE(storage));

    assert(zpt_route_override_press(&table, 1, 2) == 0);
    assert(zpt_route_override_press(&table, 2, 3) == 0);
    assert(zpt_route_override_press(&table, 3, 0) == -ENOSPC);

    /* Releasing one slot makes room again. */
    assert(zpt_route_override_release(&table, 1) == 0);
    assert(zpt_route_override_press(&table, 3, 0) == 0);
    size_t pipeline_index;
    assert(zpt_route_override_selected(&table, &pipeline_index));
    assert(pipeline_index == 0);
}

static void test_rejects_null_table(void) {
    size_t pipeline_index;
    assert(zpt_route_override_press(NULL, 1, 2) == -EINVAL);
    assert(zpt_route_override_release(NULL, 1) == -EINVAL);
    assert(!zpt_route_override_selected(NULL, &pipeline_index));
}

int main(void) {
    test_press_selects_and_release_restores();
    test_newest_press_wins_and_release_reveals_previous();
    test_release_without_override_is_ok();
    test_press_replaces_same_position_and_refreshes_order();
    test_capacity_exhausted_returns_enospc();
    test_rejects_null_table();
    puts("route override tests passed");
    return 0;
}
