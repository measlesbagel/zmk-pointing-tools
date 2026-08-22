/* SPDX-License-Identifier: MIT */

/* Unit tests for the USB-serial telemetry service (src/service/telemetry.c)
 * over the virtual UART driver (vnd,serial,
 * zephyr/drivers/serial/serial_test.c): host requests are injected into
 * the RX queue, service responses are captured from the TX buffer.
 *
 * The wire protocol constants below are the public protocol (v6); they
 * mirror src/service/telemetry.c, which keeps its own private copies.
 *
 * All cases of this binary belong to the single zpt_unit suite (defined in
 * test_tuning.c): ztest executes suites and cases in name-sorted linker
 * order, and the tuning registry is a process-wide singleton with no
 * unregister API, so name is the only reliable execution order. The
 * capacity-fill case is named z_capacity_fill so it runs last of all. */

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/ztest.h>
#include <zephyr/drivers/uart/serial_test.h>

#include <zmk/pointing_tools/observer/state.h>
#include <zmk/pointing_tools/platform/zmk/device_table.h>
#include <zmk/pointing_tools/service/tuning.h>

/* --- Wire protocol (v6) ---------------------------------------------- */

#define ZPT_PROTOCOL_VERSION 7
#define ZPT_FRAME_MAGIC_0 0x5a
#define ZPT_FRAME_MAGIC_1 0x50

#define ZPT_REQ_DESCRIBE 0x01
#define ZPT_REQ_PING 0x03
#define ZPT_REQ_TUNING_TARGETS 0x04
#define ZPT_REQ_TUNING_DESCRIBE 0x05
#define ZPT_REQ_TUNING_SET 0x06
#define ZPT_REQ_TUNING_RESET 0x07
#define ZPT_REQ_TUNING_HELP 0x08
#define ZPT_REQ_TUNING_TARGET_METADATA 0x09
#define ZPT_REQ_TUNING_PARAMETER_METADATA 0x0a
#define ZPT_REQ_TUNING_SET_MANY 0x0b
#define ZPT_REQ_STATE_CONTROL 0x0c
#define ZPT_RESP_DESCRIBE 0x81
#define ZPT_RESP_ACK 0x82
#define ZPT_RESP_TUNING_TARGETS 0x83
#define ZPT_RESP_TUNING_DESCRIBE 0x84
#define ZPT_RESP_TUNING_RESULT 0x85
#define ZPT_RESP_TUNING_HELP 0x86
#define ZPT_RESP_TUNING_TARGET_METADATA 0x87
#define ZPT_RESP_TUNING_PARAMETER_METADATA 0x88
#define ZPT_RESP_STATE_STATUS 0x89
#define ZPT_EVENT_STATE 0x91

#define ZPT_TUNING_STATUS_OK 0
#define ZPT_TUNING_STATUS_UNKNOWN_TARGET 1
#define ZPT_TUNING_STATUS_UNKNOWN_PARAMETER 2
#define ZPT_TUNING_STATUS_INVALID_VALUE 3
#define ZPT_TUNING_STATUS_INTERNAL_ERROR 4

#define ZPT_MAX_REQUEST_PAYLOAD 128
#define ZPT_MAX_RESPONSE_PAYLOAD 512
#define ZPT_RX_TIMEOUT_MS 2000
#define ZPT_STATE_SCHEMA_VERSION 2

/* Record queue owned by the service; the drop test fills it directly.
 * Same layout as the private struct zpt_telemetry_record in
 * src/service/telemetry.c (kind byte, then a one-member union). */
extern struct k_msgq zpt_record_queue;

struct zpt_test_record {
    uint8_t kind;
    union {
        struct zpt_state_sample state;
    } sample;
};

#define ZPT_TEST_UART DT_NODELABEL(zpt_test_uart)
/* Exposed so the shared suite setup (test_tuning.c) can assert readiness. */
const struct device *const uart_dev = DEVICE_DT_GET(ZPT_TEST_UART);

/* --- Frame helpers ------------------------------------------------------ */

static void tx_raw(const uint8_t *data, size_t length) {
    zassert_equal(serial_vnd_queue_in_data(uart_dev, data, length), (int)length);
}

static void tx_frame(uint8_t type, const uint8_t *payload, uint16_t length) {
    uint8_t frame[5 + ZPT_MAX_REQUEST_PAYLOAD];

    frame[0] = ZPT_FRAME_MAGIC_0;
    frame[1] = ZPT_FRAME_MAGIC_1;
    frame[2] = type;
    frame[3] = (uint8_t)(length & 0xff);
    frame[4] = (uint8_t)(length >> 8);
    memcpy(&frame[5], payload, length);
    tx_raw(frame, 5 + length);
}

static int rx_frame(uint8_t *type, uint8_t *payload, uint16_t *length, uint32_t timeout_ms) {
    uint32_t deadline = (uint32_t)k_uptime_get_32() + timeout_ms;

    for (;;) {
        if (serial_vnd_out_data_size_get(uart_dev) >= 5) {
            uint8_t header[5];
            uint16_t frame_length;

            serial_vnd_read_out_data(uart_dev, header, 5);
            if (header[0] != ZPT_FRAME_MAGIC_0 || header[1] != ZPT_FRAME_MAGIC_1) {
                return -EPROTO;
            }
            frame_length = (uint16_t)(header[3] | (uint16_t)header[4] << 8);
            if (frame_length > ZPT_MAX_RESPONSE_PAYLOAD) {
                return -EPROTO;
            }
            /* The header is already out of the buffer, so only the payload
             * bytes remain to be counted. */
            while (serial_vnd_out_data_size_get(uart_dev) < frame_length) {
                if ((uint32_t)k_uptime_get_32() > deadline) {
                    return -ETIMEDOUT;
                }
                k_sleep(K_MSEC(1));
            }
            *type = header[2];
            serial_vnd_read_out_data(uart_dev, payload, frame_length);
            *length = frame_length;
            return 0;
        }
        if ((uint32_t)k_uptime_get_32() > deadline) {
            return -ETIMEDOUT;
        }
        /* Sleep (not busy-wait): the ztest thread has higher priority than
         * the telemetry service thread, so a busy loop would starve the
         * service and the expected frame would never be produced. */
        k_sleep(K_MSEC(1));
    }
}

