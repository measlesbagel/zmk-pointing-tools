/* SPDX-License-Identifier: MIT */

/* Bluetooth GATT transport for the host protocol, beside ZMK's HID service
 * on the central's host connection. The tuner writes request bytes to one
 * characteristic and receives response and telemetry bytes as notifications
 * from another. Both carry the same framed byte stream as USB serial, split
 * at arbitrary points (docs/protocol.md). */

#include <errno.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/l2cap.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/ring_buffer.h>
#include <zephyr/sys/util.h>

#include <zmk/pointing_tools/service/host_transport.h>

LOG_MODULE_DECLARE(zpt_telemetry);

/* Fixed UUIDs the tuner lists in Web Bluetooth's optionalServices. */
#define ZPT_GATT_UUID(first) BT_UUID_128_ENCODE(first, 0x531c, 0x49cf, 0x9d5d, 0x846a5db2398b)
#define ZPT_GATT_SERVICE_UUID ZPT_GATT_UUID(0xf336b450)
#define ZPT_GATT_REQUEST_UUID ZPT_GATT_UUID(0xf336b451)
#define ZPT_GATT_RESPONSE_UUID ZPT_GATT_UUID(0xf336b452)

static const struct bt_uuid_128 zpt_service_uuid = BT_UUID_INIT_128(ZPT_GATT_SERVICE_UUID);
static const struct bt_uuid_128 zpt_request_uuid = BT_UUID_INIT_128(ZPT_GATT_REQUEST_UUID);
static const struct bt_uuid_128 zpt_response_uuid = BT_UUID_INIT_128(ZPT_GATT_RESPONSE_UUID);

/* ATT notification header: opcode and attribute handle. */
#define ZPT_ATT_NOTIFY_OVERHEAD 3
#define ZPT_GATT_CHUNK_MAX (BT_L2CAP_TX_MTU - ZPT_ATT_NOTIFY_OVERHEAD)
#define ZPT_GATT_CREDITS CONFIG_ZMK_POINTING_TOOLS_TELEMETRY_BLE_TX_CREDITS

/* The ring buffer handle is documented as `extern struct ring_buf
 * <name>` (the storage is already static); the one-line macro has no
 * static variant. */
RING_BUF_DECLARE(zpt_gatt_rx_ring, 256); // NOLINT(misc-use-internal-linkage)
static atomic_t zpt_gatt_rx_reset;

/* Notifications handed to the stack and not yet sent. Notifications on one
 * connection leave in order, HID reports included, so the cap bounds how
 * many telemetry notifications a HID report can wait behind. K_SEM_DEFINE
 * records are collected from their linker section at boot and have no
 * static variant. */
// NOLINTNEXTLINE(misc-use-internal-linkage)
K_SEM_DEFINE(zpt_gatt_credits, ZPT_GATT_CREDITS, ZPT_GATT_CREDITS);

/* The connection that wrote the latest request; responses go only to it.
 * Only the Bluetooth thread writes it, so Bluetooth callbacks compare it
 * without the lock; the protocol worker takes a reference under the lock. */
static struct k_spinlock zpt_gatt_lock;
static struct bt_conn *zpt_gatt_conn;

struct zpt_gatt_bond_match {
    const bt_addr_le_t *addr;
    bool found;
};

static void zpt_gatt_match_bond(const struct bt_bond_info *info, void *user_data) {
    struct zpt_gatt_bond_match *match = user_data;
    match->found = match->found || bt_addr_le_eq(&info->addr, match->addr);
}

/* An encrypted link to a peer with stored keys. The connection's
 * destination is the peer's identity address once its private address is
 * resolved, which is the address bonds are stored under. */
static bool zpt_gatt_bonded(struct bt_conn *conn) {
    struct bt_conn_info info;
    if (bt_conn_get_info(conn, &info) < 0 || info.type != BT_CONN_TYPE_LE ||
        bt_conn_get_security(conn) < BT_SECURITY_L2) {
        return false;
    }
    struct zpt_gatt_bond_match match = {.addr = info.le.dst, .found = false};
    bt_foreach_bond(info.id, zpt_gatt_match_bond, &match);
    return match.found;
}

