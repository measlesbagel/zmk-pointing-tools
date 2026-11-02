/* SPDX-License-Identifier: MIT */

/* Unit tests for the USB-serial telemetry service (src/service/telemetry.c)
 * over the virtual UART driver (vnd,serial,
 * zephyr/drivers/serial/serial_test.c): host requests are injected into
 * the RX queue, service responses are captured from the TX buffer.
 *
 * The wire protocol constants below are the public protocol (v8); they
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

/* --- Wire protocol (v8) ---------------------------------------------- */

#define ZPT_PROTOCOL_VERSION 8
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
 * src/service/telemetry.c (a kind byte, then a union whose largest member
 * is the state sample). */
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
     * range, and a read-only pad. Flags: bit0 reachable (local, or the
     * owning half answered; this build has no tunnel), bit1 has settable
     * capabilities. */
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

ZTEST(zpt_unit, zz_device_describe_reports_range_capabilities) {
    uint8_t request[1] = {1};
    static const char stable_id[] = "test-remote-trackball";
    static const char dt_path[] = "/trackball-remote";
    uint8_t expected[64];
    size_t offset = 0;

    expected[offset++] = (uint8_t)(sizeof(stable_id) - 1);
    memcpy(&expected[offset], stable_id, sizeof(stable_id) - 1);
    offset += sizeof(stable_id) - 1;
    expected[offset++] = (uint8_t)(sizeof(dt_path) - 1);
    expected[offset++] = 0;
    memcpy(&expected[offset], dt_path, sizeof(dt_path) - 1);
    offset += sizeof(dt_path) - 1;
    for (int i = 0; i < 2; i++) { /* current, then compiled default: 400 */
        expected[offset++] = 0x90;
        expected[offset++] = 0x01;
    }
    expected[offset++] = 0x01; /* settable, value is current */
    expected[offset++] = 0;    /* range form */
    memcpy(&expected[offset], (uint8_t[]){100, 0, 0xb0, 0x04, 100, 0}, 6);
    offset += 6;

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
    zassert_equal(length, 8);
    zassert_equal(sys_get_le32(&payload[0]), 0u);
    /* payload[4..7] counts capture drops; capture_records_pipeline_input,
     * which runs earlier, fills the capture buffer on purpose. */
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
    /* The entry count also covers earlier state registrations (there is no
     * unregister API, so every earlier case keeps its slot). */
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

/* Regression: allocation used to treat any slot at the default OFF level as
 * free, so every stage registered during init shared one id and the status
 * response listed only the last label. */
ZTEST(zpt_unit, state_ids_distinct) {
    uint8_t frame_type;
    uint8_t payload[ZPT_MAX_RESPONSE_PAYLOAD];
    uint16_t length;
    uint8_t first;
    uint8_t second;

    zassert_equal(zpt_state_telemetry_register_target(&first, "zpt-test-first"), 0);
    zassert_equal(zpt_state_telemetry_register_target(&second, "zpt-test-second"), 0);
    zassert_not_equal(first, second);
    zassert_equal(zpt_state_telemetry_level(first), ZPT_STATE_LEVEL_OFF);
    zassert_equal(zpt_state_telemetry_level(second), ZPT_STATE_LEVEL_OFF);

    tx_frame(ZPT_REQ_STATE_CONTROL, NULL, 0);
    zassert_equal(rx_frame(&frame_type, payload, &length, ZPT_RX_TIMEOUT_MS), 0);
    zassert_equal(frame_type, ZPT_RESP_STATE_STATUS);
    const uint8_t *first_entry = state_status_entry(payload, first);
    const uint8_t *second_entry = state_status_entry(payload, second);
    zassert_not_null(first_entry);
    zassert_not_null(second_entry);
    zassert_equal(first_entry[2], (uint8_t)strlen("zpt-test-first"));
    zassert_mem_equal(&first_entry[3], "zpt-test-first", strlen("zpt-test-first"));
    zassert_equal(second_entry[2], (uint8_t)strlen("zpt-test-second"));
    zassert_mem_equal(&second_entry[3], "zpt-test-second", strlen("zpt-test-second"));
}

/* A tunable stage reports state under its tuning id, and a stage observed in
 * several pipelines registers once per pipeline but stays one target. */
ZTEST(zpt_unit, state_ids_follow_tuning_and_repeats) {
    uint8_t tuned;
    zassert_equal(zpt_state_telemetry_register_target(&tuned, "zpt-test-target-a"), 0);
    const struct zpt_tuning_target *target = zpt_tuning_target_get(tuned);
    zassert_not_null(target);
    zassert_str_equal(target->stable_id, "zpt-test-target-a");

    uint8_t first;
    uint8_t again;
    zassert_equal(zpt_state_telemetry_register_target(&first, "zpt-test-shared"), 0);
    zassert_equal(zpt_state_telemetry_register_target(&again, "zpt-test-shared"), 0);
    zassert_equal(first, again);
    zassert_true(first >= (uint8_t)zpt_tuning_target_count());
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

/* --- Input capture (protocol v8) ------------------------------------- */

#include <zmk/pointing_tools/core/fixed.h>
#include <zmk/pointing_tools/observer/capture.h>
#include <zmk/pointing_tools/service/capture.h>
#include <zmk/pointing_tools/stage/motion_gate/coherent_displacement.h>
#include <zmk/pointing_tools/stage/orientation.h>
#include <zmk/pointing_tools/stage/resolution_normalize.h>

#define ZPT_TEST_REQ_CAPTURE_CONTROL 0x10
#define ZPT_TEST_REQ_CAPTURE_DESCRIBE 0x11
#define ZPT_TEST_RESP_CAPTURE_STATUS 0x8c
#define ZPT_TEST_RESP_CAPTURE_DESCRIPTION 0x8d
#define ZPT_TEST_EVENT_CAPTURE 0x92

/* The test app has no ZMK routers to hold, so no frame can be running. */
int zpt_capture_between_frames(void (*fn)(void *arg), void *arg) {
    fn(arg);
    return 0;
}

/* A cursor pipeline front, so a recording resets it and enters it as the
 * session in host/tests/capture_session.c does, and a keypress guard that
 * input capture can freeze. The gate stage borrows tuning target A's
 * stable id, so the settings hash covers A's parameters. */
static bool capture_guard_have;
static uint32_t capture_guard_ms;

static bool capture_guard_suppressed(void *context, const struct zpt_signal *signal,
                                     uint32_t now_ms) {
    ARG_UNUSED(context);
    ARG_UNUSED(signal);
    ARG_UNUSED(now_ms);
    return false;
}

/* Freezes not yet thawed: zero after every push. */
static int capture_guard_frozen;

static bool capture_guard_freeze(void *context, uint32_t *time_ms) {
    ARG_UNUSED(context);
    capture_guard_frozen++;
    *time_ms = capture_guard_ms;
    return capture_guard_have;
}

static void capture_guard_thaw(void *context) {
    ARG_UNUSED(context);
    capture_guard_frozen--;
}

/* The link refuses this many droppable frames (src/service/telemetry.c). */
extern atomic_t zpt_test_refuse_droppable;

static const struct zpt_suppression_policy capture_guard = {
    .is_suppressed = capture_guard_suppressed,
    .keypress_window_ms = 40,
    .freeze_keypress = capture_guard_freeze,
    .thaw_keypress = capture_guard_thaw,
};

static int capture_sink_emit(struct zpt_sink *sink, const struct zpt_signal *signal) {
    ARG_UNUSED(sink);
    ARG_UNUSED(signal);
    return 0;
}

static const struct zpt_sink_api capture_sink_api = {
    .type_id = "test-sink",
    .accepted_kinds = ZPT_SIGNAL_KIND_MASK(ZPT_SIGNAL_NORMALIZED_MOTION),
    .emit = capture_sink_emit,
};

static const struct zpt_orientation_config capture_orientation = {.invert_y = true};
static const struct zpt_coherent_displacement_stage_config capture_gate = {
    .settings = {.enabled = true,
                 .activation_distance = ZPT_FIXED_ONE,
                 .coherence_percent = 50,
                 .qualification_timeout_ms = 100,
                 .idle_timeout_ms = 100},
    .suppression = &capture_guard,
};
static struct zpt_coherent_displacement_stage_state capture_gate_state;
static struct zpt_stage capture_stage_list[] = {
    {.stable_id = "o",
     .api = &zpt_orthogonal_orientation_stage_api,
     .config = &capture_orientation},
    {.stable_id = "n", .api = &zpt_resolution_normalize_stage_api},
    {.stable_id = "zpt-test-target-a",
     .api = &zpt_coherent_displacement_stage_api,
     .config = &capture_gate,
     .state = &capture_gate_state},
};
static struct zpt_stage *capture_stages[] = {&capture_stage_list[0], &capture_stage_list[1],
                                             &capture_stage_list[2]};
static struct zpt_sink capture_sink = {.stable_id = "test-sink", .api = &capture_sink_api};
static struct zpt_pipeline capture_pipeline = {
    .stable_id = "test-capture",
    .input_kind = ZPT_SIGNAL_RAW_MOTION,
    .stages = capture_stages,
    .stage_count = ARRAY_SIZE(capture_stages),
    .sink = &capture_sink,
    .dispatch_budget = 4,
};

static void capture_push(int32_t x, int32_t y) {
    const struct zpt_signal signal = {
        .kind = ZPT_SIGNAL_RAW_MOTION,
        .metadata = {.observed_at_ms = k_uptime_get_32(),
                     .sample_span_us = 1500,
                     .flags = ZPT_SIGNAL_FLAG_SAMPLE_SPAN_CLIPPED,
                     .source_id = 9,
                     .resolution_cpi = 800},
        .data.raw_motion = {.x_counts = x, .y_counts = y},
    };
    struct zpt_pipeline_result result;
    zassert_ok(zpt_pipeline_push(&capture_pipeline, &signal, &result));
    zassert_equal(capture_guard_frozen, 0, "the guard is thawed after every frame");
}

static uint32_t capture_dropped(void) {
    uint8_t frame_type;
    uint8_t payload[ZPT_MAX_RESPONSE_PAYLOAD];
    uint16_t length;
    tx_frame(ZPT_REQ_PING, NULL, 0);
    zassert_ok(rx_frame(&frame_type, payload, &length, ZPT_RX_TIMEOUT_MS));
    zassert_equal(frame_type, ZPT_RESP_ACK);
    return sys_get_le32(&payload[4]);
}

/* Sends a capture control request and checks the status; returns the
 * settings hash it reports. */
static uint32_t capture_control(const uint8_t *payload, uint16_t length, uint8_t recording) {
    uint8_t frame_type;
    uint8_t response[ZPT_MAX_RESPONSE_PAYLOAD];
    uint16_t response_length;

    tx_frame(ZPT_TEST_REQ_CAPTURE_CONTROL, payload, length);
    zassert_ok(rx_frame(&frame_type, response, &response_length, ZPT_RX_TIMEOUT_MS));
    zassert_equal(frame_type, ZPT_TEST_RESP_CAPTURE_STATUS);
    zassert_equal(response[0], 3, "capture schema");
    zassert_equal(response[1], recording);
    zassert_true(sys_get_le32(&response[6]) <= CONFIG_ZMK_POINTING_TOOLS_INPUT_CAPTURE_BUFFER_SIZE,
                 "buffer fill");
    zassert_equal(sys_get_le32(&response[10]), CONFIG_ZMK_POINTING_TOOLS_INPUT_CAPTURE_BUFFER_SIZE);
    /* The uptime the status was sent at, the clock of record times. */
    const uint32_t uptime_ms = sys_get_le32(&response[14]);
    zassert_true(uptime_ms <= k_uptime_get_32() && k_uptime_get_32() - uptime_ms < 1000U,
                 "device uptime");
    zassert_equal(response[18], 1, "one recordable pipeline");
    /* id, active, three stages, replayable; then the hash and the label. */
    static const uint8_t entry[] = {0, 1, 3, 1};
    static const uint8_t label[] = {12, 't', 'e', 's', 't', '-', 'c', 'a', 'p', 't', 'u', 'r', 'e'};
    zassert_equal(response_length, 19 + sizeof(entry) + 4 + sizeof(label));
    zassert_mem_equal(&response[19], entry, sizeof(entry));
    zassert_mem_equal(&response[27], label, sizeof(label));
    return sys_get_le32(&response[23]);
}

/* Collects capture batches until count records arrived; returns the
 * concatenated record bytes and checks the sequence continues. */
static size_t capture_records(uint8_t *records, size_t count, uint32_t *sequence) {
    size_t offset = 0;
    size_t received = 0;
    while (received < count) {
        uint8_t frame_type;
        uint8_t payload[ZPT_MAX_RESPONSE_PAYLOAD];
        uint16_t length;
        zassert_ok(rx_frame(&frame_type, payload, &length, ZPT_RX_TIMEOUT_MS));
        zassert_equal(frame_type, ZPT_TEST_EVENT_CAPTURE);
        zassert_equal(sys_get_le32(&payload[0]), *sequence);
        *sequence += payload[4];
        received += payload[4];
        memcpy(&records[offset], &payload[5], length - 5U);
        offset += length - 5U;
    }
    zassert_equal(received, count);
    return offset;
}

/* Lets the gate's idle deadline run and the pipeline rest, then pushes a
 * frame: a pending resync resets the pipeline and enters it again. */
/* A delta frame: kind, then three varints (time, x, y), as this test's
 * frames send them: same span and flags as the frame before. */
static size_t delta_frame_size(const uint8_t *record) {
    zassert_equal(record[0], 0x80, "delta frame with the previous span and flags");
    size_t size = 1;
    for (int field = 0; field < 3; field++) {
        while ((record[size] & 0x80U) != 0U) {
            size++;
        }
        size++;
    }
    return size;
}

static void capture_resync(uint32_t settings_hash, uint32_t *sequence) {
    uint8_t records[64];
    struct zpt_pipeline_result result;
    k_sleep(K_MSEC(150));
    zassert_ok(zpt_pipeline_flush(&capture_pipeline, k_uptime_get_32(), &result));
    zassert_equal(capture_records(records, 1, sequence), 5);
    zassert_equal(records[0], 7, "flush");
    k_sleep(K_MSEC(ZPT_CAPTURE_RESYNC_REST_MS + 20));
    capture_push(1, 0);
    const size_t bytes = capture_records(records, 4, sequence);
    zassert_equal(records[0], 4, "entered");
    zassert_equal(sys_get_le32(&records[1]), settings_hash);
    zassert_equal(records[5], 2, "source");
    zassert_equal(records[11], 3, "the guard's key press, again");
    /* Every trace starts with a whole frame, whether a drop or a settings
     * change ended the one before. */
    zassert_equal(bytes, 5 + 6 + 5 + 20);
    zassert_equal(records[16], 1);
}

ZTEST(zpt_unit, capture_records_pipeline_input) {
    uint8_t frame_type;
    uint8_t payload[ZPT_MAX_RESPONSE_PAYLOAD];
    uint16_t length;
    uint8_t records[CONFIG_ZMK_POINTING_TOOLS_INPUT_CAPTURE_BUFFER_SIZE];
    uint32_t sequence = 0;

    zassert_ok(zpt_pipeline_validate(&capture_pipeline));
    zassert_ok(zpt_pipeline_activate(&capture_pipeline, ZPT_RESET_PIPELINE_ENTERED));
    zassert_ok(zpt_capture_register_pipeline(&capture_pipeline));

    /* Off by default: frames before a recording send nothing. */
    const uint32_t hash = capture_control(NULL, 0, 0xff);
    zassert_not_equal(hash, 0, "target A's settings are hashed");
    capture_guard_have = true;
    capture_guard_ms = 1234;
    capture_push(1, 1);
    expect_no_frame(200);

    /* Starting runs zpt_capture_tap_start, as the host session does: the
     * replayable pipeline resets and enters, and the first frame then
     * describes its source and the key press the guard holds. */
    const uint8_t start = 0;
    zassert_equal(capture_control(&start, 1, 0), hash);
    size_t bytes = capture_records(records, 1, &sequence);
    zassert_equal(bytes, 5);
    zassert_equal(records[0], 4, "entered");
    zassert_equal(sys_get_le32(&records[1]), hash);

    const uint32_t before_ms = k_uptime_get_32();
    capture_push(-3, 70000);
    bytes = capture_records(records, 3, &sequence);
    zassert_equal(bytes, 6 + 5 + 20);
    static const uint8_t opening[] = {2, 0xff, 9, 0, 0x20, 0x03, 3, 0xd2, 0x04, 0, 0};
    zassert_mem_equal(records, opening, sizeof(opening));
    const uint8_t *frame = &records[sizeof(opening)];
    zassert_equal(frame[0], 1);
    zassert_equal(frame[1], (uint8_t)ZPT_SIGNAL_FLAG_SAMPLE_SPAN_CLIPPED);
    zassert_true(sys_get_le32(&frame[2]) - before_ms <= 1);
    zassert_true(sys_get_le16(&frame[6]) < 1000);
    zassert_equal((int32_t)sys_get_le32(&frame[8]), -3);
    zassert_equal((int32_t)sys_get_le32(&frame[12]), 70000);
    zassert_equal(sys_get_le32(&frame[16]), 1500);

    /* A newer key press is recorded before the frame it decides; the same
     * one isn't recorded twice. */
    capture_guard_ms = before_ms + 1;
    capture_push(1, 0);
    capture_push(1, 0);
    /* Frames after the first are deltas from the frame before. */
    bytes = capture_records(records, 3, &sequence);
    zassert_equal(records[0], 3);
    zassert_equal(sys_get_le32(&records[1]), before_ms + 1);
    const size_t first_delta = delta_frame_size(&records[5]);
    zassert_equal(bytes, 5 + first_delta + delta_frame_size(&records[5 + first_delta]));

    /* A link without room keeps the records: after two refused sends, the
     * three frames go out in order in one batch. */
    atomic_set(&zpt_test_refuse_droppable, 2);
    for (int i = 0; i < 3; i++) {
        capture_push(1, 0);
        if (i < 2) {
            expect_no_frame(50);
        }
    }
    zassert_ok(rx_frame(&frame_type, payload, &length, ZPT_RX_TIMEOUT_MS));
    zassert_equal(frame_type, ZPT_TEST_EVENT_CAPTURE);
    zassert_equal(sys_get_le32(&payload[0]), sequence);
    zassert_equal(payload[4], 3);
    size_t offset = 5;
    for (int i = 0; i < 3; i++) {
        offset += delta_frame_size(&payload[offset]);
    }
    zassert_equal(offset, length);
    sequence += 3;
    zassert_equal(atomic_get(&zpt_test_refuse_droppable), 0);

    /* While the link takes nothing, records wait in the buffer until it is
     * full; only then are records dropped and counted. Once the link has
     * room, every stored record arrives in order, then the gap. */
    const uint32_t dropped_at_start = capture_dropped();
    atomic_set(&zpt_test_refuse_droppable, 100000);
    const uint32_t pushed = CONFIG_ZMK_POINTING_TOOLS_INPUT_CAPTURE_BUFFER_SIZE / 3U;
    for (uint32_t i = 0; i < pushed; i++) {
        capture_push(1, 0);
        /* Let the worker try the link; sleeping here would outlast the
         * host timeout, which stops the recording. */
        k_yield();
    }
    const uint32_t dropped = capture_dropped() - dropped_at_start;
    zassert_true(dropped > 0U && dropped < pushed, "the buffer filled, then dropped (%u of %u)",
                 dropped, pushed);
    /* The link has room again; a request wakes the worker, as a returned
     * Bluetooth credit does. */
    atomic_set(&zpt_test_refuse_droppable, 0);
    zassert_equal(capture_dropped() - dropped_at_start, dropped);
    bytes = capture_records(records, pushed - dropped, &sequence);
    zassert_true(bytes > CONFIG_ZMK_POINTING_TOOLS_INPUT_CAPTURE_BUFFER_SIZE - 20U, "it was full");
    for (offset = 0; offset < bytes;) {
        offset += delta_frame_size(&records[offset]);
    }
    zassert_equal(offset, bytes);
    sequence += dropped;
    /* The pipeline kept running; at the next rest the tap resyncs, and the
     * first frame after the drop goes whole. */
    capture_resync(hash, &sequence);

    /* A tuning change while recording: the next entered record, at the
     * next rest, carries the new settings' hash. */
    int32_t gain;
    zassert_ok(zpt_tuning_get(0, 0, false, &gain));
    zassert_ok(zpt_tuning_set(0, 0, gain + 5));
    zpt_capture_settings_changed();
    zassert_equal(capture_records(records, 1, &sequence), 1);
    zassert_equal(records[0], 6, "settings changed");
    capture_resync(zpt_capture_settings_hash_add(
                       hash - zpt_capture_settings_hash_add(0, "zpt-test-target-a", "gain", gain),
                       "zpt-test-target-a", "gain", gain + 5),
                   &sequence);
    zassert_ok(zpt_tuning_set(0, 0, gain));

    /* Stopping sends what is still stored before the status, whatever the
     * link's room, and ends the stream. */
    atomic_set(&zpt_test_refuse_droppable, 100000);
    capture_push(1, 0);
    expect_no_frame(50);
    const uint8_t stop = 0xff;
    tx_frame(ZPT_TEST_REQ_CAPTURE_CONTROL, &stop, 1);
    zassert_equal(capture_records(records, 1, &sequence), delta_frame_size(records));
    atomic_set(&zpt_test_refuse_droppable, 0);
    zassert_ok(rx_frame(&frame_type, payload, &length, ZPT_RX_TIMEOUT_MS));
    zassert_equal(frame_type, ZPT_TEST_RESP_CAPTURE_STATUS);
    zassert_equal(payload[1], 0xff);
    zassert_equal(sys_get_le32(&payload[6]), 0, "the buffer is empty");
    capture_push(1, 0);
    expect_no_frame(200);

    /* The description names the pipeline, its settings hash, its stages,
     * and the settings they take that tuning doesn't publish. */
    const uint8_t describe = 0;
    tx_frame(ZPT_TEST_REQ_CAPTURE_DESCRIBE, &describe, 1);
    zassert_ok(rx_frame(&frame_type, payload, &length, ZPT_RX_TIMEOUT_MS));
    zassert_equal(frame_type, ZPT_TEST_RESP_CAPTURE_DESCRIPTION);
    static const uint8_t named[] = {0,   12,  't', 'e', 's', 't', '-',
                                    'c', 'a', 'p', 't', 'u', 'r', 'e'};
    zassert_mem_equal(payload, named, sizeof(named));
    zassert_equal(sys_get_le32(&payload[sizeof(named)]), hash);
    static const uint8_t orientation[] = {
        3,   22,  'o', 'r', 't', 'h', 'o', 'g', 'o', 'n', 'a', 'l', '-', 'o', 'r', 'i', 'e',
        'n', 't', 'a', 't', 'i', 'o', 'n', 1,   'o', 3,   7,   's', 'w', 'a', 'p', '-', 'x',
        'y', 0,   0,   0,   0,   8,   'i', 'n', 'v', 'e', 'r', 't', '-', 'x', 0,   0,   0,
        0,   8,   'i', 'n', 'v', 'e', 'r', 't', '-', 'y', 1,   0,   0,   0,
    };
    offset = sizeof(named) + 4;
    zassert_mem_equal(&payload[offset], orientation, sizeof(orientation));
    offset += sizeof(orientation);
    /* Normalize takes nothing; the gate takes the guard's window. */
    offset += 1 + strlen("resolution-normalize-mm") + 2 + 1;
    offset += 1 + strlen("coherent-displacement") + 1 + strlen("zpt-test-target-a");
    zassert_equal(payload[offset], 1);
    zassert_equal(payload[offset + 1], strlen("suppress-after-keypress-ms"));
    offset += 2 + strlen("suppress-after-keypress-ms");
    zassert_equal(sys_get_le32(&payload[offset]), 40);
    zassert_equal(offset + 4, length);
    const uint8_t unknown = 5;
    tx_frame(ZPT_TEST_REQ_CAPTURE_DESCRIBE, &unknown, 1);
    const uint8_t failure[] = {
        ZPT_TEST_REQ_CAPTURE_DESCRIBE, ZPT_TUNING_STATUS_UNKNOWN_TARGET, 5, 0xff, 0, 0, 0, 0};
    expect_frame(ZPT_RESP_TUNING_RESULT, failure, sizeof(failure));
}
