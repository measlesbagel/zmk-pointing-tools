/* SPDX-License-Identifier: MIT */

#include <errno.h>
#include <stdint.h>

#include <zmk/pointing_tools/source/device_caps.h>

static int validate_discrete(const struct zpt_cpi_capabilities *caps, uint16_t requested,
                             uint16_t *effective) {
    if (caps->list_count == 0U || caps->list_values == NULL) {
        return -EINVAL;
    }
    size_t best = 0;
    uint16_t best_distance = UINT16_MAX;
    for (size_t i = 0; i < caps->list_count; i++) {
        if (caps->list_values[i] == requested) {
            *effective = requested;
            return 0;
        }
        uint16_t distance = caps->list_values[i] > requested
                                ? (uint16_t)(caps->list_values[i] - requested)
                                : (uint16_t)(requested - caps->list_values[i]);
        /* Strictly less keeps the lower value on ties. */
        if (distance < best_distance) {
            best_distance = distance;
            best = i;
        }
    }
    *effective = caps->list_values[best];
    return 1;
}

static int snap_range_point(const struct zpt_cpi_capabilities *caps, uint16_t requested,
                            uint16_t *effective) {
    uint32_t offset = (uint32_t)(requested - caps->range_min);
    uint32_t below = offset - offset % caps->range_step;
    uint32_t above = below + caps->range_step;
    uint32_t chosen = below;
    if ((above <= (uint32_t)(caps->range_max - caps->range_min)) &&
        (above - offset) < (offset - below)) {
        chosen = above;
    }
    *effective = (uint16_t)(caps->range_min + chosen);
    return *effective == requested ? 0 : 1;
}

static int validate_range(const struct zpt_cpi_capabilities *caps, uint16_t requested,
                          uint16_t *effective) {
    if (caps->range_step == 0U || caps->range_min > caps->range_max) {
        return -EINVAL;
    }
    if (requested <= caps->range_min) {
        *effective = caps->range_min;
        return *effective == requested ? 0 : 1;
    }
    if (requested >= caps->range_max) {
        *effective = caps->range_max;
        return *effective == requested ? 0 : 1;
    }
    return snap_range_point(caps, requested, effective);
}

int zpt_cpi_validate(const struct zpt_cpi_capabilities *caps, uint16_t requested,
                     uint16_t *effective) {
    if (caps == NULL || effective == NULL || !caps->settable) {
        return -ENOSYS;
    }
    return caps->discrete ? validate_discrete(caps, requested, effective)
                          : validate_range(caps, requested, effective);
}
