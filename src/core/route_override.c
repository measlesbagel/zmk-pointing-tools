/* SPDX-License-Identifier: MIT */

#include <errno.h>
#include <stddef.h>

#include <zmk/pointing_tools/core/route_override.h>

void zpt_route_override_table_init(struct zpt_route_override_table *table,
                                   struct zpt_route_override *overrides, size_t capacity) {
    if (table == NULL) {
        return;
    }
    *table = (struct zpt_route_override_table){
        .overrides = overrides,
        .capacity = capacity,
    };
    for (size_t index = 0; index < capacity && overrides != NULL; index++) {
        overrides[index] = (struct zpt_route_override){0};
    }
}

int zpt_route_override_press(struct zpt_route_override_table *table, uint32_t position,
                             size_t pipeline_index) {
    if (table == NULL || table->overrides == NULL) {
        return -EINVAL;
    }

    struct zpt_route_override *available = NULL;
    for (size_t index = 0; index < table->capacity; index++) {
        struct zpt_route_override *candidate = &table->overrides[index];
        if (candidate->active && candidate->position == position) {
            available = candidate;
            break;
        }
        if (!candidate->active && available == NULL) {
            available = candidate;
        }
    }
    if (available == NULL) {
        return -ENOSPC;
    }

    *available = (struct zpt_route_override){
        .position = position,
        .pipeline_index = pipeline_index,
        .order = ++table->next_order,
        .active = true,
    };
    return 0;
}

int zpt_route_override_release(struct zpt_route_override_table *table, uint32_t position) {
    if (table == NULL || table->overrides == NULL) {
        return -EINVAL;
    }
    for (size_t index = 0; index < table->capacity; index++) {
        if (table->overrides[index].active && table->overrides[index].position == position) {
            table->overrides[index].active = false;
            break;
        }
    }
    return 0;
}

bool zpt_route_override_selected(const struct zpt_route_override_table *table,
                                 size_t *pipeline_index) {
    if (table == NULL || table->overrides == NULL) {
        return false;
    }
    const struct zpt_route_override *selected = NULL;
    for (size_t index = 0; index < table->capacity; index++) {
        const struct zpt_route_override *candidate = &table->overrides[index];
        if (candidate->active && (selected == NULL || candidate->order > selected->order)) {
            selected = candidate;
        }
    }
    if (selected == NULL) {
        return false;
    }
    if (pipeline_index != NULL) {
        *pipeline_index = selected->pipeline_index;
    }
    return true;
}
