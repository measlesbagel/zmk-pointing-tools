/* SPDX-License-Identifier: MIT */
#define DT_DRV_COMPAT measlesbagel_zpt_telemetry

#include <errno.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

#include <zmk/pointing_tools/service/host_transport.h>
#if IS_ENABLED(CONFIG_ZMK_POINTING_TOOLS_RUNTIME_TUNING)
#include <zmk/pointing_tools/service/tuning.h>
#endif
#if IS_ENABLED(CONFIG_ZMK_POINTING_TOOLS_DEVICE_TABLE)
#include <zmk/pointing_tools/platform/zmk/device_table.h>
#endif
/* Only the central relays requests for devices owned by other halves. */
#define ZPT_REMOTE_DEVICES                                                                         \
    (IS_ENABLED(CONFIG_ZMK_POINTING_TOOLS_SENSOR_TUNNEL) &&                                        \
     IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL))
#if ZPT_REMOTE_DEVICES
#include <zmk/pointing_tools/platform/zmk/sensor_control_proxy.h>
#endif
#if IS_ENABLED(CONFIG_ZMK_POINTING_TOOLS_STATE_TELEMETRY)
#include <zmk/pointing_tools/observer/state.h>
#endif
#if IS_ENABLED(CONFIG_ZMK_POINTING_TOOLS_INPUT_CAPTURE)
#include <zmk/pointing_tools/observer/capture.h>
#include <zmk/pointing_tools/service/capture.h>
#endif

/* ZMK_LOG_LEVEL is an application Kconfig symbol (zmk/app/Kconfig); it is
 * absent when this module builds against plain Zephyr (e.g. the
 * native_posix unit-test app), so fall back to INFO there. */
#ifdef CONFIG_ZMK_LOG_LEVEL
#define ZPT_TELEMETRY_LOG_LEVEL CONFIG_ZMK_LOG_LEVEL
#else
#define ZPT_TELEMETRY_LOG_LEVEL LOG_LEVEL_INF
#endif

LOG_MODULE_REGISTER(zpt_telemetry, ZPT_TELEMETRY_LOG_LEVEL);

BUILD_ASSERT(DT_NUM_INST_STATUS_OKAY(DT_DRV_COMPAT) == 1,
             "exactly one measlesbagel,zpt-telemetry node is supported");

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
#define ZPT_REQ_DEVICE_LIST 0x0d
#define ZPT_REQ_DEVICE_DESCRIBE 0x0e
#define ZPT_REQ_DEVICE_PREVIEW 0x0f
#define ZPT_REQ_CAPTURE_CONTROL 0x10
#define ZPT_REQ_CAPTURE_DESCRIBE 0x11
#define ZPT_RESP_DESCRIBE 0x81
#define ZPT_RESP_ACK 0x82
#define ZPT_RESP_TUNING_TARGETS 0x83
#define ZPT_RESP_TUNING_DESCRIBE 0x84
#define ZPT_RESP_TUNING_RESULT 0x85
#define ZPT_RESP_TUNING_HELP 0x86
#define ZPT_RESP_TUNING_TARGET_METADATA 0x87
#define ZPT_RESP_TUNING_PARAMETER_METADATA 0x88
#define ZPT_RESP_STATE_STATUS 0x89
#define ZPT_RESP_DEVICE_LIST 0x8a
#define ZPT_RESP_DEVICE_DESCRIPTION 0x8b
#define ZPT_RESP_CAPTURE_STATUS 0x8c
#define ZPT_RESP_CAPTURE_DESCRIPTION 0x8d
#define ZPT_EVENT_STATE 0x91
#define ZPT_EVENT_CAPTURE 0x92

#define ZPT_MAX_REQUEST_PAYLOAD 128
#define ZPT_MAX_DESCRIBE_PAYLOAD 512
/* A target description carries 24 bytes plus label and unit per parameter;
 * an acceleration curve's 18 parameters need about 800. */
#define ZPT_MAX_TUNING_DESCRIPTION_PAYLOAD 1024
#define ZPT_TUNING_ALL_TARGETS UINT8_MAX
#define ZPT_TUNING_BATCH_HEADER_SIZE 2
#define ZPT_TUNING_BATCH_VALUE_SIZE 5
#define ZPT_TUNING_MAX_BATCH_VALUES 20
#define ZPT_STATE_SCHEMA_VERSION 2
#define ZPT_STATE_LABEL_MAX_LENGTH 27

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
    /* Device results only: the split half owning the device didn't answer. */
    ZPT_TUNING_STATUS_UNREACHABLE = 5,
};

#if !IS_ENABLED(CONFIG_ZMK_POINTING_TOOLS_TELEMETRY_UART) &&                                       \
    !IS_ENABLED(CONFIG_ZMK_POINTING_TOOLS_TELEMETRY_BLE)
/* USB serial needs a measlesbagel,zpt-telemetry-uart chosen node; Bluetooth
 * needs CONFIG_ZMK_POINTING_TOOLS_TELEMETRY_BLE on a Bluetooth central. */
#error "the telemetry service needs a USB serial or Bluetooth transport"
#endif

/* Every link the protocol runs over. Each has its own parser, so frames
 * arriving on different links never interleave. */
static const struct zpt_host_transport *const zpt_transports[] = {
#if IS_ENABLED(CONFIG_ZMK_POINTING_TOOLS_TELEMETRY_UART)
    &zpt_uart_transport,
#endif
#if IS_ENABLED(CONFIG_ZMK_POINTING_TOOLS_TELEMETRY_BLE)
    &zpt_gatt_transport,
#endif
};

struct zpt_frame_parser {
    enum { MAGIC_0, MAGIC_1, TYPE, LENGTH_0, LENGTH_1, PAYLOAD } state;
    uint8_t type;
    uint16_t length;
    uint16_t received;
    uint8_t payload[ZPT_MAX_REQUEST_PAYLOAD];
};

static struct zpt_frame_parser zpt_parsers[ARRAY_SIZE(zpt_transports)];

/* The link that sent the latest request. Only the worker thread uses it;
 * it starts on the first link. */
static const struct zpt_host_transport *zpt_host_link;

enum zpt_record_kind {
    ZPT_RECORD_STATE,
};

/* Capture records don't use the queue; they wait in the capture buffer. */
struct zpt_telemetry_record {
    uint8_t kind;
#if IS_ENABLED(CONFIG_ZMK_POINTING_TOOLS_STATE_TELEMETRY)
    union {
        struct zpt_state_sample state;
    } sample;
#endif
};