static void expect_frame(uint8_t type, const uint8_t *payload, uint16_t length) {
    uint8_t payload_buffer[ZPT_MAX_RESPONSE_PAYLOAD];
    uint8_t frame_type;
    uint16_t frame_length;

    zassert_equal(rx_frame(&frame_type, payload_buffer, &frame_length, ZPT_RX_TIMEOUT_MS), 0);
    zassert_equal(frame_type, type);
    zassert_equal(frame_length, length);
    zassert_mem_equal(payload_buffer, payload, length);
}

static void expect_no_frame(uint32_t timeout_ms) {
    uint32_t deadline = (uint32_t)k_uptime_get_32() + timeout_ms;

    while ((uint32_t)k_uptime_get_32() < deadline) {
        zassert_equal(serial_vnd_out_data_size_get(uart_dev), 0);
        k_sleep(K_MSEC(1));
    }
}

/* Walk a 0x89 state-status payload; returns the entry for target_id
 * ([id, level, label_length, label...]) or NULL. */
static const uint8_t *state_status_entry(const uint8_t *payload, uint8_t target_id) {
    size_t offset = 8;

    for (uint8_t i = 0; i < payload[7]; i++) {
        if (payload[offset] == target_id) {
            return &payload[offset];
        }
        offset += 3 + payload[offset + 2];
    }
    return NULL;
}

ZTEST(zpt_unit, describe) {
    uint8_t payload[1] = {ZPT_PROTOCOL_VERSION};

    tx_frame(ZPT_REQ_DESCRIBE, NULL, 0);
    expect_frame(ZPT_RESP_DESCRIBE, payload, 1);
}

/* --- Pointing device discovery and preview (protocol v7) ------------- */

/* Wire copies of the v7 device messages, mirroring src/service/telemetry.c. */
#define ZPT_TEST_REQ_DEVICE_LIST 0x0d
#define ZPT_TEST_REQ_DEVICE_DESCRIBE 0x0e
#define ZPT_TEST_REQ_DEVICE_PREVIEW 0x0f
#define ZPT_TEST_RESP_DEVICE_LIST 0x8a
#define ZPT_TEST_RESP_DEVICE_DESCRIPTION 0x8b

ZTEST(zpt_unit, zz_device_list_reports_table) {
    /* The overlay declares three entries: local discrete, peripheral-one
     * range, and a read-only pad. Flags: bit0 local-connected, bit1 has
     * settable capabilities. */
    uint8_t expected[] = {
        3,   0,   0,   0x03, 20,  't', 'e', 's', 't', '-',  'l', 'o', 'c', 'a', 'l', '-', 't',  'r',
        'a', 'c', 'k', 'b',  'a', 'l', 'l', 1,   1,   0x02, 21,  't', 'e', 's', 't', '-', 'r',  'e',
        'm', 'o', 't', 'e',  '-', 't', 'r', 'a', 'c', 'k',  'b', 'a', 'l', 'l', 2,   0,   0x01, 17,
        't', 'e', 's', 't',  '-', 'f', 'i', 'x', 'e', 'd',  '-', 'n', 'u', 'm', 'p', 'a', 'd',
    };

    tx_frame(ZPT_TEST_REQ_DEVICE_LIST, NULL, 0);
    expect_frame(ZPT_TEST_RESP_DEVICE_LIST, expected, sizeof(expected));
}

ZTEST(zpt_unit, zz_device_describe_reports_identity_and_capabilities) {
    uint8_t request[1] = {0};
    static const char stable_id[] = "test-local-trackball";
    static const char dt_path[] = "/trackball-local";
    static const uint16_t values[] = {200, 400, 800, 1600};
    uint8_t expected[64];
    size_t offset = 0;

    expected[offset++] = (uint8_t)(sizeof(stable_id) - 1);
    memcpy(&expected[offset], stable_id, sizeof(stable_id) - 1);
    offset += sizeof(stable_id) - 1;
    expected[offset++] = (uint8_t)((sizeof(dt_path) - 1) & 0xff);
    expected[offset++] = (uint8_t)(((sizeof(dt_path) - 1) >> 8) & 0xff);
    memcpy(&expected[offset], dt_path, sizeof(dt_path) - 1);
    offset += sizeof(dt_path) - 1;
    for (int i = 0; i < 2; i++) { /* current, then compiled default */
        expected[offset++] = 800 & 0xff;
        expected[offset++] = 800 >> 8;
    }
    expected[offset++] = 0x01; /* settable */
    expected[offset++] = (uint8_t)(sizeof(values) / sizeof(values[0]));
    for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
        expected[offset++] = values[i] & 0xff;
        expected[offset++] = values[i] >> 8;
    }

    tx_frame(ZPT_TEST_REQ_DEVICE_DESCRIBE, request, sizeof(request));
    expect_frame(ZPT_TEST_RESP_DEVICE_DESCRIPTION, expected, offset);
}

ZTEST(zpt_unit, zz_device_describe_unknown_id_fails_cleanly) {
    uint8_t request[1] = {200};

    tx_frame(ZPT_TEST_REQ_DEVICE_DESCRIBE, request, sizeof(request));
    /* Tuning result shape: request-type, status (unknown target = 1),
     * target-id sentinel, parameter-id carries the device id, value 0. */
    uint8_t expected[] = {ZPT_TEST_REQ_DEVICE_DESCRIBE, 1, UINT8_MAX, 200, 0, 0, 0, 0};
    expect_frame(ZPT_RESP_TUNING_RESULT, expected, sizeof(expected));
}

