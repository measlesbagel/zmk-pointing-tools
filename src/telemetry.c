/* SPDX-License-Identifier: MIT */
#define DT_DRV_COMPAT measlesbagel_zpt_telemetry

#include <errno.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/ring_buffer.h>
#include <zephyr/sys/util.h>

#include <zmk/pointing_tools/trace.h>
#if IS_ENABLED(CONFIG_ZMK_POINTING_TOOLS_RUNTIME_TUNING)
#include <zmk/pointing_tools/tuning.h>
#endif

LOG_MODULE_REGISTER(zpt_telemetry, CONFIG_ZMK_LOG_LEVEL);

BUILD_ASSERT(DT_NUM_INST_STATUS_OKAY(DT_DRV_COMPAT) == 1,
             "exactly one measlesbagel,zpt-telemetry node is supported");

#if !DT_HAS_CHOSEN(measlesbagel_zpt_telemetry_uart)
#error "measlesbagel,zpt-telemetry-uart chosen node is required"
#endif

#define ZPT_PROTOCOL_VERSION (IS_ENABLED(CONFIG_ZMK_POINTING_TOOLS_RUNTIME_TUNING) ? 4 : 1)
#define ZPT_FRAME_MAGIC_0 0x5a
#define ZPT_FRAME_MAGIC_1 0x50

#define ZPT_REQ_DESCRIBE 0x01
#define ZPT_REQ_TELEMETRY 0x02
#define ZPT_REQ_PING 0x03
#define ZPT_REQ_TUNING_TARGETS 0x04
#define ZPT_REQ_TUNING_DESCRIBE 0x05
#define ZPT_REQ_TUNING_SET 0x06
#define ZPT_REQ_TUNING_RESET 0x07
#define ZPT_REQ_TUNING_HELP 0x08
#define ZPT_REQ_TUNING_TARGET_METADATA 0x09
#define ZPT_REQ_TUNING_PARAMETER_METADATA 0x0a
#define ZPT_REQ_TUNING_SET_MANY 0x0b
#define ZPT_RESP_DESCRIBE 0x81
#define ZPT_RESP_ACK 0x82
#define ZPT_RESP_TUNING_TARGETS 0x83
#define ZPT_RESP_TUNING_DESCRIBE 0x84
#define ZPT_RESP_TUNING_RESULT 0x85
#define ZPT_RESP_TUNING_HELP 0x86
#define ZPT_RESP_TUNING_TARGET_METADATA 0x87
#define ZPT_RESP_TUNING_PARAMETER_METADATA 0x88
#define ZPT_EVENT_SAMPLE 0x90

#define ZPT_MAX_REQUEST_PAYLOAD 128
#define ZPT_MAX_DESCRIBE_PAYLOAD 512
#define ZPT_TUNING_ALL_TARGETS UINT8_MAX
#define ZPT_TUNING_BATCH_HEADER_SIZE 2
#define ZPT_TUNING_BATCH_VALUE_SIZE 5
#define ZPT_TUNING_MAX_BATCH_VALUES 20

BUILD_ASSERT(ZPT_TUNING_BATCH_HEADER_SIZE +
                     ZPT_TUNING_MAX_BATCH_VALUES * ZPT_TUNING_BATCH_VALUE_SIZE <=
                 ZPT_MAX_REQUEST_PAYLOAD,
             "batch request exceeds parser payload capacity");

enum zpt_tuning_status {
    ZPT_TUNING_STATUS_OK = 0,
    ZPT_TUNING_STATUS_UNKNOWN_TARGET = 1,
    ZPT_TUNING_STATUS_UNKNOWN_PARAMETER = 2,
    ZPT_TUNING_STATUS_INVALID_VALUE = 3,
    ZPT_TUNING_STATUS_INTERNAL_ERROR = 4,
};

#define ZPT_UART_NODE DT_CHOSEN(measlesbagel_zpt_telemetry_uart)
static const struct device *const zpt_uart = DEVICE_DT_GET(ZPT_UART_NODE);

RING_BUF_DECLARE(zpt_rx_ring, 128);
K_MSGQ_DEFINE(zpt_sample_queue, sizeof(struct zpt_trace_sample),
              CONFIG_ZMK_POINTING_TOOLS_TELEMETRY_QUEUE_SIZE, 4);
K_SEM_DEFINE(zpt_wake, 0, 1);

