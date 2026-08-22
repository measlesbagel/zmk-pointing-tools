/* SPDX-License-Identifier: MIT */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include <zephyr/device.h>

/* Optional driver facet for pointing sensors whose resolution can change at
 * runtime (PAW3222 and friends). Sensor drivers implement this API alongside
 * their primary driver API; the device-control layer dispatches through it
 * when present and falls back to its RAM store otherwise.
 *
 * Contract:
 * - Drivers receive already-validated, exactly supported values: the caller
 *   snaps requests against the declared capabilities before calling set_cpi.
 * - set_cpi writes volatile registers only. The compiled devicetree value is
 *   restored by the next reboot; drivers must never write sensor NVM.
 * - get_cpi reports the live sensor state; it may differ from the compiled
 *   value after a preview or an out-of-band change.
 * - supported_count/supported_get expose the values the hardware actually
 *   implements, for drivers where that truth lives in registers rather than
 *   in the devicetree capability declaration.
 */

struct zpt_sensor_control_api {
    int (*get_cpi)(const struct device *dev, uint16_t *cpi);
    int (*set_cpi)(const struct device *dev, uint16_t cpi);
    int (*supported_count)(const struct device *dev, size_t *count);
    int (*supported_get)(const struct device *dev, size_t index, uint16_t *cpi);
};