/* K_MSGQ_DEFINE / K_SEM_DEFINE records are collected by the kernel from
 * their linker sections at boot; both macros have no static variant. */
// NOLINTNEXTLINE(misc-use-internal-linkage)
K_MSGQ_DEFINE(zpt_record_queue, sizeof(struct zpt_telemetry_record),
              CONFIG_ZMK_POINTING_TOOLS_TELEMETRY_QUEUE_SIZE, 4);
K_SEM_DEFINE(zpt_wake, 0, 1); // NOLINT(misc-use-internal-linkage)

static atomic_t zpt_sequence;
static atomic_t zpt_last_contact;
#if IS_ENABLED(CONFIG_ZMK_POINTING_TOOLS_INPUT_CAPTURE)
static atomic_t zpt_capture_dropped;
#endif
#if IS_ENABLED(CONFIG_ZMK_POINTING_TOOLS_STATE_TELEMETRY)
static atomic_t zpt_state_dropped;
/* One past the highest state-only target id handed out; status responses
 * enumerate every id below it (and below the tuning count). */
static atomic_t zpt_state_slot_limit;
static atomic_t zpt_state_levels[CONFIG_ZMK_POINTING_TOOLS_TUNING_MAX_TARGETS];
/* A non-NULL label marks a state-only slot as allocated. Registration runs
 * from device init, before the telemetry thread starts reading the table. */
static const char *zpt_state_labels[CONFIG_ZMK_POINTING_TOOLS_TUNING_MAX_TARGETS];
#endif

#if IS_ENABLED(CONFIG_ZMK_POINTING_TOOLS_STATE_TELEMETRY)

enum zpt_state_level zpt_state_telemetry_level(uint8_t target_id) {
    if (target_id >= ARRAY_SIZE(zpt_state_levels)) {
        return ZPT_STATE_LEVEL_OFF;
    }
    return (enum zpt_state_level)atomic_get(&zpt_state_levels[target_id]);
}

static int find_state_target(const char *stable_id) {
    /* A tunable stage reports state under its tuning target id. */
    for (size_t index = 0; index < zpt_tuning_target_count(); index++) {
        if (strcmp(zpt_tuning_target_get((uint8_t)index)->stable_id, stable_id) == 0) {
            return (int)index;
        }
    }
    /* A stage shared by several pipelines is observed once per pipeline but
     * is one target. */
    for (size_t index = zpt_tuning_target_count(); index < ARRAY_SIZE(zpt_state_labels); index++) {
        if (zpt_state_labels[index] != NULL && strcmp(zpt_state_labels[index], stable_id) == 0) {
            return (int)index;
        }
    }
    return -ENOENT;
}

int zpt_state_telemetry_register_target(uint8_t *target_id, const char *stable_id) {
    if (target_id == NULL || stable_id == NULL) {
        return -EINVAL;
    }
    const int existing = find_state_target(stable_id);
    if (existing >= 0) {
        *target_id = (uint8_t)existing;
        return 0;
    }
    /* Allocation is tracked by label, not by telemetry level: every level is
     * OFF at boot, so a level-based "free" test handed the same id to every
     * stage registered during init. */
    for (size_t index = zpt_tuning_target_count(); index < ARRAY_SIZE(zpt_state_levels); index++) {
        if (zpt_state_labels[index] == NULL) {
            *target_id = (uint8_t)index;
            zpt_state_labels[index] = stable_id;
            const atomic_val_t limit = (atomic_val_t)index + 1;
            if (atomic_get(&zpt_state_slot_limit) < limit) {
                atomic_set(&zpt_state_slot_limit, limit);
            }
            return 0;
        }
    }
    return -ENOSPC;
}

void zpt_state_telemetry_submit(const struct zpt_state_sample *sample) {
    if (sample == NULL || zpt_state_telemetry_level(sample->target_id) == ZPT_STATE_LEVEL_OFF) {
        return;
    }

    struct zpt_telemetry_record record = {
        .kind = ZPT_RECORD_STATE,
        .sample.state = *sample,
    };
    record.sample.state.sequence = (uint32_t)atomic_inc(&zpt_sequence);
    if (k_msgq_put(&zpt_record_queue, &record, K_NO_WAIT) < 0) {
        atomic_inc(&zpt_state_dropped);
        return;
    }
    k_sem_give(&zpt_wake);
}

static void zpt_state_disable_all(void) {
    for (size_t i = 0; i < ARRAY_SIZE(zpt_state_levels); i++) {
        atomic_clear(&zpt_state_levels[i]);
    }
}

#endif

#if IS_ENABLED(CONFIG_ZTEST)
/* Unit tests make the link refuse this many droppable frames, as a
 * Bluetooth link out of credits does. */
atomic_t zpt_test_refuse_droppable; // NOLINT(misc-use-internal-linkage)
#endif

static int zpt_send_frame_on_link(uint8_t type, const uint8_t *payload, uint16_t length,
                                  bool droppable) {
#if IS_ENABLED(CONFIG_ZTEST)
    if (droppable && atomic_get(&zpt_test_refuse_droppable) > 0) {
        atomic_dec(&zpt_test_refuse_droppable);
        return -ENOBUFS;
    }
#endif
    const uint8_t header[] = {ZPT_FRAME_MAGIC_0, ZPT_FRAME_MAGIC_1, type, (uint8_t)(length & 0xff),
                              (uint8_t)(length >> 8)};
    return zpt_host_link->send(header, sizeof(header), payload, length, droppable);
}

static void zpt_send_frame(uint8_t type, const uint8_t *payload, uint16_t length) {
    const int ret = zpt_send_frame_on_link(type, payload, length, false);
    if (ret < 0) {
        LOG_WRN("Response 0x%02x not sent over %s (%d)", type, zpt_host_link->name, ret);
    }
}

static void zpt_send_describe(void) {
    uint8_t payload[1] = {ZPT_PROTOCOL_VERSION};
    zpt_send_frame(ZPT_RESP_DESCRIBE, payload, sizeof(payload));
}

static void zpt_send_ack(void) {
    uint8_t payload[8] = {0};
#if IS_ENABLED(CONFIG_ZMK_POINTING_TOOLS_STATE_TELEMETRY)
    sys_put_le32((uint32_t)atomic_get(&zpt_state_dropped), &payload[0]);
#endif
#if IS_ENABLED(CONFIG_ZMK_POINTING_TOOLS_INPUT_CAPTURE)
    sys_put_le32((uint32_t)atomic_get(&zpt_capture_dropped), &payload[4]);
#endif
    zpt_send_frame(ZPT_RESP_ACK, payload, sizeof(payload));
}