static atomic_t zpt_enabled;
static atomic_t zpt_dropped;
static atomic_t zpt_sequence;
static atomic_t zpt_last_contact;

void zpt_telemetry_submit(const struct zpt_trace_sample *sample) {
    if (!atomic_get(&zpt_enabled)) {
        return;
    }

    struct zpt_trace_sample queued = *sample;
    queued.sequence = (uint32_t)atomic_inc(&zpt_sequence);
    if (k_msgq_put(&zpt_sample_queue, &queued, K_NO_WAIT) < 0) {
        atomic_inc(&zpt_dropped);
        return;
    }
    k_sem_give(&zpt_wake);
}

static void zpt_send_frame(uint8_t type, const uint8_t *payload, uint16_t length) {
    const uint8_t header[] = {ZPT_FRAME_MAGIC_0, ZPT_FRAME_MAGIC_1, type, (uint8_t)(length & 0xff),
                              (uint8_t)(length >> 8)};
    for (size_t i = 0; i < ARRAY_SIZE(header); i++) {
        uart_poll_out(zpt_uart, header[i]);
    }
    for (uint16_t i = 0; i < length; i++) {
        uart_poll_out(zpt_uart, payload[i]);
    }
}

static void zpt_send_describe(void) {
    uint8_t payload[ZPT_MAX_DESCRIBE_PAYLOAD];
    size_t offset = 0;
    const size_t count = zpt_trace_stream_count();

    payload[offset++] = ZPT_PROTOCOL_VERSION;
    payload[offset++] = MIN(count, UINT8_MAX);

    for (size_t i = 0; i < count && i < UINT8_MAX; i++) {
        const struct zpt_trace_descriptor *descriptor = zpt_trace_stream_descriptor(i);
        const size_t label_length = MIN(strlen(descriptor->label), UINT8_MAX);
        if (offset + 3 + label_length > sizeof(payload)) {
            LOG_WRN("Describe payload full after %u streams", (unsigned int)i);
            payload[1] = i;
            break;
        }
        payload[offset++] = descriptor->pointing_device_id;
        payload[offset++] = descriptor->stage;
        payload[offset++] = label_length;
        memcpy(&payload[offset], descriptor->label, label_length);
        offset += label_length;
    }

    zpt_send_frame(ZPT_RESP_DESCRIBE, payload, offset);
}

static void zpt_send_ack(void) {
    uint8_t payload[5];
    payload[0] = atomic_get(&zpt_enabled) ? 1 : 0;
    sys_put_le32((uint32_t)atomic_get(&zpt_dropped), &payload[1]);
    zpt_send_frame(ZPT_RESP_ACK, payload, sizeof(payload));
}

static void zpt_send_sample(const struct zpt_trace_sample *sample) {
    uint8_t payload[26];
    payload[0] = sample->pointing_device_id;
    payload[1] = sample->stage;
    sys_put_le32(sample->timestamp_ms, &payload[2]);
    sys_put_le32(sample->sequence, &payload[6]);
    sys_put_le32((uint32_t)sample->x, &payload[10]);
    sys_put_le32((uint32_t)sample->y, &payload[14]);
    sys_put_le32((uint32_t)sample->wheel, &payload[18]);
    sys_put_le32((uint32_t)sample->h_wheel, &payload[22]);
    zpt_send_frame(ZPT_EVENT_SAMPLE, payload, sizeof(payload));
}

#if IS_ENABLED(CONFIG_ZMK_POINTING_TOOLS_RUNTIME_TUNING)

static uint8_t zpt_tuning_status_from_errno(int error) {
    switch (error) {
    case 0:
        return ZPT_TUNING_STATUS_OK;
    case -ENODEV:
        return ZPT_TUNING_STATUS_UNKNOWN_TARGET;
    case -ENOENT:
        return ZPT_TUNING_STATUS_UNKNOWN_PARAMETER;
    case -EINVAL:
    case -ERANGE:
    case -EEXIST:
        return ZPT_TUNING_STATUS_INVALID_VALUE;
    default:
        return ZPT_TUNING_STATUS_INTERNAL_ERROR;
    }
}

static void zpt_send_tuning_result(uint8_t request_type, int error, uint8_t target_id,
                                   uint8_t parameter_id, int32_t value) {
    uint8_t payload[8] = {request_type, zpt_tuning_status_from_errno(error), target_id,
                          parameter_id};
    sys_put_le32((uint32_t)value, &payload[4]);
    zpt_send_frame(ZPT_RESP_TUNING_RESULT, payload, sizeof(payload));
}

