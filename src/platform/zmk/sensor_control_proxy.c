/* SPDX-License-Identifier: MIT */
#define DT_DRV_COMPAT measlesbagel_zpt_sensor_control_proxy

#include <errno.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/dt-bindings/input/input-event-codes.h>
#include <zephyr/input/input.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/spinlock.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>

#include <zmk/behavior.h>
#include <zmk/split/central.h>

#include <zmk/pointing_tools/platform/zmk/device_table.h>
#include <zmk/pointing_tools/platform/zmk/sensor_control_proxy.h>
#include <zmk/pointing_tools/source/sensor_control_session.h>
#include <zmk/pointing_tools/source/sensor_control_wire.h>

LOG_MODULE_REGISTER(zpt_sensor_proxy, CONFIG_ZMK_LOG_LEVEL);

/* Central counterpart of the sensor-control agent: relays requests to the
 * owning peripheral through INVOKE_BEHAVIOR, listens on every zmk,input-split
 * channel this central receives for the packed responses, and correlates
 * them by sequence number. One request is outstanding at a time; the host
 * protocol's flow control already serializes callers.
 *
 * A device with a split channel is owned by whichever half sends on that
 * channel, and ZMK numbers peripheral slots in pairing order, so the proxy
 * can't name the owner's slot. It sends the request to every slot instead,
 * and only accepts an answer that arrives on the device's channel. Legacy
 * entries name their slot through location and may answer on any channel. */

BUILD_ASSERT(DT_NUM_INST_STATUS_OKAY(DT_DRV_COMPAT) <= 1,
             "one sensor-control proxy serves every peripheral");

#define ZPT_PROXY_TIMEOUT K_MSEC(ZPT_SENSOR_CONTROL_TIMEOUT_MS)

/* Peripheral indices whose last relay succeeded, one bit per index. */
#define ZPT_PROXY_REACHABLE_BITS (sizeof(atomic_t) * 8U)

/* expected_reg value that accepts an answer on any channel. */
#define ZPT_PROXY_ANY_CHANNEL (-1)

static struct zpt_sensor_proxy_state {
    const char *agent_name;
    struct k_spinlock lock;
    struct zpt_scw_session session;
    /* The channel the outstanding request's answer must arrive on. */
    int16_t expected_reg;
    struct k_sem response_ready;
    atomic_t reachable;
} zpt_sensor_proxy;

static void zpt_sensor_proxy_receive(struct input_event *event, void *user_data) {
    const int16_t reg = (int16_t)(uintptr_t)user_data;

    if (event->type != INPUT_EV_REL || event->code != INPUT_REL_MISC || !event->sync) {
        return;
    }

    k_spinlock_key_t key = k_spin_lock(&zpt_sensor_proxy.lock);
    const int16_t expected = zpt_sensor_proxy.expected_reg;
    const bool answered = (expected == ZPT_PROXY_ANY_CHANNEL || expected == reg) &&
                          zpt_scw_session_offer(&zpt_sensor_proxy.session, (uint32_t)event->value);
    k_spin_unlock(&zpt_sensor_proxy.lock, key);

    if (answered) {
        k_sem_give(&zpt_sensor_proxy.response_ready);
    } else {
        LOG_DBG("Dropped stale or foreign control frame 0x%08x on channel %d",
                (uint32_t)event->value, reg);
    }
}

static void zpt_sensor_proxy_note_reachable(uint8_t source, bool reachable) {
    if (source >= ZPT_PROXY_REACHABLE_BITS) {
        return;
    }
    if (reachable) {
        atomic_set_bit(&zpt_sensor_proxy.reachable, source);
    } else {
        atomic_clear_bit(&zpt_sensor_proxy.reachable, source);
    }
}

