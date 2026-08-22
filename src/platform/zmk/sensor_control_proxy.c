/* SPDX-License-Identifier: MIT */
#define DT_DRV_COMPAT measlesbagel_zpt_input_processor_sensor_control_proxy

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/dt-bindings/input/input-event-codes.h>
#include <zephyr/input/input.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>

#include <drivers/behavior.h>
#include <drivers/input_processor.h>
#include <zmk/behavior.h>
#include <zmk/split/central.h>

#include <zmk/pointing_tools/platform/zmk/device_table.h>
#include <zmk/pointing_tools/platform/zmk/sensor_control_proxy.h>
#include <zmk/pointing_tools/source/sensor_control_wire.h>

LOG_MODULE_REGISTER(zpt_sensor_proxy, CONFIG_ZMK_LOG_LEVEL);

/* Central-half counterpart of the sensor-control agent: relays requests to
 * the owning peripheral through INVOKE_BEHAVIOR, decodes packed responses
 * from the reserved input-event channel, and correlates them by sequence
 * number. One request is outstanding at a time; the host protocol's flow
 * control already serializes callers. */

#define ZPT_PROXY_TIMEOUT K_MSEC(ZPT_SENSOR_CONTROL_TIMEOUT_MS)

struct zpt_sensor_proxy_config {
    const struct device *agent_device;
};

static struct zpt_sensor_proxy_state {
    const struct device *agent_device;
    struct k_sem response_ready;
    atomic_t expected_seq;
    atomic_t result_status;
    atomic_t result_value;
} zpt_sensor_proxy;

static int zpt_sensor_proxy_handle_event(const struct device *dev, struct input_event *event,
                                         uint32_t param1, uint32_t param2,
                                         struct zmk_input_processor_state *state) {
    ARG_UNUSED(dev);
    ARG_UNUSED(param1);
    ARG_UNUSED(param2);
    ARG_UNUSED(state);

    if (event->type != INPUT_EV_REL || event->code != INPUT_REL_MISC || !event->sync) {
        return ZMK_INPUT_PROC_CONTINUE;
    }

    uint8_t status = 0;
    uint8_t seq = 0;
    uint16_t value = 0;
    if (!zpt_scw_decode((uint32_t)event->value, &status, &seq, &value)) {
        LOG_DBG("Dropped unrecognized control frame 0x%08x", (uint32_t)event->value);
        return ZMK_INPUT_PROC_STOP;
    }

    const int expected = (int)atomic_get(&zpt_sensor_proxy.expected_seq);
    if (expected == 0 || seq != (uint8_t)expected) {
        /* Unsolicited or stale frame: count it as dropped noise. */
        return ZMK_INPUT_PROC_STOP;
    }

    atomic_set(&zpt_sensor_proxy.result_status, (atomic_t)status);
    atomic_set(&zpt_sensor_proxy.result_value, (atomic_t)value);
    atomic_set(&zpt_sensor_proxy.expected_seq, 0);
    k_sem_give(&zpt_sensor_proxy.response_ready);
    return ZMK_INPUT_PROC_STOP;
}

static const struct zmk_input_processor_driver_api zpt_sensor_proxy_driver_api = {
    .handle_event = zpt_sensor_proxy_handle_event,
};

static uint8_t zpt_sensor_proxy_next_seq(void) {
    static atomic_t seq_counter;
    const uint32_t next = atomic_inc(&seq_counter) + 1U;
    /* Sequence wraps at 16; exact-match correlation makes wrap harmless. */
    return (uint8_t)(next % 16u + 1u); /* never zero: zero marks "none" */
}

static int zpt_sensor_proxy_relay(const struct zpt_pointing_device *device, uint8_t opcode,
                                  uint16_t payload, uint16_t *value) {
    if (device->location == 0 || device->location > UINT8_MAX) {
        return -ENODEV;
    }

    const uint8_t seq = zpt_sensor_proxy_next_seq();
    struct zmk_behavior_binding binding = {
        .behavior_dev = zpt_sensor_proxy.agent_device->name,
        .param1 = (uint32_t)zpt_scw_param1(seq, opcode, device->id),
        .param2 = payload,
    };
    const struct zmk_behavior_binding_event event = {.position = 0, .timestamp = k_uptime_get()};

    atomic_set(&zpt_sensor_proxy.expected_seq, (atomic_t)seq);
    k_sem_reset(&zpt_sensor_proxy.response_ready);

    const int ret = zmk_split_central_invoke_behavior(device->location - 1U, &binding, event, true);

    if (ret < 0) {
        atomic_set(&zpt_sensor_proxy.expected_seq, 0);
        LOG_WRN("Sensor-control relay to half %u failed: %d", device->location - 1U, ret);
        return ret;
    }

    if (k_sem_take(&zpt_sensor_proxy.response_ready, ZPT_PROXY_TIMEOUT) != 0) {
        atomic_set(&zpt_sensor_proxy.expected_seq, 0);
        return -EAGAIN;
    }

    *value = (uint16_t)atomic_get(&zpt_sensor_proxy.result_value);
    return -(int)atomic_get(&zpt_sensor_proxy.result_status);
}

int zpt_sensor_control_preview_remote(const struct zpt_pointing_device *device, uint16_t requested,
                                      uint16_t *effective) {
    uint16_t snapped = requested;
    int ret = zpt_cpi_validate(&device->caps, requested, &snapped);
    if (ret < 0) {
        return ret;
    }

    uint16_t applied = 0;
    ret = zpt_sensor_proxy_relay(device, ZPT_SCW_OPCODE_SET_CPI, snapped, &applied);
    if (ret < 0) {
        return ret;
    }
    if (effective != NULL) {
        *effective = applied;
    }
    return 0;
}

int zpt_sensor_control_get_remote(const struct zpt_pointing_device *device, uint16_t *cpi) {
    uint16_t value = 0;
    int ret = zpt_sensor_proxy_relay(device, ZPT_SCW_OPCODE_GET_CPI, 0, &value);
    if (ret < 0) {
        /* Degrade to the compiled value when the half cannot be reached. */
        *cpi = device->default_cpi;
        return ret;
    }
    *cpi = value;
    return 0;
}

static int zpt_sensor_proxy_init(const struct device *dev) {
    ARG_UNUSED(dev);
    const struct zpt_sensor_proxy_config *config = dev->config;
    zpt_sensor_proxy.agent_device = config->agent_device;
    k_sem_init(&zpt_sensor_proxy.response_ready, 0, 1);
    atomic_set(&zpt_sensor_proxy.expected_seq, 0);
    LOG_INF("Pointing device control proxy ready");
    return 0;
}

#define ZPT_SENSOR_PROXY_DEFINE(inst)                                                              \
    static const struct zpt_sensor_proxy_config zpt_sensor_proxy_config_##inst = {                 \
        .agent_device = DEVICE_DT_GET(DT_INST_PHANDLE(inst, agent)),                               \
    };                                                                                             \
    DEVICE_DT_INST_DEFINE(inst, zpt_sensor_proxy_init, NULL, NULL,                                 \
                          &zpt_sensor_proxy_config_##inst, POST_KERNEL,                            \
                          CONFIG_APPLICATION_INIT_PRIORITY, &zpt_sensor_proxy_driver_api);

DT_INST_FOREACH_STATUS_OKAY(ZPT_SENSOR_PROXY_DEFINE)
