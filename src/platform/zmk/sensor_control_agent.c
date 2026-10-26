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

#include <zmk/pointing_tools/platform/zmk/device_table.h>

LOG_MODULE_REGISTER(zpt_sensor_agent, CONFIG_ZMK_LOG_LEVEL);

/* Peripheral-half agent for split-safe CPI control. The central relays
 * requests through ZMK's INVOKE_BEHAVIOR command, and this behavior serves
 * them against the local device table. The outcome rides back as one packed
 * relative input event reported by this behavior device; a zmk,input-split
 * node whose device is this behavior forwards it to the central proxy (see
 * docs/device-cpi-control.md for the wiring).
 *
 * Invocations arrive on the Bluetooth receive path, so serving (which talks
 * to the sensor over SPI) and reporting (which sends a notification) are
 * deferred to the system work queue. The central keeps one request
 * outstanding, so one pending slot is enough; a newer request replaces one
 * that hasn't run yet, because the central has stopped waiting for it. */

struct zpt_agent_request {
    const struct device *behavior;
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
    const int ret = input_report(request.behavior, INPUT_EV_REL, INPUT_REL_MISC, (int32_t)frame,
                                 true, K_NO_WAIT);
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

    k_spinlock_key_t key = k_spin_lock(&zpt_agent_lock);
    zpt_agent_pending = (struct zpt_agent_request){
        .behavior = behavior,
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

/* Some zmk,input-split node must name the agent as its device, or the
 * peripheral never forwards the answers and every request times out. */
#define ZPT_AGENT_FORWARDED_BY(split, agent)                                                       \
    /* NOLINTNEXTLINE(bugprone-macro-parentheses): unary-plus DT_FOREACH idiom */                  \
    +COND_CODE_1(DT_NODE_HAS_PROP(split, device),                                                  \
                 (DT_SAME_NODE(DT_PHANDLE(split, device), agent)), (0))

#define ZPT_SENSOR_CONTROL_DEFINE(inst)                                                            \
    BUILD_ASSERT(sizeof(DEVICE_DT_NAME(DT_DRV_INST(inst))) <= 9,                                   \
                 "ZMK's split relay carries at most 8 characters of behavior name");               \
    BUILD_ASSERT((0 DT_FOREACH_STATUS_OKAY_VARGS(zmk_input_split, ZPT_AGENT_FORWARDED_BY,          \
                                                 DT_DRV_INST(inst))) > 0,                          \
                 "add a zmk,input-split node whose device is the sensor-control agent");           \
    BEHAVIOR_DT_INST_DEFINE(inst, NULL, NULL, NULL, NULL, POST_KERNEL,                             \
                            CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,                                   \
                            &zpt_sensor_control_behavior_api);

DT_INST_FOREACH_STATUS_OKAY(ZPT_SENSOR_CONTROL_DEFINE)