ZTEST(zpt_unit, zz_device_preview_snaps_and_persists) {
    const struct zpt_pointing_device *local;
    const struct zpt_pointing_device *remote;
    uint8_t request[3];
    uint8_t expected[8];

    local = zpt_device_table_find("test-local-trackball");
    remote = zpt_device_table_find("test-remote-trackball");
    zassert_not_null(local);
    zassert_not_null(remote);

    /* Local device, off-list request: snaps and reports the effective value
     * with an OK status; the host compares requested against effective. */
    memcpy(request, (uint8_t[]){0, 0xe8, 0x03}, 3); /* 1000 -> snaps to 800 */
    tx_frame(ZPT_TEST_REQ_DEVICE_PREVIEW, request, sizeof(request));
    memcpy(expected, (uint8_t[]){ZPT_TEST_REQ_DEVICE_PREVIEW, 0, UINT8_MAX, 0, 0x20, 0x03, 0, 0},
           sizeof(expected)); /* status ok, value 800 */
    expect_frame(ZPT_RESP_TUNING_RESULT, expected, sizeof(expected));

    /* The preview persists in the store until reset or reboot. */
    uint16_t cpi = 0;
    zassert_ok(zpt_device_control_get(local, &cpi));
    zassert_equal(cpi, 800);

    /* Remote-owned entries need the sensor-control tunnel, which this
     * plain-Zephyr build does not compile: rejected as not applicable. */
    memcpy(request, (uint8_t[]){1, 0xb9, 0x02}, 3); /* 697 */
    tx_frame(ZPT_TEST_REQ_DEVICE_PREVIEW, request, sizeof(request));
    memcpy(expected, (uint8_t[]){ZPT_TEST_REQ_DEVICE_PREVIEW, 3, UINT8_MAX, 1, 0, 0, 0, 0},
           sizeof(expected)); /* status invalid value, value 0 */
    expect_frame(ZPT_RESP_TUNING_RESULT, expected, sizeof(expected));
    zassert_ok(zpt_device_control_get(remote, &cpi));
    zassert_equal(cpi, 400);

    /* Restore the compiled value so suite order stays irrelevant. */
    zassert_ok(zpt_device_control_reset(local));
}

ZTEST(zpt_unit, ping) {
    uint8_t frame_type;
    uint8_t payload[ZPT_MAX_RESPONSE_PAYLOAD];
    uint16_t length;

    tx_frame(ZPT_REQ_PING, NULL, 0);
    zassert_equal(rx_frame(&frame_type, payload, &length, ZPT_RX_TIMEOUT_MS), 0);
    zassert_equal(frame_type, ZPT_RESP_ACK);
    zassert_equal(length, 4);
    zassert_equal(sys_get_le32(&payload[0]), 0u);
}

ZTEST(zpt_unit, parser_resync) {
    /* Garbage, a broken magic pair, and a stray 0x5a before a valid ping:
     * the parser must resynchronize and dispatch exactly one request. */
    const uint8_t stream[] = {0x00,         0xff, 0x5a, 0x00, 0x5a, 0x5a, ZPT_FRAME_MAGIC_1,
                              ZPT_REQ_PING, 0x00, 0x00};
    uint8_t frame_type;
    uint8_t payload[ZPT_MAX_RESPONSE_PAYLOAD];
    uint16_t length;

    tx_raw(stream, sizeof(stream));
    zassert_equal(rx_frame(&frame_type, payload, &length, ZPT_RX_TIMEOUT_MS), 0);
    zassert_equal(frame_type, ZPT_RESP_ACK);
    expect_no_frame(300);
}

ZTEST(zpt_unit, oversized_length) {
    /* A 256-byte length exceeds the 128-byte parser buffer: the frame is
     * discarded and the parser stays usable. */
    const uint8_t oversized[] = {ZPT_FRAME_MAGIC_0, ZPT_FRAME_MAGIC_1, ZPT_REQ_PING, 0x00, 0x01};
    uint8_t frame_type;
    uint8_t payload[ZPT_MAX_RESPONSE_PAYLOAD];
    uint16_t length;

    tx_raw(oversized, sizeof(oversized));
    expect_no_frame(300);
    tx_frame(ZPT_REQ_PING, NULL, 0);
    zassert_equal(rx_frame(&frame_type, payload, &length, ZPT_RX_TIMEOUT_MS), 0);
    zassert_equal(frame_type, ZPT_RESP_ACK);
}

ZTEST(zpt_unit, unknown_request) {
    /* Unknown request types are dropped without a response. */
    uint8_t frame_type;
    uint8_t payload[ZPT_MAX_RESPONSE_PAYLOAD];
    uint16_t length;

    tx_frame(0x20, NULL, 0);
    expect_no_frame(300);
    tx_frame(ZPT_REQ_PING, NULL, 0);
    zassert_equal(rx_frame(&frame_type, payload, &length, ZPT_RX_TIMEOUT_MS), 0);
    zassert_equal(frame_type, ZPT_RESP_ACK);
}

ZTEST(zpt_unit, tuning_targets) {
    uint8_t frame_type;
    uint8_t payload[ZPT_MAX_RESPONSE_PAYLOAD];
    uint16_t length;
    size_t offset;

    tx_frame(ZPT_REQ_TUNING_TARGETS, NULL, 0);
    zassert_equal(rx_frame(&frame_type, payload, &length, ZPT_RX_TIMEOUT_MS), 0);
    zassert_equal(frame_type, ZPT_RESP_TUNING_TARGETS);
    zassert_equal(payload[0] >= 2, 1);

    offset = 1;
    for (uint8_t i = 0; i < 2; i++) {
        zassert_equal(payload[offset], i);
        zassert_equal(payload[offset + 1], 4); /* ZPT_TUNING_TARGET_PIPELINE_STAGE */
        const char *expected = i == 0 ? "Target A" : "Target B";
        zassert_equal(payload[offset + 2], (uint8_t)strlen(expected));
        zassert_mem_equal(&payload[offset + 3], expected, payload[offset + 2]);
        offset += 3 + payload[offset + 2];
    }
}