static void zpt_send_tuning_targets(void) {
    uint8_t payload[ZPT_MAX_DESCRIBE_PAYLOAD];
    size_t offset = 0;
    const size_t count = MIN(zpt_tuning_target_count(), UINT8_MAX);

    payload[offset++] = count;
    for (size_t i = 0; i < count; i++) {
        const struct zpt_tuning_target *target = zpt_tuning_target_get(i);
        const size_t label_length = MIN(strlen(target->label), UINT8_MAX);
        if (offset + 3 + label_length > sizeof(payload)) {
            LOG_WRN("Tuning target payload full after %u targets", (unsigned int)i);
            payload[0] = i;
            break;
        }
        payload[offset++] = i;
        payload[offset++] = target->kind;
        payload[offset++] = label_length;
        memcpy(&payload[offset], target->label, label_length);
        offset += label_length;
    }
    zpt_send_frame(ZPT_RESP_TUNING_TARGETS, payload, offset);
}

static void zpt_send_tuning_description(uint8_t target_id) {
    const struct zpt_tuning_target *target = zpt_tuning_target_get(target_id);
    if (target == NULL) {
        zpt_send_tuning_result(ZPT_REQ_TUNING_DESCRIBE, -ENODEV, target_id, UINT8_MAX, 0);
        return;
    }

    uint8_t payload[ZPT_MAX_DESCRIBE_PAYLOAD];
    size_t offset = 0;
    payload[offset++] = target_id;
    payload[offset++] = MIN(target->parameter_count, UINT8_MAX);

    for (size_t i = 0; i < target->parameter_count && i < UINT8_MAX; i++) {
        const struct zpt_tuning_parameter *parameter = &target->parameters[i];
        const size_t label_length = MIN(strlen(parameter->label), UINT8_MAX);
        const size_t unit_length = MIN(strlen(parameter->unit), UINT8_MAX);
        const size_t encoded_length = 24 + label_length + unit_length;
        int32_t compiled;
        int32_t current;
        int ret = zpt_tuning_get(target_id, parameter->id, true, &compiled);
        if (ret == 0) {
            ret = zpt_tuning_get(target_id, parameter->id, false, &current);
        }
        if (ret < 0) {
            zpt_send_tuning_result(ZPT_REQ_TUNING_DESCRIBE, ret, target_id, parameter->id, 0);
            return;
        }
        if (offset + encoded_length > sizeof(payload)) {
            LOG_WRN("Tuning description full after %u parameters", (unsigned int)i);
            payload[1] = i;
            break;
        }

        payload[offset++] = parameter->id;
        payload[offset++] = parameter->type;
        sys_put_le32((uint32_t)parameter->minimum, &payload[offset]);
        offset += 4;
        sys_put_le32((uint32_t)parameter->maximum, &payload[offset]);
        offset += 4;
        sys_put_le32((uint32_t)parameter->step, &payload[offset]);
        offset += 4;
        sys_put_le32((uint32_t)compiled, &payload[offset]);
        offset += 4;
        sys_put_le32((uint32_t)current, &payload[offset]);
        offset += 4;
        payload[offset++] = label_length;
        payload[offset++] = unit_length;
        memcpy(&payload[offset], parameter->label, label_length);
        offset += label_length;
        memcpy(&payload[offset], parameter->unit, unit_length);
        offset += unit_length;
    }
    zpt_send_frame(ZPT_RESP_TUNING_DESCRIBE, payload, offset);
}

static void zpt_handle_tuning_set(const uint8_t *payload, uint16_t length) {
    if (length != 6) {
        zpt_send_tuning_result(ZPT_REQ_TUNING_SET, -EINVAL, UINT8_MAX, UINT8_MAX, 0);
        return;
    }

    const uint8_t target_id = payload[0];
    const uint8_t parameter_id = payload[1];
    const int32_t requested = (int32_t)sys_get_le32(&payload[2]);
    int ret = zpt_tuning_set(target_id, parameter_id, requested);
    int32_t current = requested;
    if (ret == 0) {
        ret = zpt_tuning_get(target_id, parameter_id, false, &current);
    }
    zpt_send_tuning_result(ZPT_REQ_TUNING_SET, ret, target_id, parameter_id, current);
}

