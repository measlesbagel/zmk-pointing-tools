/* SPDX-License-Identifier: MIT */
#pragma once

#include <stdint.h>

#include <zephyr/device.h>

#include <zmk/pointing_tools/core/pipeline.h>
#include <zmk/pointing_tools/core/signal.h>

__subsystem struct zpt_router_driver_api {
    int (*push)(const struct device *dev, const struct zpt_signal *signal,
                struct zpt_pipeline_result *result);
    int (*refresh_layer_route)(const struct device *dev, uint32_t now_ms);
    int (*override_press)(const struct device *dev, const struct device *pipeline_device,
                          uint32_t position, uint32_t now_ms);
    int (*override_release)(const struct device *dev, uint32_t position, uint32_t now_ms);
};

int zpt_zmk_router_push(const struct device *dev, const struct zpt_signal *signal,
                        struct zpt_pipeline_result *result);
int zpt_zmk_router_refresh_layer_route(const struct device *dev, uint32_t now_ms);
int zpt_zmk_router_override_press(const struct device *dev, const struct device *pipeline_device,
                                  uint32_t position, uint32_t now_ms);
int zpt_zmk_router_override_release(const struct device *dev, uint32_t position, uint32_t now_ms);

/* Hold every ready router between frames so a change spanning several
 * fields reaches stages whole, whichever router runs them. Routers are
 * taken in devicetree order; pair with zpt_zmk_router_unlock_all(). */
int zpt_zmk_router_lock_all(void);
int zpt_zmk_router_unlock_all(void);
