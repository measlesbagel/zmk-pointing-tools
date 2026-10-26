/* SPDX-License-Identifier: MIT */

#define DT_DRV_COMPAT measlesbagel_zpt_pointing_device

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>
#include <zmk/pointing_tools/platform/zmk/device_table.h>
#include <zmk/pointing_tools/source/device_caps.h>
#include <zmk/pointing_tools/source/sensor_control.h>
#include <zmk/pointing_tools/source/sensor_control_wire.h>

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
        .id = (uint8_t)(inst),                                                                     \
        .location = (uint8_t)DT_INST_PROP_OR(inst, location, 0),                                   \
        .default_cpi = (uint16_t)DT_INST_PROP(inst, resolution_cpi),                               \
        .stable_id = DT_INST_PROP(inst, stable_id),                                                \
        .devicetree_path = DT_NODE_PATH(DT_DRV_INST(inst)),                                        \
        .sensor = COND_CODE_1(DT_INST_NODE_HAS_PROP(inst, sensor),                                 \
                              (DEVICE_DT_GET(DT_INST_PHANDLE(inst, sensor))), (NULL)),             \
        .caps = ZPT_DEVICE_INIT_CAPS(inst)};

DT_INST_FOREACH_STATUS_OKAY(ZPT_DEVICE_DEFINE)

#define ZPT_DEVICE_ENTRY(inst) &zpt_device_##inst,

static const struct zpt_pointing_device *const zpt_device_entries[] = {
    DT_INST_FOREACH_STATUS_OKAY(ZPT_DEVICE_ENTRY)};

/* Volatile current values, seeded from the compiled defaults. One control
 * request is outstanding at a time, so writers never race each other, but
 * source processors read these on the input thread to label frames, hence
 * the atomics. Entries without settable capabilities are never written and
 * keep reporting their default. */
#define ZPT_DEVICE_DEFAULT_CPI(inst) ATOMIC_INIT(DT_INST_PROP(inst, resolution_cpi)),
static atomic_t zpt_device_current_cpi_store[] = {
    DT_INST_FOREACH_STATUS_OKAY(ZPT_DEVICE_DEFAULT_CPI)};

static atomic_t *zpt_device_valid(const struct zpt_pointing_device *device) {
    if (device == NULL || device->id >= ARRAY_SIZE(zpt_device_entries) ||
        zpt_device_entries[device->id] != device) {
        return NULL;
    }
    return &zpt_device_current_cpi_store[device->id];
}

uint16_t zpt_device_current_cpi(const struct zpt_pointing_device *device) {
    const atomic_t *current = zpt_device_valid(device);
    return current == NULL ? 0U : (uint16_t)atomic_get(current);
}

int zpt_device_control_record(const struct zpt_pointing_device *device, uint16_t cpi) {
    atomic_t *current = zpt_device_valid(device);

    if (current == NULL || cpi == 0U) {
        return -EINVAL;
    }
    atomic_set(current, (atomic_val_t)cpi);
    return 0;
}

int zpt_device_control_get(const struct zpt_pointing_device *device, uint16_t *cpi) {
    const atomic_t *current = zpt_device_valid(device);

    if (current == NULL || cpi == NULL) {
        return -EINVAL;
    }
    /* Native facets report live sensor state; the RAM value tracks the
     * compiled default until a preview changes it. */
    const struct zpt_sensor_control_api *api = zpt_sensor_control(device->sensor);
    if (api != NULL && api->get_cpi != NULL) {
        return api->get_cpi(device->sensor, cpi);
    }
    *cpi = (uint16_t)atomic_get(current);
    return 0;
}

int zpt_device_control_preview(const struct zpt_pointing_device *device, uint16_t requested,
                               uint16_t *effective) {
    atomic_t *current = zpt_device_valid(device);
    int ret;

    if (current == NULL) {
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
    /* Native facets receive the already-snapped value and write volatile
     * registers; the store mirrors the applied value so fallback reads stay
     * consistent when a driver has no read-back op. */
    const struct zpt_sensor_control_api *api = zpt_sensor_control(device->sensor);
    if (api != NULL && api->set_cpi != NULL) {
        ret = api->set_cpi(device->sensor, *effective);
        if (ret < 0) {
            return ret;
        }
    }
    atomic_set(current, (atomic_val_t)*effective);
    return ret;
}

int zpt_device_control_reset(const struct zpt_pointing_device *device) {
    atomic_t *current = zpt_device_valid(device);

    if (current == NULL) {
        return -EINVAL;
    }
    if (!device->caps.settable) {
        return -ENOSYS;
    }
    const struct zpt_sensor_control_api *api = zpt_sensor_control(device->sensor);
    if (api != NULL && api->set_cpi != NULL) {
        const int ret = api->set_cpi(device->sensor, device->default_cpi);
        if (ret < 0) {
            return ret;
        }
    }
    atomic_set(current, (atomic_val_t)device->default_cpi);
    return 0;
}

/* --- Split request service -------------------------------------------- */

uint32_t zpt_device_control_serve(uint32_t param1, uint32_t param2) {
    const uint8_t seq = zpt_scw_param_seq(param1);
    const struct zpt_pointing_device *device = zpt_device_table_at(zpt_scw_param_device_id(param1));
    uint16_t value = 0;
    int ret;

    if (device == NULL || device->location == 0U) {
        /* Unknown here, or owned by the central, which never relays its own
         * devices: answer rather than leave the central waiting. */
        ret = -ENODEV;
    } else {
        switch (zpt_scw_param_opcode(param1)) {
        case ZPT_SCW_OPCODE_GET_CPI:
            ret = zpt_device_control_get(device, &value);
            break;
        case ZPT_SCW_OPCODE_SET_CPI:
            /* The central validated and snapped against the same capability
             * declaration; preview validates again and echoes the value it
             * applied. */
            ret = zpt_device_control_preview(device, (uint16_t)(param2 & 0xFFFFu), &value);
            break;
        default:
            ret = -EINVAL;
            break;
        }
    }
    if (ret < 0) {
        value = 0;
    }
    return zpt_scw_encode(ret < 0 ? (uint8_t)MIN(-ret, UINT8_MAX) : 0U, seq, value);
}

/* --- Native sensor facet registry ------------------------------------- */

#define ZPT_SENSOR_CONTROL_MAX_BINDINGS 4

static const struct zpt_sensor_control_api
    *zpt_sensor_control_bindings[ZPT_SENSOR_CONTROL_MAX_BINDINGS];
static const struct device *zpt_sensor_control_sensors[ZPT_SENSOR_CONTROL_MAX_BINDINGS];

int zpt_sensor_control_register(const struct device *sensor,
                                const struct zpt_sensor_control_api *api) {
    if (sensor == NULL || api == NULL) {
        return -EINVAL;
    }
    for (size_t i = 0; i < ARRAY_SIZE(zpt_sensor_control_sensors); i++) {
        if (zpt_sensor_control_sensors[i] == NULL || zpt_sensor_control_sensors[i] == sensor) {
            zpt_sensor_control_sensors[i] = sensor;
            zpt_sensor_control_bindings[i] = api;
            return 0;
        }
    }
    return -ENOMEM;
}

const struct zpt_sensor_control_api *zpt_sensor_control(const struct device *sensor) {
    if (sensor == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < ARRAY_SIZE(zpt_sensor_control_sensors); i++) {
        if (zpt_sensor_control_sensors[i] == sensor) {
            return zpt_sensor_control_bindings[i];
        }
    }
    return NULL;
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