ZTEST(zpt_unit, tuning_describe) {
    uint8_t frame_type;
    uint8_t payload[ZPT_MAX_RESPONSE_PAYLOAD];
    uint16_t length;
    size_t offset;

    /* The tuning suite left target A at gain=10 (compiled), enabled=1. */
    tx_frame(ZPT_REQ_TUNING_DESCRIBE, (const uint8_t[1]){0}, 1);
    zassert_equal(rx_frame(&frame_type, payload, &length, ZPT_RX_TIMEOUT_MS), 0);
    zassert_equal(frame_type, ZPT_RESP_TUNING_DESCRIBE);
    zassert_equal(payload[0], 0);
    zassert_equal(payload[1], 2);

    offset = 2;
    for (uint8_t i = 0; i < 2; i++) {
        zassert_equal(payload[offset], i);
        zassert_equal(payload[offset + 1],
                      i == 0 ? ZPT_TUNING_VALUE_INTEGER : ZPT_TUNING_VALUE_BOOLEAN);
        zassert_equal(sys_get_le32(&payload[offset + 2]), i == 0 ? 0 : 0);
        zassert_equal(sys_get_le32(&payload[offset + 6]), i == 0 ? 100 : 1);
        zassert_equal(sys_get_le32(&payload[offset + 10]), i == 0 ? 5 : 1);
        zassert_equal(sys_get_le32(&payload[offset + 14]), i == 0 ? 10 : 1);
        zassert_equal(sys_get_le32(&payload[offset + 18]), i == 0 ? 10 : 1);
        const char *label = i == 0 ? "Gain" : "Enabled";
        const char *unit = i == 0 ? "cpi" : "";
        zassert_equal(payload[offset + 22], (uint8_t)strlen(label));
        zassert_equal(payload[offset + 23], (uint8_t)strlen(unit));
        zassert_mem_equal(&payload[offset + 24], label, strlen(label));
        zassert_mem_equal(&payload[offset + 24 + strlen(label)], unit, strlen(unit));
        offset += 24 + strlen(label) + strlen(unit);
    }
    zassert_equal(offset, length);

    /* Unknown target. */
    tx_frame(ZPT_REQ_TUNING_DESCRIBE, (const uint8_t[1]){15}, 1);
    zassert_equal(rx_frame(&frame_type, payload, &length, ZPT_RX_TIMEOUT_MS), 0);
    zassert_equal(frame_type, ZPT_RESP_TUNING_RESULT);
    zassert_equal(payload[0], ZPT_REQ_TUNING_DESCRIBE);
    zassert_equal(payload[1], ZPT_TUNING_STATUS_UNKNOWN_TARGET);
    zassert_equal(payload[2], 15);
    zassert_equal(payload[3], 0xff);

    /* Bad length. */
    tx_frame(ZPT_REQ_TUNING_DESCRIBE, NULL, 0);
    zassert_equal(rx_frame(&frame_type, payload, &length, ZPT_RX_TIMEOUT_MS), 0);
    zassert_equal(frame_type, ZPT_RESP_TUNING_RESULT);
    zassert_equal(payload[1], ZPT_TUNING_STATUS_INVALID_VALUE);
}

ZTEST(zpt_unit, tuning_set) {
    uint8_t frame_type;
    uint8_t payload[ZPT_MAX_RESPONSE_PAYLOAD];
    uint16_t length;

    /* Success: response carries the re-read current value. */
    uint8_t set_ok[6] = {0, 0, 25, 0, 0, 0};
    tx_frame(ZPT_REQ_TUNING_SET, set_ok, 6);
    zassert_equal(rx_frame(&frame_type, payload, &length, ZPT_RX_TIMEOUT_MS), 0);
    zassert_equal(frame_type, ZPT_RESP_TUNING_RESULT);
    zassert_equal(payload[0], ZPT_REQ_TUNING_SET);
    zassert_equal(payload[1], ZPT_TUNING_STATUS_OK);
    zassert_equal(payload[2], 0);
    zassert_equal(payload[3], 0);
    zassert_equal(sys_get_le32(&payload[4]), 25);

    /* Out of range. */
    uint8_t set_bad[6] = {0, 0, 105, 0, 0, 0};
    tx_frame(ZPT_REQ_TUNING_SET, set_bad, 6);
    zassert_equal(rx_frame(&frame_type, payload, &length, ZPT_RX_TIMEOUT_MS), 0);
    zassert_equal(payload[1], ZPT_TUNING_STATUS_INVALID_VALUE);

    /* Unknown target and parameter. */
    uint8_t set_unknown[6] = {99, 0, 1, 0, 0, 0};
    tx_frame(ZPT_REQ_TUNING_SET, set_unknown, 6);
    zassert_equal(rx_frame(&frame_type, payload, &length, ZPT_RX_TIMEOUT_MS), 0);
    zassert_equal(payload[1], ZPT_TUNING_STATUS_UNKNOWN_TARGET);
    uint8_t set_unknown_param[6] = {0, 99, 1, 0, 0, 0};
    tx_frame(ZPT_REQ_TUNING_SET, set_unknown_param, 6);
    zassert_equal(rx_frame(&frame_type, payload, &length, ZPT_RX_TIMEOUT_MS), 0);
    zassert_equal(payload[1], ZPT_TUNING_STATUS_UNKNOWN_PARAMETER);

    /* Bad length. */
    tx_frame(ZPT_REQ_TUNING_SET, (const uint8_t[5]){0, 0, 1, 0, 0}, 5);
    zassert_equal(rx_frame(&frame_type, payload, &length, ZPT_RX_TIMEOUT_MS), 0);
    zassert_equal(payload[1], ZPT_TUNING_STATUS_INVALID_VALUE);
}

