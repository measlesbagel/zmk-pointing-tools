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
 * a split keyboard agree without negotiation.
 *
 * The settable capability form is selected per instance inside macro
 * expansion (COND_CODE_1 over property presence): preprocessing directives
 * cannot make this choice, because at file scope the loop variable is an
 * unresolved token rather than an instance number. */

BUILD_ASSERT(DT_NUM_INST_STATUS_OKAY(DT_DRV_COMPAT) <= UINT8_MAX + 1,
             "device ids are assigned in devicetree order and must fit in a u8");

#define ZPT_DEVICE_VALUE_ELEM(node_id, prop, idx) DT_PROP_BY_IDX(node_id, prop, idx),

/* Discrete list form: ascending values, no duplicates, each fitting a u16. */
#define ZPT_DEVICE_LIST_FORM(inst)                                                                 \
    BUILD_ASSERT(!(DT_NODE_HAS_PROP(DT_DRV_INST(inst), cpi_min) ||                                 \
                   DT_NODE_HAS_PROP(DT_DRV_INST(inst), cpi_max) ||                                 \
                   DT_NODE_HAS_PROP(DT_DRV_INST(inst), cpi_step)),                                 \
                 "declare either cpi-values or the cpi-min/cpi-max/cpi-step range, not both");     \
    BUILD_ASSERT(DT_INST_PROP_LEN(inst, cpi_values) >= 1, "cpi-values must not be empty");         \
    static const uint16_t zpt_device_values_##inst[] = {                                           \
        DT_INST_FOREACH_PROP_ELEM(inst, cpi_values, ZPT_DEVICE_VALUE_ELEM)};

/* Stepped range form. */
#define ZPT_DEVICE_RANGE_FORM(inst)                                                                \
    BUILD_ASSERT(!DT_NODE_HAS_PROP(DT_DRV_INST(inst), cpi_values),                                 \
                 "declare either cpi-values or the cpi-min/cpi-max/cpi-step range, not both");     \
    BUILD_ASSERT(IN_RANGE(DT_INST_PROP(inst, cpi_min), 1, UINT16_MAX) &&                           \
                     IN_RANGE(DT_INST_PROP(inst, cpi_max), 1, UINT16_MAX),                         \
                 "cpi bounds must fit in a u16");                                                  \
    BUILD_ASSERT(DT_INST_PROP(inst, cpi_min) <= DT_INST_PROP(inst, cpi_max),                       \
                 "cpi-min must not exceed cpi-max");                                               \
    BUILD_ASSERT(IN_RANGE(DT_INST_PROP_OR(inst, cpi_step, 1), 1, UINT16_MAX),                      \
                 "cpi-step must fit in a u16");                                                    \
    struct zpt_device_range_form_##inst {                                                          \
        uint8_t range_form_has_no_storage;                                                         \
    };

/* Neither representation: discoverable but read-only. */
#define ZPT_DEVICE_READONLY_FORM(inst)                                                             \
    BUILD_ASSERT(!(DT_NODE_HAS_PROP(DT_DRV_INST(inst), cpi_max) ||                                 \
                   DT_NODE_HAS_PROP(DT_DRV_INST(inst), cpi_step)),                                 \
                 "cpi-max/cpi-step require cpi-min; omit them all for a read-only device");        \
    struct zpt_device_read_only_##inst {                                                           \
        uint8_t read_only_device_has_no_storage;                                                   \
    };

#define ZPT_DEVICE_FORM(inst)                                                                      \
    COND_CODE_1(DT_NODE_HAS_PROP(DT_DRV_INST(inst), cpi_values), (ZPT_DEVICE_LIST_FORM(inst)),     \
                (COND_CODE_1(DT_NODE_HAS_PROP(DT_DRV_INST(inst), cpi_min),                         \
                             (ZPT_DEVICE_RANGE_FORM(inst)), (ZPT_DEVICE_READONLY_FORM(inst)))))

