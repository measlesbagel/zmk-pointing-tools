/* SPDX-License-Identifier: MIT */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Supported-resolution capabilities for one physical pointing device.
 *
 * A device is settable when its devicetree entry declares either an
 * ascending discrete value list (`cpi-values`) or a stepped range
 * (`cpi-min`/`cpi-max`/`cpi-step`). Exactly one representation may be
 * declared; devices without one are discoverable but read-only. */

struct zpt_cpi_capabilities {
    bool settable;
    bool discrete; /* true: list form, false: range form */
    /* Discrete list form: ascending values, no duplicates. */
    const uint16_t *list_values;
    size_t list_count;
    /* Stepped range form: min <= max, step >= 1. */
    uint16_t range_min;
    uint16_t range_max;
    uint16_t range_step;
};

/* Validate a requested counts-per-inch value against device capabilities.
 *
 * On success writes the effective value to *effective and returns:
 * - 0 when the request matches a supported value exactly,
 * - 1 when the request was snapped to the nearest supported value
 *   (ties resolve toward the lower value),
 * and returns a negative errno otherwise:
 * - -ENOSYS when the device is not settable,
 * - -EINVAL when the capabilities themselves are malformed.
 */
int zpt_cpi_validate(const struct zpt_cpi_capabilities *caps, uint16_t requested,
                     uint16_t *effective);