#if IS_ENABLED(CONFIG_ZMK_POINTING_TOOLS_STATE_TELEMETRY)

static void zpt_send_state(const struct zpt_state_sample *sample) {
    uint8_t payload[14 + ZPT_STATE_VALUE_COUNT * sizeof(int32_t)];
    payload[0] = sample->target_id;
    payload[1] = sample->target_kind;
    payload[2] = sample->event;
    payload[3] = sample->intent;
    sys_put_le16(sample->flags, &payload[4]);
    sys_put_le32(sample->timestamp_ms, &payload[6]);
    sys_put_le32(sample->sequence, &payload[10]);
    for (size_t i = 0; i < ZPT_STATE_VALUE_COUNT; i++) {
        sys_put_le32((uint32_t)sample->values[i], &payload[14 + i * sizeof(int32_t)]);
    }
    /* A link without room refuses the whole sample; the drop counter
     * reports it like a queue overflow. */
    if (zpt_send_frame_on_link(ZPT_EVENT_STATE, payload, sizeof(payload), true) < 0) {
        atomic_inc(&zpt_state_dropped);
    }
}

static void zpt_send_state_status(void) {
    uint8_t payload[8 + CONFIG_ZMK_POINTING_TOOLS_TUNING_MAX_TARGETS *
                            (3 + ZPT_STATE_LABEL_MAX_LENGTH)];
    const size_t target_count =
        MIN(MAX(zpt_tuning_target_count(), (size_t)atomic_get(&zpt_state_slot_limit)),
            ARRAY_SIZE(zpt_state_levels));
    payload[0] = ZPT_STATE_SCHEMA_VERSION;
    sys_put_le32((uint32_t)atomic_get(&zpt_state_dropped), &payload[1]);
    sys_put_le16(CONFIG_ZMK_POINTING_TOOLS_TELEMETRY_QUEUE_SIZE, &payload[5]);
    payload[7] = target_count;
    size_t offset = 8;
    for (size_t i = 0; i < target_count; i++) {
        const struct zpt_tuning_target *target = zpt_tuning_target_get((uint8_t)i);
        const char *label = target != NULL ? target->label : zpt_state_labels[i];
        const size_t label_length =
            label != NULL ? MIN(strlen(label), ZPT_STATE_LABEL_MAX_LENGTH) : 0;
        payload[offset++] = i;
        payload[offset++] = atomic_get(&zpt_state_levels[i]);
        payload[offset++] = label_length;
        if (label_length > 0) {
            /* Binary protocol payload: the buffer is not a string, so a
             * trailing NUL is neither guaranteed nor needed. */
            // NOLINTNEXTLINE(bugprone-not-null-terminated-result)
            memcpy(&payload[offset], label, label_length);
            offset += label_length;
        }
    }
    zpt_send_frame(ZPT_RESP_STATE_STATUS, payload, offset);
}

static void zpt_handle_state_control(const uint8_t *payload, uint16_t length) {
    if (length == 2) {
        const uint8_t target_id = payload[0];
        const uint8_t level = payload[1];
        if (level <= ZPT_STATE_LEVEL_VERBOSE && target_id == ZPT_STATE_ALL_TARGETS) {
            for (size_t i = 0; i < ARRAY_SIZE(zpt_state_levels); i++) {
                atomic_set(&zpt_state_levels[i], level);
            }
        } else if (level <= ZPT_STATE_LEVEL_VERBOSE && target_id < ARRAY_SIZE(zpt_state_levels)) {
            atomic_set(&zpt_state_levels[target_id], level);
        }
    }
    zpt_send_state_status();
}

#endif

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
    /* Read-only devices reject previews; the requested change is not
     * applicable to the addressed device. */
    case -ENOSYS:
        return ZPT_TUNING_STATUS_INVALID_VALUE;
    case -EAGAIN:
        return ZPT_TUNING_STATUS_UNREACHABLE;
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

/* Responses too large for the worker's stack: tuning target descriptions,
 * and the capture status and pipeline description. Only the worker thread
 * builds them, and each is sent before the next is built. */
static uint8_t zpt_large_payload[ZPT_MAX_TUNING_DESCRIPTION_PAYLOAD];

static void zpt_send_tuning_description(uint8_t target_id) {
    const struct zpt_tuning_target *target = zpt_tuning_target_get(target_id);
    if (target == NULL) {
        zpt_send_tuning_result(ZPT_REQ_TUNING_DESCRIBE, -ENODEV, target_id, UINT8_MAX, 0);
        return;
    }

    size_t offset = 0;
    zpt_large_payload[offset++] = target_id;
    zpt_large_payload[offset++] = MIN(target->parameter_count, UINT8_MAX);

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
        /* Target getters contractually return 0 or a negative errno; any
         * other value leaves 'current' unread, so treat it as failure. */
        if (ret != 0) {
            zpt_send_tuning_result(ZPT_REQ_TUNING_DESCRIBE, ret, target_id, parameter->id, 0);
            return;
        }
        if (offset + encoded_length > sizeof(zpt_large_payload)) {
            LOG_WRN("Tuning description full after %u parameters", (unsigned int)i);
            zpt_large_payload[1] = i;
            break;
        }

        /* The bounds check above reserves exactly encoded_length =
         * 24 + label_length + unit_length bytes, which is everything the
         * block writes below; the static analyzer cannot see through the
         * symbolic lengths and flags the first write anyway. */
        // NOLINTNEXTLINE(clang-analyzer-security.ArrayBound)
        zpt_large_payload[offset++] = parameter->id;
        zpt_large_payload[offset++] = parameter->type;
        sys_put_le32((uint32_t)parameter->minimum, &zpt_large_payload[offset]);
        offset += 4;
        sys_put_le32((uint32_t)parameter->maximum, &zpt_large_payload[offset]);
        offset += 4;
        sys_put_le32((uint32_t)parameter->step, &zpt_large_payload[offset]);
        offset += 4;
        sys_put_le32((uint32_t)compiled, &zpt_large_payload[offset]);
        offset += 4;
        sys_put_le32((uint32_t)current, &zpt_large_payload[offset]);
        offset += 4;
        zpt_large_payload[offset++] = label_length;
        zpt_large_payload[offset++] = unit_length;
        // NOLINTNEXTLINE(clang-analyzer-security.ArrayBound)
        memcpy(&zpt_large_payload[offset], parameter->label, label_length);
        offset += label_length;
        // NOLINTNEXTLINE(clang-analyzer-security.ArrayBound)
        memcpy(&zpt_large_payload[offset], parameter->unit, unit_length);
        offset += unit_length;
    }
    zpt_send_frame(ZPT_RESP_TUNING_DESCRIBE, zpt_large_payload, offset);
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