static void zpt_handle_tuning_reset(const uint8_t *payload, uint16_t length) {
    if (length != 1) {
        zpt_send_tuning_result(ZPT_REQ_TUNING_RESET, -EINVAL, UINT8_MAX, UINT8_MAX, 0);
        return;
    }

    const uint8_t target_id = payload[0];
    int ret =
        target_id == ZPT_TUNING_ALL_TARGETS ? zpt_tuning_reset_all() : zpt_tuning_reset(target_id);
    zpt_send_tuning_result(ZPT_REQ_TUNING_RESET, ret, target_id, UINT8_MAX, 0);
}

static void zpt_send_tuning_help(uint8_t target_id, uint8_t parameter_id) {
    const struct zpt_tuning_target *target = zpt_tuning_target_get(target_id);
    if (target == NULL) {
        zpt_send_tuning_result(ZPT_REQ_TUNING_HELP, -ENODEV, target_id, parameter_id, 0);
        return;
    }

    const struct zpt_tuning_parameter *parameter = zpt_tuning_parameter_get(target, parameter_id);
    if (parameter == NULL) {
        zpt_send_tuning_result(ZPT_REQ_TUNING_HELP, -ENOENT, target_id, parameter_id, 0);
        return;
    }

    uint8_t payload[256];
    const size_t description_length = MIN(strlen(parameter->description), sizeof(payload) - 4);
    payload[0] = target_id;
    payload[1] = parameter_id;
    sys_put_le16((uint16_t)description_length, &payload[2]);
    memcpy(&payload[4], parameter->description, description_length);
    zpt_send_frame(ZPT_RESP_TUNING_HELP, payload, (uint16_t)(4 + description_length));
}

static void zpt_send_tuning_target_metadata(uint8_t target_id) {
    const struct zpt_tuning_target *target = zpt_tuning_target_get(target_id);
    if (target == NULL) {
        zpt_send_tuning_result(ZPT_REQ_TUNING_TARGET_METADATA, -ENODEV, target_id, UINT8_MAX, 0);
        return;
    }

    uint8_t payload[ZPT_MAX_DESCRIBE_PAYLOAD];
    const size_t stable_id_length = MIN(strlen(target->stable_id), UINT8_MAX);
    const size_t path_length = MIN(strlen(target->devicetree_path), UINT16_MAX);
    if (4 + stable_id_length + path_length > sizeof(payload)) {
        zpt_send_tuning_result(ZPT_REQ_TUNING_TARGET_METADATA, -ENOMEM, target_id, UINT8_MAX, 0);
        return;
    }

    payload[0] = target_id;
    payload[1] = stable_id_length;
    sys_put_le16((uint16_t)path_length, &payload[2]);
    memcpy(&payload[4], target->stable_id, stable_id_length);
    memcpy(&payload[4 + stable_id_length], target->devicetree_path, path_length);
    zpt_send_frame(ZPT_RESP_TUNING_TARGET_METADATA, payload,
                   (uint16_t)(4 + stable_id_length + path_length));
}

static void zpt_send_tuning_parameter_metadata(uint8_t target_id, uint8_t parameter_id) {
    const struct zpt_tuning_target *target = zpt_tuning_target_get(target_id);
    if (target == NULL) {
        zpt_send_tuning_result(ZPT_REQ_TUNING_PARAMETER_METADATA, -ENODEV, target_id, parameter_id,
                               0);
        return;
    }
    const struct zpt_tuning_parameter *parameter = zpt_tuning_parameter_get(target, parameter_id);
    if (parameter == NULL) {
        zpt_send_tuning_result(ZPT_REQ_TUNING_PARAMETER_METADATA, -ENOENT, target_id, parameter_id,
                               0);
        return;
    }

    uint8_t payload[ZPT_MAX_DESCRIBE_PAYLOAD];
    const size_t key_length = MIN(strlen(parameter->key), UINT8_MAX);
    const size_t property_length = MIN(strlen(parameter->devicetree_property), UINT8_MAX);
    if (4 + key_length + property_length > sizeof(payload)) {
        zpt_send_tuning_result(ZPT_REQ_TUNING_PARAMETER_METADATA, -ENOMEM, target_id, parameter_id,
                               0);
        return;
    }
    payload[0] = target_id;
    payload[1] = parameter_id;
    payload[2] = key_length;
    payload[3] = property_length;
    memcpy(&payload[4], parameter->key, key_length);
    memcpy(&payload[4 + key_length], parameter->devicetree_property, property_length);
    zpt_send_frame(ZPT_RESP_TUNING_PARAMETER_METADATA, payload,
                   (uint16_t)(4 + key_length + property_length));
}

