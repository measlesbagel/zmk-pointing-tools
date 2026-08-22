/* SPDX-License-Identifier: MIT */

#define DT_DRV_COMPAT measlesbagel_zpt_pointing_device

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/sys/util.h>
#include <zmk/pointing_tools/platform/zmk/device_table.h>
#include <zmk/pointing_tools/source/device_caps.h>

/* Devicetree-declared physical pointing devices. The table is fully const:
 * ids, capabilities, and defaults are fixed at build time so both halves of
 * a split keyboard agree without negotiation. */

BUILD_ASSERT(DT_NUM_INST_STATUS_OKAY(DT_DRV_COMPAT) <= UINT8_MAX + 1,
             "device ids are assigned in devicetree order and must fit in a u8");

#define ZPT_DEVICE_VALUE_ELEM(node_id, idx) DT_PROP_BY_IDX(node_id, cpi_values, idx),

/* Build-time constraints plus ROM storage for one instance. Every form
 * expands to at least one complete top-level statement. */
#if DT_NODE_HAS_PROP(DT_DRV_INST(inst), cpi_values)

#define ZPT_DEVICE_CONSTRAINTS(inst)                                                               \
    BUILD_ASSERT(!(DT_NODE_HAS_PROP(DT_DRV_INST(inst), cpi_min) ||                                 \
                   DT_NODE_HAS_PROP(DT_DRV_INST(inst), cpi_max) ||                                 \
                   DT_NODE_HAS_PROP(DT_DRV_INST(inst), cpi_step)),                                 \
                 "declare either cpi-values or the cpi-min/cpi-max/cpi-step range, not both");     \
    BUILD_ASSERT(DT_INST_PROP_LEN(inst, cpi_values) >= 1, "cpi-values must not be empty")
#define ZPT_DEVICE_STORAGE(inst)                                                                   \
    static const uint16_t zpt_device_values_##inst[] = {                                           \
        DT_INST_FOREACH_PROP_ELEM(inst, cpi_values, ZPT_DEVICE_VALUE_ELEM)};
#define ZPT_DEVICE_INIT_CAPS(inst)                                                                 \
    {.settable = true,                                                                             \
     .discrete = true,                                                                             \
     .list_values = zpt_device_values_##inst,                                                      \
     .list_count = DT_INST_PROP_LEN(inst, cpi_values)}

#elif DT_NODE_HAS_PROP(DT_DRV_INST(inst), cpi_min)

#define ZPT_DEVICE_CONSTRAINTS(inst)                                                               \
    BUILD_ASSERT(DT_NODE_HAS_PROP(DT_DRV_INST(inst), cpi_max), "cpi-min requires cpi-max");        \
    BUILD_ASSERT(!DT_NODE_HAS_PROP(DT_DRV_INST(inst), cpi_values),                                 \
                 "declare either cpi-values or the cpi-min/cpi-max/cpi-step range, not both");     \
    BUILD_ASSERT(DT_INST_PROP(inst, cpi_min) <= DT_INST_PROP(inst, cpi_max),                       \
                 "cpi-min must not exceed cpi-max");                                               \
    BUILD_ASSERT(IN_RANGE(DT_INST_PROP(inst, cpi_min), 1, UINT16_MAX) &&                           \
                     IN_RANGE(DT_INST_PROP(inst, cpi_max), 1, UINT16_MAX),                         \
                 "cpi bounds must fit in a u16");                                                  \
    BUILD_ASSERT(IN_RANGE(DT_INST_PROP_OR(inst, cpi_step, 1), 1, UINT16_MAX),                      \
                 "cpi-step must fit in a u16")
#define ZPT_DEVICE_STORAGE(inst)                                                                   \
    struct zpt_device_range_form_##inst {                                                          \
        uint8_t range_form_has_no_storage;                                                         \
    }
#define ZPT_DEVICE_INIT_CAPS(inst)                                                                 \
    {.settable = true,                                                                             \
     .discrete = false,                                                                            \
     .range_min = (uint16_t)DT_INST_PROP(inst, cpi_min),                                           \
     .range_max = (uint16_t)DT_INST_PROP(inst, cpi_max),                                           \
     .range_step = (uint16_t)DT_INST_PROP_OR(inst, cpi_step, 1)}

#else

#define ZPT_DEVICE_CONSTRAINTS(inst)                                                               \
    BUILD_ASSERT(!(DT_NODE_HAS_PROP(DT_DRV_INST(inst), cpi_max) ||                                 \
                   DT_NODE_HAS_PROP(DT_DRV_INST(inst), cpi_step)),                                 \
                 "cpi-max/cpi-step require cpi-min; omit them all for a read-only device")
#define ZPT_DEVICE_STORAGE(inst)                                                                   \
    struct zpt_device_read_only_##inst {                                                           \
        uint8_t read_only_device_has_no_storage;                                                   \
    }
#define ZPT_DEVICE_INIT_CAPS(inst) {.settable = false}

#endif

#define ZPT_DEVICE_DEFINE(inst)                                                                    \
    BUILD_ASSERT(IN_RANGE(DT_INST_PROP_OR(inst, location, 0), 0, UINT8_MAX),                       \
                 "location must fit in a u8 (0 local, otherwise peripheral index + 1)");           \
    BUILD_ASSERT(IN_RANGE(DT_INST_PROP(inst, resolution_cpi), 1, UINT16_MAX),                      \
                 "resolution-cpi must fit in a u16");                                              \
    ZPT_DEVICE_CONSTRAINTS(inst);                                                                  \
    ZPT_DEVICE_STORAGE(inst);                                                                      \
    static const struct zpt_pointing_device zpt_device_##inst = {                                  \
        .id = (uint8_t)inst,                                                                       \
        .location = (uint8_t)DT_INST_PROP_OR(inst, location, 0),                                   \
        .default_cpi = (uint16_t)DT_INST_PROP(inst, resolution_cpi),                               \
        .stable_id = DT_INST_PROP(inst, stable_id),                                                \
        .sensor = DEVICE_DT_GET(DT_INST_PHANDLE(inst, sensor)),                                    \
        .caps = ZPT_DEVICE_INIT_CAPS(inst)};

DT_INST_FOREACH_STATUS_OKAY(ZPT_DEVICE_DEFINE)

#define ZPT_DEVICE_ENTRY(inst) &zpt_device_##inst,

static const struct zpt_pointing_device *const zpt_device_entries[] = {
    DT_INST_FOREACH_STATUS_OKAY(ZPT_DEVICE_ENTRY)};

size_t zpt_device_table_count(void) { return ARRAY_SIZE(zpt_device_entries); }

const struct zpt_pointing_device *zpt_device_table_at(size_t index) {
    if (index >= ARRAY_SIZE(zpt_device_entries)) {
        return NULL;
    }
    return zpt_device_entries[index];
}

const struct zpt_pointing_device *zpt_device_table_find(const char *stable_id) {
    if (stable_id == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < ARRAY_SIZE(zpt_device_entries); i++) {
        const struct zpt_pointing_device *entry = zpt_device_entries[i];
        if (strcmp(entry->stable_id, stable_id) == 0) {
            return entry;
        }
    }
    return NULL;
}
