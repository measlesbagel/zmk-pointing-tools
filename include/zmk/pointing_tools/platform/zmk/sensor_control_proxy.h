/* SPDX-License-Identifier: MIT */
#pragma once

#include <stdint.h>

#include <zmk/pointing_tools/platform/zmk/device_table.h>

/* Central-side access to devices owned by other split halves. Requests relay
 * through ZMK's INVOKE_BEHAVIOR command to the peripheral sensor-control
 * agent; responses arrive on the packed input-event channel and are
 * correlated by sequence number. One control request is outstanding at a
 * time, mirroring the host protocol's flow control. */

#define ZPT_SENSOR_CONTROL_TIMEOUT_MS 500

/* Preview a resolution on a peripheral-owned entry (location != 0). The
 * request is validated and snapped centrally first; the effective value is
 * echoed back and stored. Returns 0, or a negative errno (-ENODEV when the
 * half is unreachable, -EAGAIN on timeout). */
int zpt_sensor_control_preview_remote(const struct zpt_pointing_device *device, uint16_t requested,
                                      uint16_t *effective);

/* Read the live value of a peripheral-owned entry. Falls back to the entry's
 * compiled default when the half cannot be reached. */
int zpt_sensor_control_get_remote(const struct zpt_pointing_device *device, uint16_t *cpi);