#if IS_ENABLED(CONFIG_ZMK_POINTING_TOOLS_INPUT_CAPTURE)

#define ZPT_CAPTURE_NONE UINT8_MAX
#define ZPT_CAPTURE_LABEL_MAX_LENGTH 27
#define ZPT_CAPTURE_STATUS_HEADER_SIZE 15
/* id, active, stage count, flags, settings hash, label length. */
#define ZPT_CAPTURE_STATUS_ENTRY_SIZE (9 + ZPT_CAPTURE_LABEL_MAX_LENGTH)
#define ZPT_CAPTURE_FLAG_REPLAYABLE 0x01

BUILD_ASSERT(ZPT_CAPTURE_STATUS_HEADER_SIZE +
                     CONFIG_ZMK_POINTING_TOOLS_INPUT_CAPTURE_MAX_PIPELINES *
                         ZPT_CAPTURE_STATUS_ENTRY_SIZE <=
                 sizeof(zpt_large_payload),
             "capture status exceeds the response buffer");

struct zpt_capture_entry {
    struct zpt_pipeline *pipeline;
    struct zpt_capture_tap tap;
    struct zpt_capture_sink sink;
};

static void zpt_drain_records(void);

static struct zpt_capture_entry
    zpt_capture_entries[CONFIG_ZMK_POINTING_TOOLS_INPUT_CAPTURE_MAX_PIPELINES];
static size_t zpt_capture_count;
/* The entry being recorded, or ZPT_CAPTURE_NONE. Read on every frame of
 * every registered pipeline, the only cost while nothing records. */
static atomic_t zpt_capture_recording = ATOMIC_INIT(ZPT_CAPTURE_NONE);
/* Records were dropped or settings changed since the tap last resynced. */
static atomic_t zpt_capture_resync;
/* The recorded pipeline's settings hash, for its entered records. Set
 * while no frame runs: at the start and when a tuning change lands. */
static atomic_t zpt_capture_hash;
/* Records of the running recording, encoded, until the link has room for
 * them. Taps on the input thread and the worker share it under the lock;
 * the lock also orders the recording flag with the records. */
static struct k_spinlock zpt_capture_lock;
static uint8_t zpt_capture_storage[CONFIG_ZMK_POINTING_TOOLS_INPUT_CAPTURE_BUFFER_SIZE];
static struct zpt_capture_buffer zpt_capture_buffer = {
    .data = zpt_capture_storage,
    .capacity = sizeof(zpt_capture_storage),
};

static bool zpt_capture_entry_recording(void *context) {
    const struct zpt_capture_entry *entry = context;
    return atomic_get(&zpt_capture_recording) == (atomic_val_t)(entry - zpt_capture_entries);
}

static void zpt_capture_count_drop(atomic_val_t records) {
    atomic_add(&zpt_capture_dropped, records);
    atomic_set(&zpt_capture_resync, 1);
}

static void zpt_capture_submit(void *context, const struct zpt_capture_record *capture) {
    ARG_UNUSED(context);
    bool stored = true;
    /* Records are numbered and stored in one step, so the host reads every
     * gap as a drop; none lands after the recording stopped. Encoding and
     * storing a record masks interrupts for a few hundred cycles. */
    K_SPINLOCK(&zpt_capture_lock) {
        if (atomic_get(&zpt_capture_recording) != ZPT_CAPTURE_NONE) {
            stored = zpt_capture_buffer_put(&zpt_capture_buffer, capture);
        }
    }
    /* A full buffer drops the record; the host sees the sequence gap. */
    if (!stored) {
        zpt_capture_count_drop(1);
        return;
    }
    k_sem_give(&zpt_wake);
}

static uint16_t zpt_capture_sub_ms_us(void *context, uint32_t time_ms) {
    ARG_UNUSED(context);
    const uint64_t now_us = k_ticks_to_us_floor64(k_uptime_ticks());
    /* A frame that waited past its millisecond keeps that millisecond, which
     * is what its stages see, and sorts last within it. */
    return (uint32_t)(now_us / 1000U) == time_ms ? (uint16_t)(now_us % 1000U) : 999U;
}

static bool zpt_capture_take_resync(void *context) {
    ARG_UNUSED(context);
    return atomic_clear(&zpt_capture_resync) != 0;
}

static uint32_t zpt_capture_current_hash(void *context) {
    ARG_UNUSED(context);
    return (uint32_t)atomic_get(&zpt_capture_hash);
}

static uint32_t zpt_capture_settings_hash(const struct zpt_pipeline *pipeline);

int zpt_capture_register_pipeline(struct zpt_pipeline *pipeline) {
    if (pipeline == NULL) {
        return -EINVAL;
    }
    if (zpt_capture_count >= ARRAY_SIZE(zpt_capture_entries)) {
        return -ENOSPC;
    }
    struct zpt_capture_entry *entry = &zpt_capture_entries[zpt_capture_count];
    entry->sink = (struct zpt_capture_sink){
        .recording = zpt_capture_entry_recording,
        .submit = zpt_capture_submit,
        .sub_ms_us = zpt_capture_sub_ms_us,
        .take_resync = zpt_capture_take_resync,
        .settings_hash = zpt_capture_current_hash,
        .context = entry,
    };
    entry->pipeline = pipeline;
    zpt_capture_tap_attach(&entry->tap, pipeline, &entry->sink);
    zpt_capture_count++;
    return 0;
}

void zpt_capture_settings_changed(void) {
    const atomic_val_t recording = atomic_get(&zpt_capture_recording);
    if (recording == ZPT_CAPTURE_NONE) {
        return;
    }
    /* Called with every router held, after the change landed: the next
     * entered record, at the resync this asks for, carries the new hash. */
    atomic_set(&zpt_capture_hash,
               (atomic_val_t)zpt_capture_settings_hash(zpt_capture_entries[recording].pipeline));
    const struct zpt_capture_record record = {.kind = ZPT_CAPTURE_RECORD_SETTINGS_CHANGED};
    zpt_capture_submit(NULL, &record);
    atomic_set(&zpt_capture_resync, 1);
}

