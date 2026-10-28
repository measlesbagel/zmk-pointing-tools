/* SPDX-License-Identifier: MIT */
#define DT_DRV_COMPAT measlesbagel_zpt_sensor_control

#include <errno.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/input/input.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/spinlock.h>

#include <drivers/behavior.h>
#include <zmk/behavior.h>
#include <zmk/split/peripheral.h>

#include <zmk/pointing_tools/platform/zmk/device_table.h>

LOG_MODULE_REGISTER(zpt_sensor_agent, CONFIG_ZMK_LOG_LEVEL);

/* Peripheral-half agent for split-safe CPI control. The central relays
 * requests through ZMK's INVOKE_BEHAVIOR command, and this behavior serves
 * them against the local device table. The outcome rides back as one packed
 * relative input event, reported straight to the split transport on the
 * agent's split-input channel, where the central proxy listens (see
 * docs/device-cpi-control.md for the wiring).
 *
 * Invocations arrive on the Bluetooth receive path, so serving (which talks
 * to the sensor over SPI) and reporting (which sends a notification) are
 * deferred to the system work queue. The central keeps one request
 * outstanding, so one pending slot is enough; a newer request replaces one
 * that hasn't run yet, because the central has stopped waiting for it. */

struct zpt_agent_config {
    uint8_t split_reg;
};

struct zpt_agent_request {
    uint8_t split_reg;
    uint32_t param1;
    uint32_t param2;
};

static struct k_spinlock zpt_agent_lock;
static struct zpt_agent_request zpt_agent_pending;

static void zpt_agent_serve(struct k_work *work) {
    ARG_UNUSED(work);

    k_spinlock_key_t key = k_spin_lock(&zpt_agent_lock);
    const struct zpt_agent_request request = zpt_agent_pending;
    k_spin_unlock(&zpt_agent_lock, key);

    const uint32_t frame = zpt_device_control_serve(request.param1, request.param2);
    /* Bypasses the channel's input processors, which shape sensor motion. */
    const struct zmk_split_transport_peripheral_event response = {
        .type = ZMK_SPLIT_TRANSPORT_PERIPHERAL_EVENT_TYPE_INPUT_EVENT,
        .data = {.input_event =
                     {
                         .reg = request.split_reg,
                         .type = INPUT_EV_REL,
                         .code = INPUT_REL_MISC,
                         .value = (int32_t)frame,
                         .sync = true,
                     }},
    };
    const int ret = zmk_split_peripheral_report_event(&response);
    if (ret < 0) {
        LOG_ERR("Failed to emit sensor-control response: %d", ret);
    }
}

static K_WORK_DEFINE(zpt_agent_work, zpt_agent_serve);

static int zpt_agent_pressed(struct zmk_behavior_binding *binding,
                             struct zmk_behavior_binding_event event) {
    ARG_UNUSED(event);
    const struct device *behavior = zmk_behavior_get_binding(binding->behavior_dev);
    if (behavior == NULL) {
        return -ENODEV;
    }

    const struct zpt_agent_config *config = behavior->config;
    k_spinlock_key_t key = k_spin_lock(&zpt_agent_lock);
    zpt_agent_pending = (struct zpt_agent_request){
        .split_reg = config->split_reg,
        .param1 = binding->param1,
        .param2 = binding->param2,
    };
    k_spin_unlock(&zpt_agent_lock, key);

    k_work_submit(&zpt_agent_work);
    return ZMK_BEHAVIOR_OPAQUE;
}

static const struct behavior_driver_api zpt_sensor_control_behavior_api = {
    .binding_pressed = zpt_agent_pressed,
};

/* ZMK's central subscribes reliably to only one zmk,input-split channel per
 * peripheral, so the agent answers on the channel that half already uses
 * for its sensor instead of needing one of its own. */
#define ZPT_SENSOR_CONTROL_DEFINE(inst)                                                            \
    BUILD_ASSERT(sizeof(DEVICE_DT_NAME(DT_DRV_INST(inst))) <= 9,                                   \
                 "ZMK's split relay carries at most 8 characters of behavior name");               \
    BUILD_ASSERT(DT_NODE_HAS_COMPAT(DT_INST_PHANDLE(inst, split_input), zmk_input_split),          \
                 "split-input must name a zmk,input-split node");                                  \
    static const struct zpt_agent_config zpt_agent_config_##inst = {                               \
        .split_reg = DT_REG_ADDR(DT_INST_PHANDLE(inst, split_input)),                              \
    };                                                                                             \
    BEHAVIOR_DT_INST_DEFINE(inst, NULL, NULL, NULL, &zpt_agent_config_##inst, POST_KERNEL,         \
                            CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,                                   \
                            &zpt_sensor_control_behavior_api);

DT_INST_FOREACH_STATUS_OKAY(ZPT_SENSOR_CONTROL_DEFINE)