#define ZPT_DEVICE_INIT_CAPS(inst)                                                                 \
    COND_CODE_1(DT_NODE_HAS_PROP(DT_DRV_INST(inst), cpi_values),                                   \
                ({.settable = true,                                                                \
                  .discrete = true,                                                                \
                  .list_values = zpt_device_values_##inst,                                         \
                  .list_count = DT_INST_PROP_LEN(inst, cpi_values)}),                              \
                (COND_CODE_1(DT_NODE_HAS_PROP(DT_DRV_INST(inst), cpi_min),                         \
                             ({.settable = true,                                                   \
                               .discrete = false,                                                  \
                               .range_min = (uint16_t)DT_INST_PROP(inst, cpi_min),                 \
                               .range_max = (uint16_t)DT_INST_PROP(inst, cpi_max),                 \
                               .range_step = (uint16_t)DT_INST_PROP_OR(inst, cpi_step, 1)}),       \
                             ({.settable = false}))))

#define ZPT_DEVICE_DEFINE(inst)                                                                    \
    BUILD_ASSERT(IN_RANGE(DT_INST_PROP_OR(inst, location, 0), 0, UINT8_MAX),                       \
                 "location must fit in a u8 (0 local, otherwise peripheral index + 1)");           \
    BUILD_ASSERT(IN_RANGE(DT_INST_PROP(inst, resolution_cpi), 1, UINT16_MAX),                      \
                 "resolution-cpi must fit in a u16");                                              \
    ZPT_DEVICE_FORM(inst)                                                                          \
    static const struct zpt_pointing_device zpt_device_##inst = {                                  \
        .id = (uint8_t)inst,                                                                       \
        .location = (uint8_t)DT_INST_PROP_OR(inst, location, 0),                                   \
        .default_cpi = (uint16_t)DT_INST_PROP(inst, resolution_cpi),                               \
        .stable_id = DT_INST_PROP(inst, stable_id),                                                \
        .devicetree_path = DT_NODE_PATH(DT_DRV_INST(inst)),                                        \
        .sensor = DEVICE_DT_GET(DT_INST_PHANDLE(inst, sensor)),                                    \
        .caps = ZPT_DEVICE_INIT_CAPS(inst)};

DT_INST_FOREACH_STATUS_OKAY(ZPT_DEVICE_DEFINE)

#define ZPT_DEVICE_ENTRY(inst) &zpt_device_##inst,

static const struct zpt_pointing_device *const zpt_device_entries[] = {
    DT_INST_FOREACH_STATUS_OKAY(ZPT_DEVICE_ENTRY)};

/* Volatile current values, seeded from the compiled defaults. The telemetry
 * service is the only writer today (one outstanding control request at a
 * time); entries without settable capabilities are never written and keep
 * reporting their default. */
#define ZPT_DEVICE_DEFAULT_CPI(inst) DT_INST_PROP(inst, resolution_cpi),
static uint16_t zpt_device_current_cpi[] = {DT_INST_FOREACH_STATUS_OKAY(ZPT_DEVICE_DEFAULT_CPI)};

static const struct zpt_pointing_device *zpt_device_valid(const struct zpt_pointing_device *device,
                                                          uint16_t **current) {
    if (device == NULL || device->id >= ARRAY_SIZE(zpt_device_entries) ||
        zpt_device_entries[device->id] != device) {
        return NULL;
    }
    *current = &zpt_device_current_cpi[device->id];
    return device;
}

int zpt_device_control_get(const struct zpt_pointing_device *device, uint16_t *cpi) {
    uint16_t *current;

    if (device == NULL || cpi == NULL || zpt_device_valid(device, &current) == NULL) {
        return -EINVAL;
    }
    /* Native sensor facets read through here once they exist; the RAM value
     * tracks the compiled default until a preview changes it. */
    *cpi = *current;
    return 0;
}

int zpt_device_control_preview(const struct zpt_pointing_device *device, uint16_t requested,
                               uint16_t *effective) {
    uint16_t *current;
    int ret;

    if (zpt_device_valid(device, &current) == NULL) {
        return -EINVAL;
    }
    if (!device->caps.settable) {
        return -ENOSYS;
    }
    if (effective == NULL) {
        return -EINVAL;
    }
    ret = zpt_cpi_validate(&device->caps, requested, effective);
    if (ret < 0) {
        return ret;
    }
    /* Native sensor facets dispatch here before the store lands. */
    *current = *effective;
    return ret;
}

int zpt_device_control_reset(const struct zpt_pointing_device *device) {
    uint16_t *current;

    if (zpt_device_valid(device, &current) == NULL) {
        return -EINVAL;
    }
    if (!device->caps.settable) {
        return -ENOSYS;
    }
    *current = device->default_cpi;
    return 0;
}

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