/* Send stored records while the link has room. With wait, as when a
 * recording ends, send them all, waiting for room; a link that fails then
 * loses what is left, counted as dropped. */
static void zpt_capture_drain(bool wait) {
    for (;;) {
        /* A link with no room keeps the records; skip encoding a batch it
         * would refuse. */
        if (!wait && zpt_host_link->has_room != NULL && !zpt_host_link->has_room()) {
            return;
        }
        /* Copy the bookkeeping under the lock and build the batch from the
         * copy without it, so interrupts stay enabled while records are
         * copied out. Taps only write past head + used, and only this
         * worker consumes, discards, or resets, so the copied range stays
         * intact until it is consumed below. */
        struct zpt_capture_buffer snapshot;
        K_SPINLOCK(&zpt_capture_lock) { snapshot = zpt_capture_buffer; }
        uint8_t payload[ZPT_CAPTURE_BATCH_MAX_PAYLOAD];
        struct zpt_capture_take take;
        const size_t length = zpt_capture_buffer_peek(&snapshot, payload, sizeof(payload), &take);
        if (length == 0U) {
            return;
        }
        if (zpt_send_frame_on_link(ZPT_EVENT_CAPTURE, payload, (uint16_t)length, !wait) < 0) {
            if (wait) {
                uint32_t lost;
                K_SPINLOCK(&zpt_capture_lock) {
                    lost = zpt_capture_buffer_discard(&zpt_capture_buffer);
                }
                zpt_capture_count_drop((atomic_val_t)lost);
            }
            return;
        }
        K_SPINLOCK(&zpt_capture_lock) { zpt_capture_buffer_consume(&zpt_capture_buffer, &take); }
    }
}

static void zpt_capture_stop(void) {
    K_SPINLOCK(&zpt_capture_lock) { atomic_set(&zpt_capture_recording, ZPT_CAPTURE_NONE); }
}

/* Runs while no frame is in any pipeline, so the reset and the first
 * records come before the recording's first frame. */
static void zpt_capture_start_between_frames(void *arg) {
    struct zpt_capture_entry *entry = arg;
    K_SPINLOCK(&zpt_capture_lock) { zpt_capture_buffer_reset(&zpt_capture_buffer); }
    atomic_clear(&zpt_capture_resync);
    atomic_set(&zpt_capture_hash, (atomic_val_t)zpt_capture_settings_hash(entry->pipeline));
    atomic_set(&zpt_capture_recording, (atomic_val_t)(entry - zpt_capture_entries));
    zpt_capture_tap_start(&entry->tap);
}

/* The sum of zpt_capture_settings_hash_add over the current values of
 * every tunable stage of the pipeline. */
static uint32_t zpt_capture_settings_hash(const struct zpt_pipeline *pipeline) {
    uint32_t hash = 0;
    for (size_t i = 0; i < pipeline->stage_count; i++) {
        const char *stable_id = pipeline->stages[i]->stable_id;
        for (size_t id = 0; id < zpt_tuning_target_count(); id++) {
            const struct zpt_tuning_target *target = zpt_tuning_target_get((uint8_t)id);
            if (strcmp(target->stable_id, stable_id) != 0) {
                continue;
            }
            for (size_t p = 0; p < target->parameter_count; p++) {
                const struct zpt_tuning_parameter *parameter = &target->parameters[p];
                int32_t value = 0;
                (void)zpt_tuning_get((uint8_t)id, parameter->id, false, &value);
                hash = zpt_capture_settings_hash_add(hash, stable_id, parameter->key, value);
            }
        }
    }
    return hash;
}

static void zpt_send_capture_status(void) {
    size_t offset = 0;
    zpt_large_payload[offset++] = ZPT_CAPTURE_SCHEMA_VERSION;
    zpt_large_payload[offset++] = (uint8_t)atomic_get(&zpt_capture_recording);
    sys_put_le32((uint32_t)atomic_get(&zpt_capture_dropped), &zpt_large_payload[offset]);
    offset += 4;
    uint32_t used;
    K_SPINLOCK(&zpt_capture_lock) { used = zpt_capture_buffer.used; }
    sys_put_le32(used, &zpt_large_payload[offset]);
    offset += 4;
    sys_put_le32(zpt_capture_buffer.capacity, &zpt_large_payload[offset]);
    offset += 4;
    zpt_large_payload[offset++] = (uint8_t)zpt_capture_count;
    for (size_t i = 0; i < zpt_capture_count; i++) {
        const struct zpt_capture_entry *entry = &zpt_capture_entries[i];
        const struct zpt_pipeline *pipeline = entry->pipeline;
        const size_t label_length = MIN(strlen(pipeline->stable_id), ZPT_CAPTURE_LABEL_MAX_LENGTH);
        zpt_large_payload[offset++] = (uint8_t)i;
        zpt_large_payload[offset++] = pipeline->active ? 1U : 0U;
        zpt_large_payload[offset++] = (uint8_t)pipeline->stage_count;
        zpt_large_payload[offset++] = entry->tap.replayable ? ZPT_CAPTURE_FLAG_REPLAYABLE : 0U;
        sys_put_le32(zpt_capture_settings_hash(pipeline), &zpt_large_payload[offset]);
        offset += 4;
        zpt_large_payload[offset++] = (uint8_t)label_length;
        // NOLINTNEXTLINE(bugprone-not-null-terminated-result)
        memcpy(&zpt_large_payload[offset], pipeline->stable_id, label_length);
        offset += label_length;
    }
    zpt_send_frame(ZPT_RESP_CAPTURE_STATUS, zpt_large_payload, (uint16_t)offset);
}

static void zpt_handle_capture_control(const uint8_t *payload, uint16_t length) {
    if (length == 1) {
        /* Stopping or switching recordings sends everything recorded so
         * far before the status that confirms it. */
        zpt_capture_stop();
        zpt_drain_records();
        zpt_capture_drain(true);
        if (payload[0] < zpt_capture_count) {
            int ret = zpt_capture_between_frames(zpt_capture_start_between_frames,
                                                 &zpt_capture_entries[payload[0]]);
            if (ret < 0) {
                zpt_capture_stop();
                LOG_WRN("Input capture didn't start (%d)", ret);
            }
        }
    }
    zpt_send_capture_status();
}

