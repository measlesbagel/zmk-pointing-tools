/* SPDX-License-Identifier: MIT */
#define DT_DRV_COMPAT measlesbagel_zpt_pointing_device

#include <errno.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/init.h>
#include <zephyr/logging/log.h>
#include <zmk/pointing_tools/platform/zmk/device_table.h>
#include <zmk/pointing_tools/source/sensor_control.h>

LOG_MODULE_REGISTER(zpt_paw32xx_control, CONFIG_ZMK_LOG_LEVEL);

/* Native sensor-control adapter for the PAW3222 driver module
 * (black-trooper/zmk-driver-paw3222, DT compatible pixart,paw3222). Binds
 * every device-table entry whose backing sensor is a PAW3222 to the
 * driver's register-level resolution setter.
 *
 * The driver exposes set only - its CPI registers are write-only through
 * this API - so reads stay on the control layer's stored value, and the
 * supported-values truth remains the devicetree capability declaration.
 * The prototype mirrors the driver header; the module does not install it
 * on the global include path. */
int paw32xx_set_resolution(const struct device *dev, uint16_t res_cpi);

static int zpt_paw32xx_set_cpi(const struct device *dev, uint16_t cpi) {
    return paw32xx_set_resolution(dev, cpi);
}

static const struct zpt_sensor_control_api zpt_paw32xx_api = {
    .set_cpi = zpt_paw32xx_set_cpi,
};

#define ZPT_PAW32XX_BIND(inst)                                                                     \
    IF_ENABLED(                                                                                    \
        DT_NODE_HAS_COMPAT(DT_INST_PHANDLE(inst, sensor), pixart_paw3222), ({                      \
            const struct zpt_pointing_device *device = zpt_device_table_at(inst);                  \
            if (device != NULL) {                                                                  \
                const int ret = zpt_sensor_control_register(device->sensor, &zpt_paw32xx_api);     \
                if (ret < 0) {                                                                     \
                    LOG_ERR("Failed to bind PAW32XX facet for %s: %d", device->stable_id, ret);    \
                } else {                                                                           \
                    LOG_INF("PAW32XX native control bound for %s", device->stable_id);             \
                }                                                                                  \
            }                                                                                      \
        }))

static int zpt_paw32xx_adapter_init(void) {
    DT_INST_FOREACH_STATUS_OKAY(ZPT_PAW32XX_BIND)
    return 0;
}

SYS_INIT(zpt_paw32xx_adapter_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