static int zpt_sensor_proxy_relay(const struct zpt_pointing_device *device, uint8_t opcode,
                                  uint16_t payload, uint16_t *value) {
    if (zpt_sensor_proxy.agent_name == NULL) {
        return -ENODEV;
    }
    if (device->location == 0) {
        return -EINVAL;
    }
    const uint8_t source = device->location - 1U;

    k_sem_reset(&zpt_sensor_proxy.response_ready);
    k_spinlock_key_t key = k_spin_lock(&zpt_sensor_proxy.lock);
    const uint8_t seq = zpt_scw_session_begin(&zpt_sensor_proxy.session);
    zpt_sensor_proxy.expected_reg =
        device->has_split_channel ? (int16_t)device->split_reg : ZPT_PROXY_ANY_CHANNEL;
    k_spin_unlock(&zpt_sensor_proxy.lock, key);

    struct zmk_behavior_binding binding = {
        .behavior_dev = zpt_sensor_proxy.agent_name,
        .param1 = zpt_scw_param1(seq, opcode, device->id),
        .param2 = payload,
    };
    const struct zmk_behavior_binding_event event = {.position = 0, .timestamp = k_uptime_get()};
    /* ZMK queues the command and drops it if the half is disconnected, so
     * silence is the only sign of an unreachable half. */
    int ret;
    if (device->has_split_channel) {
        /* Halves that don't own the device stay silent, so asking every
         * slot reaches the owner without knowing which slot it paired as. */
        ret = -ENOTCONN;
        for (int slot = 0; slot < ZMK_SPLIT_CENTRAL_PERIPHERAL_COUNT; slot++) {
            if (zmk_split_central_invoke_behavior((uint8_t)slot, &binding, event, true) == 0) {
                ret = 0;
            }
        }
    } else {
        ret = zmk_split_central_invoke_behavior(source, &binding, event, true);
    }
    const bool answered =
        ret == 0 && k_sem_take(&zpt_sensor_proxy.response_ready, ZPT_PROXY_TIMEOUT) == 0;

    key = k_spin_lock(&zpt_sensor_proxy.lock);
    if (answered) {
        ret = zpt_scw_session_result(&zpt_sensor_proxy.session, value);
    } else {
        zpt_scw_session_cancel(&zpt_sensor_proxy.session);
        ret = ret < 0 ? ret : -EAGAIN;
    }
    zpt_sensor_proxy.expected_reg = ZPT_PROXY_ANY_CHANNEL;
    k_spin_unlock(&zpt_sensor_proxy.lock, key);

    /* An answer, even an error, shows the half is there. */
    zpt_sensor_proxy_note_reachable(source, answered);
    if (!answered) {
        LOG_WRN("Sensor-control relay to half %u failed: %d", source, ret);
    }
    return ret;
}

bool zpt_sensor_control_remote_reachable(const struct zpt_pointing_device *device) {
    if (device == NULL || device->location == 0) {
        return false;
    }
    const int source = (int)device->location - 1;
    return source < (int)ZPT_PROXY_REACHABLE_BITS &&
           atomic_test_bit(&zpt_sensor_proxy.reachable, source);
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
    /* Frames from that half now carry counts at the new resolution. */
    (void)zpt_device_control_record(device, applied);
    if (effective != NULL) {
        *effective = applied;
    }
    return 0;
}

int zpt_sensor_control_get_remote(const struct zpt_pointing_device *device, uint16_t *cpi) {
    uint16_t value = 0;
    int ret = zpt_sensor_proxy_relay(device, ZPT_SCW_OPCODE_GET_CPI, 0, &value);
    if (ret < 0) {
        /* Degrade to the last value this half knows about. */
        *cpi = zpt_device_current_cpi(device);
        return ret;
    }
    (void)zpt_device_control_record(device, value);
    *cpi = value;
    return 0;
}

/* The ZMK split relay carries at most eight characters of behavior name and
 * silently truncates longer ones, so the agent would never be found. */
#define ZPT_SENSOR_PROXY_AGENT_NAME(inst) DEVICE_DT_NAME(DT_INST_PHANDLE(inst, agent))

#define ZPT_SENSOR_PROXY_DEFINE(inst)                                                              \
    BUILD_ASSERT(sizeof(ZPT_SENSOR_PROXY_AGENT_NAME(inst)) <= 9,                                   \
                 "the sensor-control agent's node name must be at most 8 characters");             \
    static int zpt_sensor_proxy_init_##inst(void) {                                                \
        zpt_sensor_proxy.agent_name = ZPT_SENSOR_PROXY_AGENT_NAME(inst);                           \
        zpt_sensor_proxy.expected_reg = ZPT_PROXY_ANY_CHANNEL;                                     \
        k_sem_init(&zpt_sensor_proxy.response_ready, 0, 1);                                        \
        zpt_scw_session_init(&zpt_sensor_proxy.session);                                           \
        LOG_INF("Pointing device control proxy ready");                                            \
        return 0;                                                                                  \
    }                                                                                              \
    SYS_INIT(zpt_sensor_proxy_init_##inst, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

DT_INST_FOREACH_STATUS_OKAY(ZPT_SENSOR_PROXY_DEFINE)

/* Answers can come from any peripheral, so listen on every channel this
 * central receives. Each callback knows its channel's reg. The extra macro
 * layer expands the callback's name before INPUT_CALLBACK_DEFINE_NAMED
 * pastes it. */
#define ZPT_SENSOR_PROXY_CALLBACK(dev, user_data, name)                                            \
    INPUT_CALLBACK_DEFINE_NAMED(dev, zpt_sensor_proxy_receive, user_data, name)

#define ZPT_SENSOR_PROXY_LISTEN(node)                                                              \
    ZPT_SENSOR_PROXY_CALLBACK(DEVICE_DT_GET(node), (void *)(uintptr_t)DT_REG_ADDR(node),           \
                              DT_CAT(zpt_sensor_proxy_listen_, node));

DT_FOREACH_STATUS_OKAY(zmk_input_split, ZPT_SENSOR_PROXY_LISTEN)
