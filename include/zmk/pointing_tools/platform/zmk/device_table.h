/* SPDX-License-Identifier: MIT */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zmk/pointing_tools/source/device_caps.h>

/* One physical pointing device declared in devicetree
 * ("measlesbagel,zpt-pointing-device"). Entries are compiled into every half
 * of a split keyboard from the same source, so numeric ids and stable ids
 * agree without runtime negotiation. */

struct zpt_pointing_device {
    uint8_t id;       /* dense session id, assigned in devicetree order */
    uint8_t location; /* 0 central-local, otherwise owning peripheral index + 1 */
    uint16_t default_cpi;
    const char *stable_id;
    const struct device *sensor;
    struct zpt_cpi_capabilities caps;
};

/* Number of declared devices (never changes after boot). */
size_t zpt_device_table_count(void);

/* Entry by index; NULL when out of bounds. */
const struct zpt_pointing_device *zpt_device_table_at(size_t index);

/* Entry by cross-half stable identity; NULL when unknown. */
const struct zpt_pointing_device *zpt_device_table_find(const char *stable_id);