ZTEST(zpt_unit, tuning_reset) {
    uint8_t frame_type;
    uint8_t payload[ZPT_MAX_RESPONSE_PAYLOAD];
    uint16_t length;

    /* Reset target A (its fake reset returns 0). */
    tx_frame(ZPT_REQ_TUNING_RESET, (const uint8_t[1]){0}, 1);
    zassert_equal(rx_frame(&frame_type, payload, &length, ZPT_RX_TIMEOUT_MS), 0);
    zassert_equal(frame_type, ZPT_RESP_TUNING_RESULT);
    zassert_equal(payload[1], ZPT_TUNING_STATUS_OK);

    /* Reset-all reports the first failing target (B fails with -EIO). */
    tx_frame(ZPT_REQ_TUNING_RESET, (const uint8_t[1]){0xff}, 1);
    zassert_equal(rx_frame(&frame_type, payload, &length, ZPT_RX_TIMEOUT_MS), 0);
    zassert_equal(payload[1], ZPT_TUNING_STATUS_INTERNAL_ERROR);

    /* Unknown target, bad length. */
    tx_frame(ZPT_REQ_TUNING_RESET, (const uint8_t[1]){99}, 1);
    zassert_equal(rx_frame(&frame_type, payload, &length, ZPT_RX_TIMEOUT_MS), 0);
    zassert_equal(payload[1], ZPT_TUNING_STATUS_UNKNOWN_TARGET);
    tx_frame(ZPT_REQ_TUNING_RESET, NULL, 0);
    zassert_equal(rx_frame(&frame_type, payload, &length, ZPT_RX_TIMEOUT_MS), 0);
    zassert_equal(payload[1], ZPT_TUNING_STATUS_INVALID_VALUE);
}

ZTEST(zpt_unit, tuning_help) {
    uint8_t frame_type;
    uint8_t payload[ZPT_MAX_RESPONSE_PAYLOAD];
    uint16_t length;
    const char *description = "Sensitivity gain.";

    tx_frame(ZPT_REQ_TUNING_HELP, (const uint8_t[2]){0, 0}, 2);
    zassert_equal(rx_frame(&frame_type, payload, &length, ZPT_RX_TIMEOUT_MS), 0);
    zassert_equal(frame_type, ZPT_RESP_TUNING_HELP);
    zassert_equal(payload[0], 0);
    zassert_equal(payload[1], 0);
    zassert_equal(sys_get_le16(&payload[2]), (uint16_t)strlen(description));
    zassert_mem_equal(&payload[4], description, strlen(description));
    zassert_equal(length, 4 + strlen(description));

    /* Unknown parameter and unknown target. */
    tx_frame(ZPT_REQ_TUNING_HELP, (const uint8_t[2]){0, 99}, 2);
    zassert_equal(rx_frame(&frame_type, payload, &length, ZPT_RX_TIMEOUT_MS), 0);
    zassert_equal(frame_type, ZPT_RESP_TUNING_RESULT);
    zassert_equal(payload[1], ZPT_TUNING_STATUS_UNKNOWN_PARAMETER);
    tx_frame(ZPT_REQ_TUNING_HELP, (const uint8_t[2]){99, 0}, 2);
    zassert_equal(rx_frame(&frame_type, payload, &length, ZPT_RX_TIMEOUT_MS), 0);
    zassert_equal(payload[1], ZPT_TUNING_STATUS_UNKNOWN_TARGET);
}

ZTEST(zpt_unit, tuning_metadata) {
    uint8_t frame_type;
    uint8_t payload[ZPT_MAX_RESPONSE_PAYLOAD];
    uint16_t length;

    tx_frame(ZPT_REQ_TUNING_TARGET_METADATA, (const uint8_t[1]){0}, 1);
    zassert_equal(rx_frame(&frame_type, payload, &length, ZPT_RX_TIMEOUT_MS), 0);
    zassert_equal(frame_type, ZPT_RESP_TUNING_TARGET_METADATA);
    zassert_equal(payload[0], 0);
    const char *stable_id = "zpt-test-target-a";
    const char *path = "/zpt/a";
    zassert_equal(payload[1], (uint8_t)strlen(stable_id));
    zassert_equal(sys_get_le16(&payload[2]), (uint16_t)strlen(path));
    zassert_mem_equal(&payload[4], stable_id, strlen(stable_id));
    zassert_mem_equal(&payload[4 + strlen(stable_id)], path, strlen(path));

    tx_frame(ZPT_REQ_TUNING_PARAMETER_METADATA, (const uint8_t[2]){0, 0}, 2);
    zassert_equal(rx_frame(&frame_type, payload, &length, ZPT_RX_TIMEOUT_MS), 0);
    zassert_equal(frame_type, ZPT_RESP_TUNING_PARAMETER_METADATA);
    zassert_equal(payload[0], 0);
    zassert_equal(payload[1], 0);
    const char *key = "gain";
    const char *property = "measlesbagel,gain";
    zassert_equal(payload[2], (uint8_t)strlen(key));
    zassert_equal(payload[3], (uint8_t)strlen(property));
    zassert_mem_equal(&payload[4], key, strlen(key));
    zassert_mem_equal(&payload[4 + strlen(key)], property, strlen(property));

    /* Unknown target and parameter. */
    tx_frame(ZPT_REQ_TUNING_TARGET_METADATA, (const uint8_t[1]){99}, 1);
    zassert_equal(rx_frame(&frame_type, payload, &length, ZPT_RX_TIMEOUT_MS), 0);
    zassert_equal(payload[1], ZPT_TUNING_STATUS_UNKNOWN_TARGET);
    tx_frame(ZPT_REQ_TUNING_PARAMETER_METADATA, (const uint8_t[2]){0, 99}, 2);
    zassert_equal(rx_frame(&frame_type, payload, &length, ZPT_RX_TIMEOUT_MS), 0);
    zassert_equal(payload[1], ZPT_TUNING_STATUS_UNKNOWN_PARAMETER);
}