static void zpt_send_capture_description(const uint8_t *payload, uint16_t length) {
    if (length != 1 || payload[0] >= zpt_capture_count) {
        zpt_send_tuning_result(ZPT_REQ_CAPTURE_DESCRIBE, -ENODEV,
                               length == 1 ? payload[0] : UINT8_MAX, UINT8_MAX, 0);
        return;
    }
    const struct zpt_pipeline *pipeline = zpt_capture_entries[payload[0]].pipeline;
    const int ret =
        zpt_capture_describe_pipeline(pipeline, payload[0], zpt_capture_settings_hash(pipeline),
                                      zpt_large_payload, sizeof(zpt_large_payload));
    if (ret < 0) {
        zpt_send_tuning_result(ZPT_REQ_CAPTURE_DESCRIBE, ret, payload[0], UINT8_MAX, 0);
        return;
    }
    zpt_send_frame(ZPT_RESP_CAPTURE_DESCRIPTION, zpt_large_payload, (uint16_t)ret);
}

#endif /* CONFIG_ZMK_POINTING_TOOLS_INPUT_CAPTURE */

#if IS_ENABLED(CONFIG_ZMK_POINTING_TOOLS_RUNTIME_TUNING)
static void zpt_dispatch_tuning(uint8_t type, const uint8_t *payload, uint16_t length) {
    switch (type) {
    case ZPT_REQ_TUNING_TARGETS:
        zpt_send_tuning_targets();
        break;
    case ZPT_REQ_TUNING_DESCRIBE:
        if (length == 1) {
            zpt_send_tuning_description(payload[0]);
        } else {
            zpt_send_tuning_result(type, -EINVAL, UINT8_MAX, UINT8_MAX, 0);
        }
        break;
    case ZPT_REQ_TUNING_SET:
        zpt_handle_tuning_set(payload, length);
        break;
    case ZPT_REQ_TUNING_RESET:
        zpt_handle_tuning_reset(payload, length);
        break;
    case ZPT_REQ_TUNING_HELP:
        if (length == 2) {
            zpt_send_tuning_help(payload[0], payload[1]);
        } else {
            zpt_send_tuning_result(type, -EINVAL, UINT8_MAX, UINT8_MAX, 0);
        }
        break;
    case ZPT_REQ_TUNING_TARGET_METADATA:
        if (length == 1) {
            zpt_send_tuning_target_metadata(payload[0]);
        } else {
            zpt_send_tuning_result(type, -EINVAL, UINT8_MAX, UINT8_MAX, 0);
        }
        break;
    case ZPT_REQ_TUNING_PARAMETER_METADATA:
        if (length == 2) {
            zpt_send_tuning_parameter_metadata(payload[0], payload[1]);
        } else {
            zpt_send_tuning_result(type, -EINVAL, UINT8_MAX, UINT8_MAX, 0);
        }
        break;
    case ZPT_REQ_TUNING_SET_MANY:
        zpt_handle_tuning_set_many(payload, length);
        break;
    default:
        break;
    }
}
#endif

#if IS_ENABLED(CONFIG_ZMK_POINTING_TOOLS_RUNTIME_TUNING)
static bool zpt_is_tuning_request(uint8_t type) {
    switch (type) {
    case ZPT_REQ_TUNING_TARGETS:
    case ZPT_REQ_TUNING_DESCRIBE:
    case ZPT_REQ_TUNING_SET:
    case ZPT_REQ_TUNING_RESET:
    case ZPT_REQ_TUNING_HELP:
    case ZPT_REQ_TUNING_TARGET_METADATA:
    case ZPT_REQ_TUNING_PARAMETER_METADATA:
    case ZPT_REQ_TUNING_SET_MANY:
        return true;
    default:
        return false;
    }
}
#endif

#if IS_ENABLED(CONFIG_ZMK_POINTING_TOOLS_DEVICE_TABLE) &&                                          \
    IS_ENABLED(CONFIG_ZMK_POINTING_TOOLS_RUNTIME_TUNING)

#define ZPT_DEVICE_FLAG_REACHABLE 0x01
#define ZPT_DEVICE_FLAG_SETTABLE 0x02
#define ZPT_DEVICE_DESCRIPTION_SETTABLE 0x01
#define ZPT_DEVICE_DESCRIPTION_STALE 0x02
/* Description bytes besides the two strings: stable-id length (1), path
 * length (2), current and compiled CPI (4), flags (1), and the range form's
 * reserved count byte plus min, max and step (7). */
#define ZPT_DEVICE_DESCRIPTION_FIXED_SIZE 15

/* Device discovery and preview reuse the tuning result message so hosts
 * keep one error-status mapping; device ids ride the parameter-id byte. */
static void zpt_send_device_result(uint8_t request_type, int error, uint8_t device_id,
                                   uint16_t value) {
    zpt_send_tuning_result(request_type, error, UINT8_MAX, device_id, (int32_t)value);
}

static void zpt_send_device_list(void) {
    uint8_t payload[ZPT_MAX_DESCRIBE_PAYLOAD];
    size_t offset = 0;
    const size_t count = MIN(zpt_device_table_count(), UINT8_MAX);

    payload[offset++] = (uint8_t)count;
    for (size_t i = 0; i < count; i++) {
        const struct zpt_pointing_device *device = zpt_device_table_at(i);
        const size_t label_length = MIN(strlen(device->stable_id), UINT8_MAX);
        if (offset + 5 + label_length > sizeof(payload)) {
            LOG_WRN("Device list payload full after %u devices", (unsigned int)i);
            payload[0] = (uint8_t)i;
            break;
        }
        /* Location 0 is central-local and always reachable. Entries owned
         * by other halves are reachable when their half answered the last
         * request sent to it. */
        const uint8_t location = device->location;
        bool reachable = location == 0;
#if ZPT_REMOTE_DEVICES
        reachable = reachable || zpt_sensor_control_remote_reachable(device);
#endif
        const uint8_t flags = (uint8_t)((reachable ? ZPT_DEVICE_FLAG_REACHABLE : 0x00) |
                                        (device->caps.settable ? ZPT_DEVICE_FLAG_SETTABLE : 0x00));
        payload[offset++] = device->id;
        payload[offset++] = location;
        payload[offset++] = flags;
        payload[offset++] = (uint8_t)label_length;
        memcpy(&payload[offset], device->stable_id, label_length);
        offset += label_length;
    }
    zpt_send_frame(ZPT_RESP_DEVICE_LIST, payload, offset);
}

