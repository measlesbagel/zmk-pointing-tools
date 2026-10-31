/* SPDX-License-Identifier: MIT */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/sys/ring_buffer.h>

/* A byte link that carries the host protocol (docs/protocol.md). The
 * telemetry service owns framing, parsing, and request handling; a transport
 * only queues received bytes and sends whole frames. Each transport keeps its
 * own parser state, so several links can be open at once. */
struct zpt_host_transport {
    const char *name;
    /* Received bytes waiting for the protocol worker. The transport writes,
     * the worker reads. */
    struct ring_buf *rx;
    /* Sends one frame, the header and then the payload. Returns 0 once the
     * whole frame is handed to the link, or a negative errno. A droppable
     * frame (an unsolicited state sample) is refused rather than delayed
     * when the link has no room for it. A frame is never cut short
     * while the link stays up: host decoders can't resynchronise after a
     * truncated frame. */
    int (*send)(const uint8_t *header, size_t header_length, const uint8_t *payload,
                size_t payload_length, bool droppable);
};

/* Wakes the protocol worker after a transport queued received bytes. Safe
 * from interrupt context. */
void zpt_host_protocol_wake(void);

/* USB CDC-ACM serial (src/service/transport/uart.c). */
extern const struct zpt_host_transport zpt_uart_transport;
/* Bluetooth GATT, with CONFIG_ZMK_POINTING_TOOLS_TELEMETRY_BLE
 * (src/service/transport/gatt.c). */
extern const struct zpt_host_transport zpt_gatt_transport;
