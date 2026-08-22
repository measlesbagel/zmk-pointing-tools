/* SPDX-License-Identifier: MIT */
#define DT_DRV_COMPAT measlesbagel_zpt_sensor_control

#include <errno.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/input/input.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <drivers/behavior.h>
#include <zmk/behavior.h>

#include <zmk/pointing_tools/platform/zmk/device_table.h>
#include <zmk/pointing_tools/source/sensor_control_wire.h>

LOG_MODULE_REGISTER(zpt_sensor_agent, CONFIG_ZMK_LOG_LEVEL);

/* Peripheral-half agent for split-safe CPI control: the central relays
 * requests through ZMK's INVOKE_BEHAVIOR command, this behavior executes the
 * operation against the local device table, and the outcome rides back as
 * one packed relative input event on a reserved code. */

static int zpt_agent_respond(const struct device *report_dev, uint8_t seq, int error,
                             uint16_t value) {
    const uint32_t frame = zpt_scw_encode(error < 0 ? (uint8_t)(-error) : 0, seq, value);
    const int ret =
        input_report(report_dev, INPUT_EV_REL, INPUT_REL_MISC, (int32_t)frame, true, K_NO_WAIT);
    if (ret < 0) {
        LOG_ERR("Failed to emit sensor-control response: %d", ret);
    }
    return ret;
}

static int zpt_agent_pressed(struct zmk_behavior_binding *binding,
                             struct zmk_behavior_binding_event event) {
    ARG_UNUSED(event);
    const struct device *behavior = zmk_behavior_get_binding(binding->behavior_dev);
    if (behavior == NULL) {
        return -ENODEV;
    }

    const uint8_t seq = zpt_scw_param_seq(binding->param1);
    const uint8_t opcode = zpt_scw_param_opcode(binding->param1);
    const uint8_t device_id = zpt_scw_param_device_id(binding->param1);
    const struct zpt_pointing_device *device = zpt_device_table_at(device_id);
    if (device == NULL) {
        return zpt_agent_respond(behavior, seq, -ENODEV, 0);
    }

    uint16_t value = 0;
    switch (opcode) {
    case ZPT_SCW_OPCODE_GET_CPI:
        value = 0;
        break;
    case ZPT_SCW_OPCODE_SET_CPI:
        /* The central already validated and snapped against the shared
         * capability declaration; preview applies and echoes the effective
         * value. */
        value = (uint16_t)(binding->param2 & 0xFFFFu);
        break;
    default:
        LOG_WRN("Unknown sensor-control opcode %u", opcode);
        return zpt_agent_respond(behavior, seq, -EINVAL, 0);
    }

    /* GET reads through control so native sensor facets answer when present;
     * SET reuses preview's validation-and-store path. */
    int ret;
    uint16_t reported = 0;
    if (opcode == ZPT_SCW_OPCODE_GET_CPI) {
        ret = zpt_device_control_get(device, &reported);
    } else {
        ret = zpt_device_control_preview(device, value, &reported);
    }
    return zpt_agent_respond(behavior, seq, ret, reported);
}

static const struct behavior_driver_api zpt_sensor_control_behavior_api = {
    .binding_pressed = zpt_agent_pressed,
};

#define ZPT_SENSOR_CONTROL_DEFINE(inst)                                                            \
    BEHAVIOR_DT_INST_DEFINE(inst, NULL, NULL, NULL, NULL, POST_KERNEL,                             \
                            CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,                                   \
                            &zpt_sensor_control_behavior_api);

DT_INST_FOREACH_STATUS_OKAY(ZPT_SENSOR_CONTROL_DEFINE)