ZTEST(zpt_unit, tuning_set_many) {
    uint8_t frame_type;
    uint8_t payload[ZPT_MAX_RESPONSE_PAYLOAD];
    uint16_t length;

    /* Two values, all valid: value field carries the applied count. */
    uint8_t batch[2 + 2 * 5] = {0, 2, 0, 10, 0, 0, 0, 1, 1, 0, 0, 0};
    tx_frame(ZPT_REQ_TUNING_SET_MANY, batch, sizeof(batch));
    zassert_equal(rx_frame(&frame_type, payload, &length, ZPT_RX_TIMEOUT_MS), 0);
    zassert_equal(frame_type, ZPT_RESP_TUNING_RESULT);
    zassert_equal(payload[0], ZPT_REQ_TUNING_SET_MANY);
    zassert_equal(payload[1], ZPT_TUNING_STATUS_OK);
    zassert_equal(payload[3], 0xff);
    zassert_equal(sys_get_le32(&payload[4]), 2);

    /* Duplicate parameter: rejected before applying anything. */
    uint8_t duplicate[2 + 2 * 5] = {0, 2, 0, 20, 0, 0, 0, 0, 30, 0, 0, 0};
    tx_frame(ZPT_REQ_TUNING_SET_MANY, duplicate, sizeof(duplicate));
    zassert_equal(rx_frame(&frame_type, payload, &length, ZPT_RX_TIMEOUT_MS), 0);
    zassert_equal(payload[1], ZPT_TUNING_STATUS_INVALID_VALUE);
    zassert_equal(payload[3], 0);

    /* Out of range: rejected before applying. */
    uint8_t out_of_range[2 + 2 * 5] = {0, 2, 0, 10, 0, 0, 0, 1, 2, 0, 0, 0};
    tx_frame(ZPT_REQ_TUNING_SET_MANY, out_of_range, sizeof(out_of_range));
    zassert_equal(rx_frame(&frame_type, payload, &length, ZPT_RX_TIMEOUT_MS), 0);
    zassert_equal(payload[1], ZPT_TUNING_STATUS_INVALID_VALUE);
    zassert_equal(payload[3], 1);

    /* Shape errors: empty, count zero, length mismatch. */
    tx_frame(ZPT_REQ_TUNING_SET_MANY, (const uint8_t[1]){0}, 1);
    zassert_equal(rx_frame(&frame_type, payload, &length, ZPT_RX_TIMEOUT_MS), 0);
    zassert_equal(payload[1], ZPT_TUNING_STATUS_INVALID_VALUE);
    tx_frame(ZPT_REQ_TUNING_SET_MANY, (const uint8_t[2]){0, 0}, 2);
    zassert_equal(rx_frame(&frame_type, payload, &length, ZPT_RX_TIMEOUT_MS), 0);
    zassert_equal(payload[1], ZPT_TUNING_STATUS_INVALID_VALUE);
    tx_frame(ZPT_REQ_TUNING_SET_MANY, (const uint8_t[3]){0, 1, 0}, 3);
    zassert_equal(rx_frame(&frame_type, payload, &length, ZPT_RX_TIMEOUT_MS), 0);
    zassert_equal(payload[1], ZPT_TUNING_STATUS_INVALID_VALUE);
}

/* --- State telemetry ----------------------------------------------------- */

static uint32_t last_observed_sequence;

static void send_state_control(uint8_t target_id, enum zpt_state_level level) {
    const uint8_t control[2] = {target_id, (uint8_t)level};

    tx_frame(ZPT_REQ_STATE_CONTROL, control, 2);
}

ZTEST(zpt_unit, state_register_submit) {
    uint8_t frame_type;
    uint8_t payload[ZPT_MAX_RESPONSE_PAYLOAD];
    uint16_t length;
    uint8_t state_id;

    zassert_equal(zpt_state_telemetry_register_target(&state_id, "zpt-test-state"), 0);
    /* State ids are allocated above the tuning targets (earlier cases may
     * already have taken lower state ids — there is no unregister API). */
    zassert_true(state_id >= (uint8_t)zpt_tuning_target_count());
    zassert_equal(zpt_state_telemetry_level(state_id), ZPT_STATE_LEVEL_OFF);

    send_state_control(state_id, ZPT_STATE_LEVEL_DECISIONS);
    zassert_equal(rx_frame(&frame_type, payload, &length, ZPT_RX_TIMEOUT_MS), 0);
    zassert_equal(frame_type, ZPT_RESP_STATE_STATUS);
    zassert_equal(payload[0], ZPT_STATE_SCHEMA_VERSION);
    /* The entry count also covers earlier state registrations; this case's
     * id is not necessarily the highest one (there is no unregister API and
     * earlier cases may leave their slots at the default OFF level, which
     * the next registration reuses). */
    zassert_true(payload[7] >= state_id + 1);
    const uint8_t *entry = state_status_entry(payload, state_id);
    zassert_not_null(entry);
    zassert_equal(entry[1], ZPT_STATE_LEVEL_DECISIONS);
    const char *state_label = "zpt-test-state";
    zassert_equal(entry[2], (uint8_t)strlen(state_label));
    zassert_mem_equal(&entry[3], state_label, strlen(state_label));

    struct zpt_state_sample sample = {
        .timestamp_ms = 1234,
        .target_id = state_id,
        .target_kind = 4,
        .event = ZPT_STATE_EVENT_FRAME,
        .intent = 2,
        .flags = ZPT_STATE_FLAG_INTENT_CHANGED | ZPT_STATE_FLAG_OUTPUT,
        .values = {1, -2, 3, -4, 5, 6, 7, 8, 9, 10},
    };
    zpt_state_telemetry_submit(&sample);

    /* The service thread only flushes queued state events when it wakes,
     * i.e. on the next host frame: ping to trigger the flush. */
    tx_frame(ZPT_REQ_PING, NULL, 0);
    zassert_equal(rx_frame(&frame_type, payload, &length, ZPT_RX_TIMEOUT_MS), 0);
    zassert_equal(frame_type, ZPT_RESP_ACK);

    zassert_equal(rx_frame(&frame_type, payload, &length, ZPT_RX_TIMEOUT_MS), 0);
    zassert_equal(frame_type, ZPT_EVENT_STATE);
    zassert_equal(length, 14 + ZPT_STATE_VALUE_COUNT * 4);
    zassert_equal(payload[0], state_id);
    zassert_equal(payload[1], 4);
    zassert_equal(payload[2], ZPT_STATE_EVENT_FRAME);
    zassert_equal(payload[3], 2);
    zassert_equal(sys_get_le16(&payload[4]), sample.flags);
    zassert_equal(sys_get_le32(&payload[6]), 1234u);
    last_observed_sequence = sys_get_le32(&payload[10]);
    zassert_true(last_observed_sequence >= 1);
    for (int i = 0; i < ZPT_STATE_VALUE_COUNT; i++) {
        zassert_equal((int32_t)sys_get_le32(&payload[14 + i * 4]), sample.values[i]);
    }
}