static void zpt_handle_tuning_set_many(const uint8_t *payload, uint16_t length) {
    if (length < ZPT_TUNING_BATCH_HEADER_SIZE) {
        zpt_send_tuning_result(ZPT_REQ_TUNING_SET_MANY, -EINVAL, UINT8_MAX, UINT8_MAX, 0);
        return;
    }

    const uint8_t target_id = payload[0];
    const uint8_t count = payload[1];
    if (count == 0 || count > ZPT_TUNING_MAX_BATCH_VALUES ||
        length != ZPT_TUNING_BATCH_HEADER_SIZE + count * ZPT_TUNING_BATCH_VALUE_SIZE) {
        zpt_send_tuning_result(ZPT_REQ_TUNING_SET_MANY, -EINVAL, target_id, UINT8_MAX, 0);
        return;
    }

    struct zpt_tuning_value values[ZPT_TUNING_MAX_BATCH_VALUES];
    size_t offset = ZPT_TUNING_BATCH_HEADER_SIZE;
    for (size_t i = 0; i < count; i++) {
        values[i].parameter_id = payload[offset++];
        values[i].value = (int32_t)sys_get_le32(&payload[offset]);
        offset += 4;
    }

    uint8_t failed_parameter_id = UINT8_MAX;
    int ret = zpt_tuning_set_many(target_id, values, count, &failed_parameter_id);
    zpt_send_tuning_result(ZPT_REQ_TUNING_SET_MANY, ret, target_id,
                           ret == 0 ? UINT8_MAX : failed_parameter_id, ret == 0 ? count : 0);
}

#endif /* CONFIG_ZMK_POINTING_TOOLS_RUNTIME_TUNING */

static void zpt_dispatch(uint8_t type, const uint8_t *payload, uint16_t length) {
    switch (type) {
    case ZPT_REQ_DESCRIBE:
        atomic_set(&zpt_last_contact, k_uptime_get_32());
        zpt_send_describe();
        break;
    case ZPT_REQ_TELEMETRY:
        atomic_set(&zpt_last_contact, k_uptime_get_32());
        if (length == 1) {
            atomic_set(&zpt_enabled, payload[0] != 0);
            if (!payload[0]) {
                k_msgq_purge(&zpt_sample_queue);
            }
        }
        zpt_send_ack();
        break;
    case ZPT_REQ_PING:
        atomic_set(&zpt_last_contact, k_uptime_get_32());
        zpt_send_ack();
        break;
#if IS_ENABLED(CONFIG_ZMK_POINTING_TOOLS_RUNTIME_TUNING)
    case ZPT_REQ_TUNING_TARGETS:
        atomic_set(&zpt_last_contact, k_uptime_get_32());
        zpt_send_tuning_targets();
        break;
    case ZPT_REQ_TUNING_DESCRIBE:
        atomic_set(&zpt_last_contact, k_uptime_get_32());
        if (length == 1) {
            zpt_send_tuning_description(payload[0]);
        } else {
            zpt_send_tuning_result(type, -EINVAL, UINT8_MAX, UINT8_MAX, 0);
        }
        break;
    case ZPT_REQ_TUNING_SET:
        atomic_set(&zpt_last_contact, k_uptime_get_32());
        zpt_handle_tuning_set(payload, length);
        break;
    case ZPT_REQ_TUNING_RESET:
        atomic_set(&zpt_last_contact, k_uptime_get_32());
        zpt_handle_tuning_reset(payload, length);
        break;
    case ZPT_REQ_TUNING_HELP:
        atomic_set(&zpt_last_contact, k_uptime_get_32());
        if (length == 2) {
            zpt_send_tuning_help(payload[0], payload[1]);
        } else {
            zpt_send_tuning_result(type, -EINVAL, UINT8_MAX, UINT8_MAX, 0);
        }
        break;
    case ZPT_REQ_TUNING_TARGET_METADATA:
        atomic_set(&zpt_last_contact, k_uptime_get_32());
        if (length == 1) {
            zpt_send_tuning_target_metadata(payload[0]);
        } else {
            zpt_send_tuning_result(type, -EINVAL, UINT8_MAX, UINT8_MAX, 0);
        }
        break;
    case ZPT_REQ_TUNING_PARAMETER_METADATA:
        atomic_set(&zpt_last_contact, k_uptime_get_32());
        if (length == 2) {
            zpt_send_tuning_parameter_metadata(payload[0], payload[1]);
        } else {
            zpt_send_tuning_result(type, -EINVAL, UINT8_MAX, UINT8_MAX, 0);
        }
        break;
    case ZPT_REQ_TUNING_SET_MANY:
        atomic_set(&zpt_last_contact, k_uptime_get_32());
        zpt_handle_tuning_set_many(payload, length);
        break;
#endif
    default:
        LOG_WRN("Unknown telemetry request 0x%02x", type);
        break;
    }
}

