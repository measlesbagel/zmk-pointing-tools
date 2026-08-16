/* SPDX-License-Identifier: MIT */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Position-keyed explicit route overrides with newest-press precedence.
 *
 * Overrides are tracked by key position so overlapping route behaviors can be
 * held simultaneously. Pressing a position activates or replaces its override;
 * the newest press wins when several are held. Releasing a position removes
 * its override and reveals the next newest held override, then the layer
 * policy. Pipeline indices are router-relative and owned by the caller.
 */

struct zpt_route_override {
    uint32_t position;
    size_t pipeline_index;
    uint64_t order;
    bool active;
};

struct zpt_route_override_table {
    struct zpt_route_override *overrides;
    size_t capacity;
    uint64_t next_order;
};

void zpt_route_override_table_init(struct zpt_route_override_table *table,
                                   struct zpt_route_override *overrides, size_t capacity);

/**
 * @brief Activate or replace the override for a key position.
 *
 * @retval 0 on success.
 * @retval -ENOSPC when the table is full of other active overrides.
 */
int zpt_route_override_press(struct zpt_route_override_table *table, uint32_t position,
                             size_t pipeline_index);

/**
 * @brief Deactivate the override for a key position.
 *
 * @retval 0 whether or not an override was active for the position.
 */
int zpt_route_override_release(struct zpt_route_override_table *table, uint32_t position);

/**
 * @brief Select the newest active override.
 *
 * @retval true when an override is active, with its pipeline index in
 *         @p pipeline_index.
 * @retval false when no override is active.
 */
bool zpt_route_override_selected(const struct zpt_route_override_table *table,
                                 size_t *pipeline_index);