static void zpt_gatt_set_conn(struct bt_conn *conn) {
    struct bt_conn *previous;
    K_SPINLOCK(&zpt_gatt_lock) {
        previous = zpt_gatt_conn;
        zpt_gatt_conn = conn != NULL ? bt_conn_ref(conn) : NULL;
    }
    if (previous != NULL) {
        bt_conn_unref(previous);
    }
}

static struct bt_conn *zpt_gatt_get_conn(void) {
    struct bt_conn *conn = NULL;
    K_SPINLOCK(&zpt_gatt_lock) {
        if (zpt_gatt_conn != NULL) {
            conn = bt_conn_ref(zpt_gatt_conn);
        }
    }
    return conn;
}

static ssize_t zpt_gatt_write(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                              const void *buf, uint16_t len, uint16_t offset, uint8_t flags) {
    ARG_UNUSED(attr);
    ARG_UNUSED(flags);

    /* The attribute permission already requires an encrypted link; tuning
     * changes settings, so the peer must also be bonded. */
    if (!zpt_gatt_bonded(conn)) {
        return BT_GATT_ERR(BT_ATT_ERR_AUTHENTICATION);
    }
    /* Long (prepared) writes aren't supported: hosts split requests into
     * writes of at most 20 bytes, which fit every MTU. */
    if (offset != 0) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
    }
    if (ring_buf_space_get(&zpt_gatt_rx_ring) < len) {
        LOG_WRN("Dropped %u Bluetooth request bytes", (unsigned int)len);
        return BT_GATT_ERR(BT_ATT_ERR_INSUFFICIENT_RESOURCES);
    }

    if (zpt_gatt_conn != conn) {
        /* A new peer, or the same one after a reconnect: a frame the
         * previous connection left half-written must not absorb these
         * bytes. Raised before they are queued. */
        atomic_set(&zpt_gatt_rx_reset, 1);
        zpt_gatt_set_conn(conn);
    }
    ring_buf_put(&zpt_gatt_rx_ring, buf, len);
    zpt_host_protocol_wake();
    return len;
}

/* A host subscribing starts a new tuner session, possibly on the same
 * connection as an earlier one that stopped mid-frame, so the parser drops
 * any partial frame before the session's first request. Called for every
 * CCC write, per connection. */
static ssize_t zpt_gatt_ccc_write(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                  uint16_t value) {
    ARG_UNUSED(conn);
    ARG_UNUSED(attr);
    if ((value & BT_GATT_CCC_NOTIFY) != 0) {
        atomic_set(&zpt_gatt_rx_reset, 1);
    }
    return sizeof(value);
}

/* The NOLINT covers the service record: BT_GATT_SERVICE_DEFINE places it in
 * an iterable linker section and has no static variant. */
// NOLINTNEXTLINE(misc-use-internal-linkage)
BT_GATT_SERVICE_DEFINE(
    zpt_gatt_service, BT_GATT_PRIMARY_SERVICE(&zpt_service_uuid),
    BT_GATT_CHARACTERISTIC(&zpt_request_uuid.uuid,
                           BT_GATT_CHRC_WRITE | BT_GATT_CHRC_WRITE_WITHOUT_RESP,
                           BT_GATT_PERM_WRITE_ENCRYPT, NULL, zpt_gatt_write, NULL),
    BT_GATT_CHARACTERISTIC(&zpt_response_uuid.uuid, BT_GATT_CHRC_NOTIFY, BT_GATT_PERM_NONE, NULL,
                           NULL, NULL),
    BT_GATT_CCC_MANAGED(((struct _bt_gatt_ccc[]){
                            BT_GATT_CCC_INITIALIZER(NULL, zpt_gatt_ccc_write, NULL)}),
                        BT_GATT_PERM_READ | BT_GATT_PERM_WRITE_ENCRYPT));

/* Attribute index of the response characteristic's value. */
#define ZPT_GATT_RESPONSE_ATTR (&zpt_gatt_service.attrs[4])