ZTEST(zpt_unit, state_level_off) {
    uint8_t state_id;

    zassert_equal(zpt_state_telemetry_register_target(&state_id, "zpt-test-state2"), 0);
    /* Level defaults to off: submits are dropped before the queue. */
    struct zpt_state_sample sample = {
        .target_id = state_id,
        .target_kind = 4,
        .event = ZPT_STATE_EVENT_FLUSH,
        .values = {42},
    };
    zpt_state_telemetry_submit(&sample);
    expect_no_frame(300);
}

ZTEST(zpt_unit, state_all_targets) {
    uint8_t frame_type;
    uint8_t payload[ZPT_MAX_RESPONSE_PAYLOAD];
    uint16_t length;

    send_state_control(ZPT_STATE_ALL_TARGETS, ZPT_STATE_LEVEL_VERBOSE);
    zassert_equal(rx_frame(&frame_type, payload, &length, ZPT_RX_TIMEOUT_MS), 0);
    zassert_equal(frame_type, ZPT_RESP_STATE_STATUS);
    for (size_t offset = 8; offset < length; offset += 3 + payload[offset + 2]) {
        zassert_equal(payload[offset + 1], ZPT_STATE_LEVEL_VERBOSE);
    }

    send_state_control(ZPT_STATE_ALL_TARGETS, ZPT_STATE_LEVEL_OFF);
    zassert_equal(rx_frame(&frame_type, payload, &length, ZPT_RX_TIMEOUT_MS), 0);
    zassert_equal(frame_type, ZPT_RESP_STATE_STATUS);
    for (size_t offset = 8; offset < length; offset += 3 + payload[offset + 2]) {
        zassert_equal(payload[offset + 1], ZPT_STATE_LEVEL_OFF);
    }

    /* A level above VERBOSE is ignored. */
    send_state_control(ZPT_STATE_ALL_TARGETS, 3);
    zassert_equal(rx_frame(&frame_type, payload, &length, ZPT_RX_TIMEOUT_MS), 0);
    for (size_t offset = 8; offset < length; offset += 3 + payload[offset + 2]) {
        zassert_equal(payload[offset + 1], ZPT_STATE_LEVEL_OFF);
    }
}

ZTEST(zpt_unit, state_dropped_counter) {
    uint8_t frame_type;
    uint8_t payload[ZPT_MAX_RESPONSE_PAYLOAD];
    uint16_t length;
    uint8_t state_id;

    zassert_equal(zpt_state_telemetry_register_target(&state_id, "zpt-test-drop"), 0);
    send_state_control(state_id, ZPT_STATE_LEVEL_DECISIONS);
    zassert_equal(rx_frame(&frame_type, payload, &length, ZPT_RX_TIMEOUT_MS), 0);

    tx_frame(ZPT_REQ_PING, NULL, 0);
    zassert_equal(rx_frame(&frame_type, payload, &length, ZPT_RX_TIMEOUT_MS), 0);
    zassert_equal(frame_type, ZPT_RESP_ACK);
    const uint32_t dropped_before = sys_get_le32(&payload[0]);

    /* Fill the record queue directly; the next accepted submit cannot be
     * queued and must bump the dropped counter. */
    struct zpt_test_record dummy = {.kind = 0xff};
    for (int i = 0; i < CONFIG_ZMK_POINTING_TOOLS_TELEMETRY_QUEUE_SIZE; i++) {
        zassert_equal(k_msgq_put(&zpt_record_queue, &dummy, K_NO_WAIT), 0);
    }
    struct zpt_state_sample sample = {.target_id = state_id, .values = {1}};
    zpt_state_telemetry_submit(&sample);

    tx_frame(ZPT_REQ_PING, NULL, 0);
    zassert_equal(rx_frame(&frame_type, payload, &length, ZPT_RX_TIMEOUT_MS), 0);
    zassert_equal(frame_type, ZPT_RESP_ACK);
    zassert_equal(sys_get_le32(&payload[0]), dropped_before + 1);

    /* Drain the dummy records so the next case starts with an empty queue. */
    struct zpt_test_record drained;
    while (k_msgq_get(&zpt_record_queue, &drained, K_NO_WAIT) == 0) {
    }
    expect_no_frame(300);
}

