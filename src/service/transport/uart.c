/* SPDX-License-Identifier: MIT */

/* USB CDC-ACM serial transport for the host protocol: the chosen
 * measlesbagel,zpt-telemetry-uart port. */

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/init.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/ring_buffer.h>
#include <zephyr/sys/util.h>

#include <zmk/pointing_tools/service/host_transport.h>

LOG_MODULE_DECLARE(zpt_telemetry);

#if !DT_HAS_CHOSEN(measlesbagel_zpt_telemetry_uart)
#error "measlesbagel,zpt-telemetry-uart chosen node is required"
#endif

#define ZPT_UART_NODE DT_CHOSEN(measlesbagel_zpt_telemetry_uart)
static const struct device *const zpt_uart = DEVICE_DT_GET(ZPT_UART_NODE);

/* The ring buffer handle is documented as `extern struct ring_buf
 * <name>` (the storage is already static); the one-line macro has no
 * static variant. */
RING_BUF_DECLARE(zpt_rx_ring, 128); // NOLINT(misc-use-internal-linkage)

/* uart_poll_out() blocks until the byte is queued, so frames are never
 * refused or cut short. */
static int zpt_uart_send(const uint8_t *header, size_t header_length, const uint8_t *payload,
                         size_t payload_length, bool droppable) {
    ARG_UNUSED(droppable);
    for (size_t i = 0; i < header_length; i++) {
        uart_poll_out(zpt_uart, header[i]);
    }
    for (size_t i = 0; i < payload_length; i++) {
        uart_poll_out(zpt_uart, payload[i]);
    }
    return 0;
}

const struct zpt_host_transport zpt_uart_transport = {
    .name = "uart",
    .rx = &zpt_rx_ring,
    .send = zpt_uart_send,
};

static void zpt_uart_callback(const struct device *dev, void *user_data) {
    ARG_UNUSED(user_data);

    /* uart_irq_update() re-runs the driver's pending-interrupt state
     * machine and reports whether work is pending. Drivers without that
     * operation (e.g. the vnd,serial unit-test mock) return -ENOSYS; for
     * those, the ready checks below are sufficient. */
    const int updated = uart_irq_update(dev);
    if (updated == 0 || (updated < 0 && !uart_irq_rx_ready(dev))) {
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
    zpt_host_protocol_wake();
}

static int zpt_uart_transport_init(void) {
    if (!device_is_ready(zpt_uart)) {
        return -ENODEV;
    }

    const int error = uart_irq_callback_user_data_set(zpt_uart, zpt_uart_callback, NULL);
    if (error < 0) {
        return error;
    }
    uart_irq_rx_enable(zpt_uart);
    LOG_INF("Pointing telemetry ready on USB serial");
    return 0;
}

SYS_INIT(zpt_uart_transport_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
