/* SPDX-License-Identifier: MIT */
#pragma once

#include <zephyr/device.h>

#include <zmk/pointing_tools/policy/suppression.h>

/*
 * ZMK keypress suppression policy.
 *
 * A measlesbagel,zpt-keypress-suppression device records physical keypresses
 * through the position-state listener and exposes a suppression policy that
 * stages reference from devicetree. Suppression is active for the configured
 * window after any keypress, mirroring the legacy scroll processor guard.
 */

__subsystem struct zpt_keypress_suppression_driver_api {
    const struct zpt_suppression_policy *(*get_policy)(const struct device *dev);
};

int zpt_zmk_keypress_suppression_get(const struct device *dev,
                                     const struct zpt_suppression_policy **policy);