static void zpt_parse_byte(uint8_t byte) {
    static enum { MAGIC_0, MAGIC_1, TYPE, LENGTH_0, LENGTH_1, PAYLOAD } state = MAGIC_0;
    static uint8_t type;
    static uint16_t length;
    static uint16_t received;
    static uint8_t payload[ZPT_MAX_REQUEST_PAYLOAD];

    switch (state) {
    case MAGIC_0:
        if (byte == ZPT_FRAME_MAGIC_0) {
            state = MAGIC_1;
        }
        break;
    case MAGIC_1:
        state = byte == ZPT_FRAME_MAGIC_1 ? TYPE : (byte == ZPT_FRAME_MAGIC_0 ? MAGIC_1 : MAGIC_0);
        break;
    case TYPE:
        type = byte;
        state = LENGTH_0;
        break;
    case LENGTH_0:
        length = byte;
        state = LENGTH_1;
        break;
    case LENGTH_1:
        length |= (uint16_t)byte << 8;
        received = 0;
        if (length > sizeof(payload)) {
            state = MAGIC_0;
        } else if (length == 0) {
            zpt_dispatch(type, NULL, 0);
            state = MAGIC_0;
        } else {
            state = PAYLOAD;
        }
        break;
    case PAYLOAD:
        payload[received++] = byte;
        if (received == length) {
            zpt_dispatch(type, payload, length);
            state = MAGIC_0;
        }
        break;
    }
}

static void zpt_uart_callback(const struct device *dev, void *user_data) {
    ARG_UNUSED(user_data);
    if (!uart_irq_update(dev)) {
        return;
    }

    while (uart_irq_rx_ready(dev)) {
        uint8_t bytes[32];
        const int count = uart_fifo_read(dev, bytes, sizeof(bytes));
        if (count <= 0) {
            break;
        }
        const uint32_t written = ring_buf_put(&zpt_rx_ring, bytes, count);
        if (written < (uint32_t)count) {
            LOG_WRN("Dropped %u serial input bytes", (unsigned int)(count - written));
        }
    }
    k_sem_give(&zpt_wake);
}

static void zpt_thread(void) {
    for (;;) {
        k_sem_take(&zpt_wake, K_FOREVER);

        uint8_t byte;
        while (ring_buf_get(&zpt_rx_ring, &byte, 1) == 1) {
            zpt_parse_byte(byte);
        }

        struct zpt_trace_sample sample;
        while (k_msgq_get(&zpt_sample_queue, &sample, K_NO_WAIT) == 0) {
            const uint32_t elapsed = k_uptime_get_32() - (uint32_t)atomic_get(&zpt_last_contact);
            if (elapsed > CONFIG_ZMK_POINTING_TOOLS_TELEMETRY_HOST_TIMEOUT_MS) {
                atomic_clear(&zpt_enabled);
                k_msgq_purge(&zpt_sample_queue);
                break;
            }
            if (atomic_get(&zpt_enabled)) {
                zpt_send_sample(&sample);
            }
        }
    }
}

K_THREAD_DEFINE(zpt_thread_id, CONFIG_ZMK_POINTING_TOOLS_TELEMETRY_THREAD_STACK_SIZE, zpt_thread,
                NULL, NULL, NULL, K_PRIO_PREEMPT(10), 0, 0);

static int zpt_telemetry_init(void) {
    if (!device_is_ready(zpt_uart)) {
        return -ENODEV;
    }

    const int error = uart_irq_callback_user_data_set(zpt_uart, zpt_uart_callback, NULL);
    if (error < 0) {
        return error;
    }
    uart_irq_rx_enable(zpt_uart);
    LOG_INF("Pointing telemetry ready with %u streams", (unsigned int)zpt_trace_stream_count());
    return 0;
}

SYS_INIT(zpt_telemetry_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