static void zpt_gatt_sent(struct bt_conn *conn, void *user_data) {
    ARG_UNUSED(conn);
    ARG_UNUSED(user_data);
    k_sem_give(&zpt_gatt_credits);
    /* A capture batch the link had no room for is waiting to retry. */
    zpt_host_protocol_wake();
}

/* Copies bytes [offset, offset + length) of header followed by payload. */
static void zpt_gatt_gather(uint8_t *chunk, size_t offset, size_t length, const uint8_t *header,
                            size_t header_length, const uint8_t *payload) {
    for (size_t i = 0; i < length; i++) {
        const size_t position = offset + i;
        chunk[i] = position < header_length ? header[position] : payload[position - header_length];
    }
}

static int zpt_gatt_notify_frame(struct bt_conn *conn, size_t chunk_size, const uint8_t *header,
                                 size_t header_length, const uint8_t *payload, size_t total) {
    uint8_t chunk[ZPT_GATT_CHUNK_MAX];
    for (size_t offset = 0; offset < total; offset += chunk_size) {
        /* Waits as long as the link stays up, so a slow link never cuts a
         * frame short. A disconnect resets the semaphore, which ends the
         * wait with an error. */
        if (k_sem_take(&zpt_gatt_credits, K_FOREVER) < 0) {
            return -ENOTCONN;
        }
        const size_t length = MIN(chunk_size, total - offset);
        zpt_gatt_gather(chunk, offset, length, header, header_length, payload);
        struct bt_gatt_notify_params params = {
            .attr = ZPT_GATT_RESPONSE_ATTR,
            .data = chunk,
            .len = (uint16_t)length,
            .func = zpt_gatt_sent,
        };
        const int ret = bt_gatt_notify_cb(conn, &params);
        if (ret < 0) {
            k_sem_give(&zpt_gatt_credits);
            return ret;
        }
    }
    return 0;
}

static int zpt_gatt_send(const uint8_t *header, size_t header_length, const uint8_t *payload,
                         size_t payload_length, bool droppable) {
    struct bt_conn *conn = zpt_gatt_get_conn();
    if (conn == NULL) {
        return -ENOTCONN;
    }

    int ret = -EPIPE;
    if (bt_gatt_is_subscribed(conn, ZPT_GATT_RESPONSE_ATTR, BT_GATT_CCC_NOTIFY)) {
        const size_t chunk_size =
            MIN((size_t)bt_gatt_get_mtu(conn) - ZPT_ATT_NOTIFY_OVERHEAD, ZPT_GATT_CHUNK_MAX);
        const size_t total = header_length + payload_length;
        /* A droppable frame starts only when the link can take it, or the
         * whole budget, at once. Only this worker takes credits, so the
         * count can only grow before the frame claims them. */
        const size_t needed = MIN(DIV_ROUND_UP(total, chunk_size), ZPT_GATT_CREDITS);
        if (droppable && k_sem_count_get(&zpt_gatt_credits) < needed) {
            ret = -ENOBUFS;
        } else {
            ret = zpt_gatt_notify_frame(conn, chunk_size, header, header_length, payload, total);
            if (ret < 0) {
                LOG_WRN("Bluetooth frame cut short (%d)", ret);
            }
        }
    }
    bt_conn_unref(conn);
    return ret;
}

const struct zpt_host_transport zpt_gatt_transport = {
    .name = "bluetooth",
    .rx = &zpt_gatt_rx_ring,
    .rx_reset = &zpt_gatt_rx_reset,
    .send = zpt_gatt_send,
};

static void zpt_gatt_disconnected(struct bt_conn *conn, uint8_t reason) {
    ARG_UNUSED(reason);
    if (zpt_gatt_conn != conn) {
        return;
    }
    zpt_gatt_set_conn(NULL);
    /* Wake a response waiting for credits and refill the budget: the stack
     * may not report completion for notifications it discards. Late
     * completions can't push the count past its limit. */
    k_sem_reset(&zpt_gatt_credits);
    for (size_t i = 0; i < ZPT_GATT_CREDITS; i++) {
        k_sem_give(&zpt_gatt_credits);
    }
}

BT_CONN_CB_DEFINE(zpt_gatt_conn_callbacks) = {
    .disconnected = zpt_gatt_disconnected,
};