static void zpt_send_device_description(uint8_t device_id) {
    const struct zpt_pointing_device *device = zpt_device_table_at(device_id);

    if (device == NULL) {
        zpt_send_device_result(ZPT_REQ_DEVICE_DESCRIBE, -ENODEV, device_id, 0);
        return;
    }

    const size_t stable_id_length = MIN(strlen(device->stable_id), UINT8_MAX);
    const char *dt_path = device->devicetree_path;
    const size_t dt_path_length = MIN(strlen(dt_path), UINT16_MAX);
    /* Everything but the discrete values must fit, with the range form as
     * the larger alternative; the value list is truncated to the rest. */
    if (ZPT_DEVICE_DESCRIPTION_FIXED_SIZE + stable_id_length + dt_path_length >
        ZPT_MAX_DESCRIBE_PAYLOAD) {
        zpt_send_device_result(ZPT_REQ_DEVICE_DESCRIBE, -ENOSPC, device_id, 0);
        return;
    }

    uint16_t current = 0;
    bool stale = false;
    int ret = 0;
#if ZPT_REMOTE_DEVICES
    if (device->location != 0) {
        /* Live read through the tunnel. When the half can't be reached,
         * describe the device anyway with the last known value, marked
         * stale. */
        stale = zpt_sensor_control_get_remote(device, &current) < 0;
    } else
#endif
    {
        ret = zpt_device_control_get(device, &current);
    }
    if (ret < 0) {
        zpt_send_device_result(ZPT_REQ_DEVICE_DESCRIBE, ret, device_id, 0);
        return;
    }

    uint8_t payload[ZPT_MAX_DESCRIBE_PAYLOAD];
    size_t offset = 0;
    payload[offset++] = (uint8_t)stable_id_length;
    memcpy(&payload[offset], device->stable_id, stable_id_length);
    offset += stable_id_length;
    sys_put_le16((uint16_t)dt_path_length, &payload[offset]);
    offset += 2;
    memcpy(&payload[offset], dt_path, dt_path_length);
    offset += dt_path_length;
    sys_put_le16(current, &payload[offset]);
    offset += 2;
    sys_put_le16(device->default_cpi, &payload[offset]);
    offset += 2;
    payload[offset++] = (uint8_t)((device->caps.settable ? ZPT_DEVICE_DESCRIPTION_SETTABLE : 0) |
                                  (stale ? ZPT_DEVICE_DESCRIPTION_STALE : 0));
    if (device->caps.settable && device->caps.discrete) {
        const size_t values = MIN(device->caps.list_count, (sizeof(payload) - offset - 1) / 2U);
        if (values < device->caps.list_count) {
            LOG_WRN("Device description truncated after %u values", (unsigned int)values);
        }
        payload[offset++] = (uint8_t)values;
        for (size_t i = 0; i < values; i++) {
            sys_put_le16(device->caps.list_values[i], &payload[offset]);
            offset += 2;
        }
    } else if (device->caps.settable) {
        /* Range form: reserved count byte, then min/max/step. */
        payload[offset++] = 0;
        sys_put_le16(device->caps.range_min, &payload[offset]);
        offset += 2;
        sys_put_le16(device->caps.range_max, &payload[offset]);
        offset += 2;
        sys_put_le16(device->caps.range_step, &payload[offset]);
        offset += 2;
    }
    zpt_send_frame(ZPT_RESP_DEVICE_DESCRIPTION, payload, offset);
}

static void zpt_handle_device_preview(const uint8_t *payload, uint16_t length) {
    if (length != 3) {
        zpt_send_device_result(ZPT_REQ_DEVICE_PREVIEW, -EINVAL, UINT8_MAX, 0);
        return;
    }

    const uint16_t requested = (uint16_t)(payload[1] | (uint16_t)payload[2] << 8);
    const struct zpt_pointing_device *device = zpt_device_table_at(payload[0]);
    if (device == NULL) {
        zpt_send_device_result(ZPT_REQ_DEVICE_PREVIEW, -ENODEV, payload[0], 0);
        return;
    }

#if ZPT_REMOTE_DEVICES
    if (device->location != 0) {
        uint16_t effective = 0;
        int tunnel_ret = zpt_sensor_control_preview_remote(device, requested, &effective);
        /* Snapped successes report as plain OK alongside their effective
         * value, matching the local path. */
        zpt_send_device_result(ZPT_REQ_DEVICE_PREVIEW, tunnel_ret < 0 ? tunnel_ret : 0, payload[0],
                               effective);
        return;
    }
#else
    if (device->location != 0) {
        /* Peripheral routing requires the sensor-control tunnel. */
        zpt_send_device_result(ZPT_REQ_DEVICE_PREVIEW, -ENOSYS, payload[0], 0);
        return;
    }
#endif

    uint16_t effective = 0;
    int ret = zpt_device_control_preview(device, requested, &effective);
    /* Snapped successes report as plain OK alongside their effective value;
     * hosts compare requested against effective to offer a correction. */
    zpt_send_device_result(ZPT_REQ_DEVICE_PREVIEW, ret < 0 ? ret : 0, payload[0], effective);
}
#endif

/* k_uptime_get_32() is uint32_t and atomic_val_t is signed; the explicit
 * cast documents the intended bit-exact round trip (readers cast back with
 * (uint32_t)atomic_get) and keeps bugprone-narrowing-conversions quiet. */
static void zpt_note_contact(void) {
    atomic_set(&zpt_last_contact, (atomic_val_t)k_uptime_get_32());
}

static void zpt_dispatch(uint8_t type, const uint8_t *payload, uint16_t length) {
#if IS_ENABLED(CONFIG_ZMK_POINTING_TOOLS_RUNTIME_TUNING)
    if (zpt_is_tuning_request(type)) {
        zpt_note_contact();
        zpt_dispatch_tuning(type, payload, length);
        return;
    }
#endif
#if IS_ENABLED(CONFIG_ZMK_POINTING_TOOLS_DEVICE_TABLE) &&                                          \
    IS_ENABLED(CONFIG_ZMK_POINTING_TOOLS_RUNTIME_TUNING)
    switch (type) {
    case ZPT_REQ_DEVICE_LIST:
        zpt_note_contact();
        zpt_send_device_list();
        return;
    case ZPT_REQ_DEVICE_DESCRIBE:
        zpt_note_contact();
        if (length == 1) {
            zpt_send_device_description(payload[0]);
        } else {
            zpt_send_device_result(type, -EINVAL, UINT8_MAX, 0);
        }
        return;
    case ZPT_REQ_DEVICE_PREVIEW:
        zpt_note_contact();
        zpt_handle_device_preview(payload, length);
        return;
    default:
        break;
    }
#endif
    switch (type) {
    case ZPT_REQ_DESCRIBE:
        zpt_note_contact();
        zpt_send_describe();
        break;
    case ZPT_REQ_PING:
        zpt_note_contact();
        zpt_send_ack();
        break;
#if IS_ENABLED(CONFIG_ZMK_POINTING_TOOLS_STATE_TELEMETRY)
    case ZPT_REQ_STATE_CONTROL:
        zpt_note_contact();
        zpt_handle_state_control(payload, length);
        break;
#endif
#if IS_ENABLED(CONFIG_ZMK_POINTING_TOOLS_INPUT_CAPTURE)
    case ZPT_REQ_CAPTURE_CONTROL:
        zpt_note_contact();
        zpt_handle_capture_control(payload, length);
        break;
    case ZPT_REQ_CAPTURE_DESCRIBE:
        zpt_note_contact();
        zpt_send_capture_description(payload, length);
        break;
#endif
    default:
        LOG_WRN("Unknown telemetry request 0x%02x", type);
        break;
    }
}

