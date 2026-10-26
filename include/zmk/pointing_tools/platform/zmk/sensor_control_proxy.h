/* SPDX-License-Identifier: MIT */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <zmk/pointing_tools/platform/zmk/device_table.h>

/* Central-side access to devices owned by other split halves. Requests relay
 * through ZMK's INVOKE_BEHAVIOR command to the peripheral sensor-control
 * agent; responses arrive on the packed input-event channel and are
 * correlated by sequence number. One control request is outstanding at a
 * time, mirroring the host protocol's flow control. */

#define ZPT_SENSOR_CONTROL_TIMEOUT_MS 500

/* Preview a resolution on a peripheral-owned entry (location != 0). The
 * request is validated and snapped centrally first; the peripheral echoes
 * the value it applied, which the central records so motion from that half
 * is labelled with it. Returns 0, or a negative errno (-EAGAIN when the half
 * didn't answer in time). */
int zpt_sensor_control_preview_remote(const struct zpt_pointing_device *device, uint16_t requested,
                                      uint16_t *effective);

/* Read the live value of a peripheral-owned entry. When the half cannot be
 * reached, reports the last value the central knows about and returns the
 * error. */
int zpt_sensor_control_get_remote(const struct zpt_pointing_device *device, uint16_t *cpi);

/* Whether the half owning a peripheral entry answered the last request sent
 * to it. False before the first request. */
bool zpt_sensor_control_remote_reachable(const struct zpt_pointing_device *device);
