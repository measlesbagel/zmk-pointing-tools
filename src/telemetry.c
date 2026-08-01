/* SPDX-License-Identifier: MIT */
#define DT_DRV_COMPAT measlesbagel_zpt_telemetry

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

LOG_MODULE_REGISTER(zpt_telemetry, CONFIG_ZMK_LOG_LEVEL);

BUILD_ASSERT(DT_NUM_INST_STATUS_OKAY(DT_DRV_COMPAT) == 1,
             "exactly one measlesbagel,zpt-telemetry node is supported");

#if !DT_HAS_CHOSEN(measlesbagel_zpt_telemetry_uart)
#error "measlesbagel,zpt-telemetry-uart chosen node is required"
#endif

#define ZPT_PROTOCOL_VERSION 1
#define ZPT_FRAME_MAGIC_0 0x5a
#define ZPT_FRAME_MAGIC_1 0x50

#define ZPT_REQ_DESCRIBE 0x01
#define ZPT_REQ_TELEMETRY 0x02
#define ZPT_REQ_PING 0x03
#define ZPT_RESP_DESCRIBE 0x81
#define ZPT_RESP_ACK 0x82
#define ZPT_EVENT_SAMPLE 0x90

#define ZPT_MAX_REQUEST_PAYLOAD 8
#define ZPT_MAX_DESCRIBE_PAYLOAD 512

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