static void zpt_dispatch_frame(size_t link, const uint8_t *payload, uint16_t length) {
    /* Requests are answered on the link they arrived on, and state samples
     * follow the latest request. */
    zpt_host_link = zpt_transports[link];
    zpt_dispatch(zpt_parsers[link].type, payload, length);
}

static void zpt_parse_byte(size_t link, uint8_t byte) {
    struct zpt_frame_parser *parser = &zpt_parsers[link];

    switch (parser->state) {
    case MAGIC_0:
        if (byte == ZPT_FRAME_MAGIC_0) {
            parser->state = MAGIC_1;
        }
        break;
    case MAGIC_1:
        parser->state =
            byte == ZPT_FRAME_MAGIC_1 ? TYPE : (byte == ZPT_FRAME_MAGIC_0 ? MAGIC_1 : MAGIC_0);
        break;
    case TYPE:
        parser->type = byte;
        parser->state = LENGTH_0;
        break;
    case LENGTH_0:
        parser->length = byte;
        parser->state = LENGTH_1;
        break;
    case LENGTH_1:
        parser->length |= (uint16_t)byte << 8;
        parser->received = 0;
        if (parser->length > sizeof(parser->payload)) {
            parser->state = MAGIC_0;
        } else if (parser->length == 0) {
            zpt_dispatch_frame(link, NULL, 0);
            parser->state = MAGIC_0;
        } else {
            parser->state = PAYLOAD;
        }
        break;
    case PAYLOAD:
        parser->payload[parser->received++] = byte;
        if (parser->received == parser->length) {
            zpt_dispatch_frame(link, parser->payload, parser->length);
            parser->state = MAGIC_0;
        }
        break;
    }
}

void zpt_host_protocol_wake(void) { k_sem_give(&zpt_wake); }

static void zpt_receive(void) {
    for (size_t i = 0; i < ARRAY_SIZE(zpt_transports); i++) {
        const struct zpt_host_transport *link = zpt_transports[i];
        uint8_t byte;
        while (ring_buf_get(link->rx, &byte, 1) == 1) {
            /* Checked after taking each byte: the transport raises the flag
             * before queuing a new peer's bytes, so the first of them always
             * sees it and starts a fresh frame instead of completing the
             * previous peer's partial one. */
            if (link->rx_reset != NULL && atomic_clear(link->rx_reset)) {
                zpt_parsers[i].state = MAGIC_0;
            }
            zpt_parse_byte(i, byte);
        }
    }
}

static void zpt_drain_records(void) {
    struct zpt_telemetry_record record;
    while (k_msgq_get(&zpt_record_queue, &record, K_NO_WAIT) == 0) {
        const uint32_t elapsed = k_uptime_get_32() - (uint32_t)atomic_get(&zpt_last_contact);
        if (elapsed > CONFIG_ZMK_POINTING_TOOLS_TELEMETRY_HOST_TIMEOUT_MS) {
            k_msgq_purge(&zpt_record_queue);
#if IS_ENABLED(CONFIG_ZMK_POINTING_TOOLS_STATE_TELEMETRY)
            zpt_state_disable_all();
#endif
            return;
        }
#if IS_ENABLED(CONFIG_ZMK_POINTING_TOOLS_STATE_TELEMETRY)
        if (record.kind == ZPT_RECORD_STATE &&
            zpt_state_telemetry_level(record.sample.state.target_id) != ZPT_STATE_LEVEL_OFF) {
            zpt_send_state(&record.sample.state);
        }
#endif
    }
#if IS_ENABLED(CONFIG_ZMK_POINTING_TOOLS_INPUT_CAPTURE)
    if (atomic_get(&zpt_capture_recording) != ZPT_CAPTURE_NONE &&
        k_uptime_get_32() - (uint32_t)atomic_get(&zpt_last_contact) >
            CONFIG_ZMK_POINTING_TOOLS_TELEMETRY_HOST_TIMEOUT_MS) {
        /* The host is gone: stop, and drop what nobody will read. */
        zpt_capture_stop();
        K_SPINLOCK(&zpt_capture_lock) { (void)zpt_capture_buffer_discard(&zpt_capture_buffer); }
        return;
    }
    /* Send what is stored while the link has room. A link without room
     * keeps it; the worker retries when the link frees a credit (it wakes
     * the worker) or the next record arrives. */
    zpt_capture_drain(false);
#endif
}

static void zpt_thread(void) {
    zpt_host_link = zpt_transports[0];
    for (;;) {
        k_sem_take(&zpt_wake, K_FOREVER);

        zpt_receive();
        zpt_drain_records();
    }
}

/* K_THREAD_DEFINE expands to a conditional operator with identical
 * branches, a const-qualified typedef (k_tid_t), and non-static globals:
 * the thread record is collected by the kernel from the
 * _static_thread_data linker section at boot, and the handle is
 * documented as `extern const k_tid_t <name>`, so static linkage is
 * impossible. All of it is Zephyr macro artifact, not code in this file. */
// NOLINTNEXTLINE(bugprone-branch-clone,misc-misplaced-const,misc-use-internal-linkage)
K_THREAD_DEFINE(zpt_thread_id, CONFIG_ZMK_POINTING_TOOLS_TELEMETRY_THREAD_STACK_SIZE, zpt_thread,
                NULL, NULL, NULL, K_PRIO_PREEMPT(10), 0, 0);