ZTEST(zpt_unit, state_timeout_purge) {
    uint8_t frame_type;
    uint8_t payload[ZPT_MAX_RESPONSE_PAYLOAD];
    uint16_t length;
    uint8_t state_id;

    zassert_equal(zpt_state_telemetry_register_target(&state_id, "zpt-test-purge"), 0);

    /* Contact now (T0), then enable the target. */
    tx_frame(ZPT_REQ_PING, NULL, 0);
    zassert_equal(rx_frame(&frame_type, payload, &length, ZPT_RX_TIMEOUT_MS), 0);
    send_state_control(state_id, ZPT_STATE_LEVEL_DECISIONS);
    zassert_equal(rx_frame(&frame_type, payload, &length, ZPT_RX_TIMEOUT_MS), 0);

    struct zpt_state_sample sample = {.target_id = state_id, .values = {77}};
    zpt_state_telemetry_submit(&sample);

    /* Ping wakes the service thread, which flushes the queued event. This
     * also refreshes the host contact time (T0 for the purge below). */
    tx_frame(ZPT_REQ_PING, NULL, 0);
    zassert_equal(rx_frame(&frame_type, payload, &length, ZPT_RX_TIMEOUT_MS), 0);
    zassert_equal(frame_type, ZPT_RESP_ACK);
    zassert_equal(rx_frame(&frame_type, payload, &length, ZPT_RX_TIMEOUT_MS), 0);
    zassert_equal(frame_type, ZPT_EVENT_STATE);
    zassert_true(sys_get_le32(&payload[10]) > last_observed_sequence);

    /* The host disappears longer than the configured timeout: the next
     * record is purged and all levels are disabled. */
    k_sleep(K_MSEC(CONFIG_ZMK_POINTING_TOOLS_TELEMETRY_HOST_TIMEOUT_MS + 200));
    zpt_state_telemetry_submit(&sample);
    expect_no_frame(300);

    send_state_control(state_id, ZPT_STATE_LEVEL_OFF);
    zassert_equal(rx_frame(&frame_type, payload, &length, ZPT_RX_TIMEOUT_MS), 0);
    const uint8_t *entry = state_status_entry(payload, state_id);
    zassert_not_null(entry);
    zassert_equal(entry[1], ZPT_STATE_LEVEL_OFF);
}

/* --- Capacity fill (must run last) -------------------------------------- */

/* The tuning registry is a process-wide singleton with no unregister API.
 * Filling it must happen after every test that registers tuning targets or
 * allocates state ids relative to the tuning count, so it is the last case
 * of the last suite. */

#define ZPT_FILL_TARGETS CONFIG_ZMK_POINTING_TOOLS_TUNING_MAX_TARGETS

static const char *const fill_stable_ids[ZPT_FILL_TARGETS] = {
    "zpt-fill-00", "zpt-fill-01", "zpt-fill-02", "zpt-fill-03", "zpt-fill-04", "zpt-fill-05",
    "zpt-fill-06", "zpt-fill-07", "zpt-fill-08", "zpt-fill-09", "zpt-fill-10", "zpt-fill-11",
    "zpt-fill-12", "zpt-fill-13", "zpt-fill-14", "zpt-fill-15",
};

static struct zpt_tuning_parameter fill_parameters[ZPT_FILL_TARGETS];
static struct zpt_tuning_target fill_targets[ZPT_FILL_TARGETS];

static int fill_get(void *context, uint8_t parameter_id, bool compiled, int32_t *value) {
    ARG_UNUSED(context);
    ARG_UNUSED(compiled);
    if (parameter_id != 0) {
        return -ENOENT;
    }
    *value = 0;
    return 0;
}

static int fill_set_many(void *context, const struct zpt_tuning_value *values, size_t value_count,
                         uint8_t *failed_parameter_id) {
    ARG_UNUSED(context);
    ARG_UNUSED(values);
    ARG_UNUSED(value_count);
    ARG_UNUSED(failed_parameter_id);
    return 0;
}

static int fill_reset(void *context) {
    ARG_UNUSED(context);
    return 0;
}

/* Never registered by the fill loop: exercises the full-registry path
 * (re-registering an already-registered pointer would return its existing
 * index instead of -ENOMEM). */
static struct zpt_tuning_parameter fill_overflow_parameter;
static struct zpt_tuning_target fill_overflow_target;

static void fill_targets_init(void) {
    for (size_t i = 0; i < ZPT_FILL_TARGETS; i++) {
        fill_parameters[i] = (struct zpt_tuning_parameter){
            .id = 0,
            .type = ZPT_TUNING_VALUE_INTEGER,
            .minimum = 0,
            .maximum = 100,
            .step = 1,
            .key = "fill",
            .devicetree_property = "measlesbagel,fill",
            .label = "Fill",
            .unit = "",
            .description = "Capacity fill target.",
        };
        fill_targets[i] = (struct zpt_tuning_target){
            .kind = ZPT_TUNING_TARGET_PIPELINE_STAGE,
            .stable_id = fill_stable_ids[i],
            .label = "Fill",
            .devicetree_path = "/zpt/fill",
            .parameter_count = 1,
            .parameters = &fill_parameters[i],
            .context = (void *)i,
            .get = fill_get,
            .set_many = fill_set_many,
            .reset = fill_reset,
        };
    }

    fill_overflow_parameter = (struct zpt_tuning_parameter){
        .id = 0,
        .type = ZPT_TUNING_VALUE_INTEGER,
        .minimum = 0,
        .maximum = 100,
        .step = 1,
        .key = "fill",
        .devicetree_property = "measlesbagel,fill",
        .label = "Fill",
        .unit = "",
        .description = "Capacity overflow target.",
    };
    fill_overflow_target = (struct zpt_tuning_target){
        .kind = ZPT_TUNING_TARGET_PIPELINE_STAGE,
        .stable_id = "zpt-fill-overflow",
        .label = "Fill",
        .devicetree_path = "/zpt/fill",
        .parameter_count = 1,
        .parameters = &fill_overflow_parameter,
        .context = NULL,
        .get = fill_get,
        .set_many = fill_set_many,
        .reset = fill_reset,
    };
}

ZTEST(zpt_unit, z_capacity_fill) {
    /* Only call init here (not in suite setup): the throwaway targets must
     * not exist while other cases observe the registry. */
    fill_targets_init();

    /* A and B were registered by the suite setup. register() returns the
     * index assigned to the target, i.e. the count before the insert. */
    size_t index = (size_t)zpt_tuning_target_count();

    zassert_equal(index, 2);
    while ((size_t)zpt_tuning_target_count() < ZPT_FILL_TARGETS) {
        zassert_equal(zpt_tuning_register(&fill_targets[index]), (int)index);
        index++;
    }
    zassert_equal(index, (size_t)ZPT_FILL_TARGETS);
    zassert_equal(zpt_tuning_register(&fill_overflow_target), -ENOMEM);
}
