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
    uint8_t location; /* 0 central-local, otherwise owning peripheral index + 1;
                         for split topologies with more than one peripheral,
                         align this with the half's zmk,input-split reg + 1 */
    uint16_t default_cpi;
    const char *stable_id;
    const char *devicetree_path; /* node path of this entry, for host profiles */
    const struct device *sensor;
    struct zpt_cpi_capabilities caps;
};

/* Number of declared devices (never changes after boot). */
size_t zpt_device_table_count(void);

/* Entry by index; NULL when out of bounds. */
const struct zpt_pointing_device *zpt_device_table_at(size_t index);

/* Entry by cross-half stable identity; NULL when unknown. */
const struct zpt_pointing_device *zpt_device_table_find(const char *stable_id);

/* Runtime resolution control for one table entry.
 *
 * The current value lives in RAM: previews never persist, and reboot (or an
 * explicit reset) restores the compiled devicetree value. Callers are
 * externally synchronized, like the tuning registry — the host protocol
 * allows one outstanding control request at a time.
 *
 * get always succeeds on a valid entry, read-only or not: a device without
 * settable capabilities simply reports its compiled value forever. preview
 * validates the request against the entry's capabilities, snaps to the
 * nearest supported value when needed (returning 1 instead of 0), applies it
 * to the RAM store, and reports the effective value. reset restores the
 * compiled value. Both return -ENOSYS on entries without settable
 * capabilities and -EINVAL on malformed arguments.
 *
 * Native sensor facets (see sensor_control.h) dispatch inside preview/get
 * ahead of the RAM store once the first real driver registers; until then
 * every entry is served by the store seeded from devicetree. */
int zpt_device_control_get(const struct zpt_pointing_device *device, uint16_t *cpi);
int zpt_device_control_preview(const struct zpt_pointing_device *device, uint16_t requested,
                               uint16_t *effective);
int zpt_device_control_reset(const struct zpt_pointing_device *device);
