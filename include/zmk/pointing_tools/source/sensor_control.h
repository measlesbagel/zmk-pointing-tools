/* SPDX-License-Identifier: MIT */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include <zephyr/device.h>

/* Optional driver facet for pointing sensors whose resolution can change at
 * runtime (PAW3222 and friends). Two integration paths exist:
 *
 * - Native drivers implement this API and register it against their device
 *   via zpt_sensor_control_register() (the bundled PAW32XX adapter does this
 *   automatically when the driver module is present).
 * - Drivers receive already-validated, exactly supported values: the caller
 *   snaps requests against the declared capabilities before calling set_cpi.
 * - set_cpi writes volatile registers only. The compiled devicetree value is
 *   restored by the next reboot; drivers must never write sensor NVM.
 * - Individual callbacks may be NULL when unsupported: without get_cpi the
 *   control layer reports its stored/compiled value; without the supported
 *   accessors the devicetree capability declaration is authoritative.
 */

struct zpt_sensor_control_api {
    int (*get_cpi)(const struct device *dev, uint16_t *cpi);
    int (*set_cpi)(const struct device *dev, uint16_t cpi);
    int (*supported_count)(const struct device *dev, size_t *count);
    int (*supported_get)(const struct device *dev, size_t index, uint16_t *cpi);
};

/* Bind a native facet to a sensor device. Call once from driver or board
 * init, before host traffic starts; later registrations replace earlier ones
 * for the same device. Returns -ENOMEM when the registry is full. */
int zpt_sensor_control_register(const struct device *sensor,
                                const struct zpt_sensor_control_api *api);

/* Look up the facet bound to a sensor device; NULL when none registered. */
const struct zpt_sensor_control_api *zpt_sensor_control(const struct device *sensor);
