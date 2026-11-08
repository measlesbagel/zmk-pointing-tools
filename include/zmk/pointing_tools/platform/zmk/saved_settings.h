/* SPDX-License-Identifier: MIT */
#pragma once

#include <stdint.h>

/* On-device saving through Zephyr settings, on the central (or a keyboard
 * that isn't split). Tuning values are kept by service/tuning_store.h under
 * "zpt/t/<stable-id>/<parameter-key>". Pointing-device CPI is kept here
 * under "zpt/d/<stable-id>", for peripheral-owned devices too: the central
 * re-sends a saved CPI each time a peripheral connects.
 *
 * Nothing is written except on an explicit save or clear. */

/* Every device, as in the host protocol. */
#define ZPT_SAVED_ALL_DEVICES UINT8_MAX

/* A device's saved CPI, or 0 when it has none. */
uint16_t zpt_saved_device_cpi(uint8_t device_id);

/* Save a device's current CPI, or erase the saved one when current matches
 * compiled. */
int zpt_saved_device_save(uint8_t device_id);

/* Erase a device's saved CPI and return it to its compiled value, through
 * the split for a peripheral's device. */
int zpt_saved_device_clear(uint8_t device_id);
