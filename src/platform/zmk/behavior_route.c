/* SPDX-License-Identifier: MIT */
#define DT_DRV_COMPAT measlesbagel_zpt_behavior_route

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>

#include <drivers/behavior.h>
#include <zmk/behavior.h>

#include <zmk/pointing_tools/platform/zmk/router.h>

struct zpt_route_behavior_config {
    const struct device *router_device;
    const struct device *pipeline_device;
};

static const struct zpt_route_behavior_config *
route_behavior_config(const struct zmk_behavior_binding *binding) {
    const struct device *behavior = zmk_behavior_get_binding(binding->behavior_dev);
    return behavior == NULL ? NULL : behavior->config;
}

static int route_behavior_pressed(struct zmk_behavior_binding *binding,
                                  struct zmk_behavior_binding_event event) {
    const struct zpt_route_behavior_config *config = route_behavior_config(binding);
    if (config == NULL) {
        return -ENODEV;
    }
    return zpt_zmk_router_override_press(config->router_device, config->pipeline_device,
                                         event.position, k_uptime_get_32());
}

static int route_behavior_released(struct zmk_behavior_binding *binding,
                                   struct zmk_behavior_binding_event event) {
    const struct zpt_route_behavior_config *config = route_behavior_config(binding);
    if (config == NULL) {
        return -ENODEV;
    }
    return zpt_zmk_router_override_release(config->router_device, event.position,
                                           k_uptime_get_32());
}

static const struct behavior_driver_api route_behavior_api = {
    .binding_pressed = route_behavior_pressed,
    .binding_released = route_behavior_released,
};

#define ZPT_ROUTE_BEHAVIOR_DEFINE(inst)                                                            \
    static const struct zpt_route_behavior_config zpt_route_behavior_config_##inst = {             \
        .router_device = DEVICE_DT_GET(DT_INST_PHANDLE(inst, router)),                             \
        .pipeline_device = DEVICE_DT_GET(DT_INST_PHANDLE(inst, pipeline)),                         \
    };                                                                                             \
    BEHAVIOR_DT_INST_DEFINE(inst, NULL, NULL, NULL, &zpt_route_behavior_config_##inst,             \
                            POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,                      \
                            &route_behavior_api);

DT_INST_FOREACH_STATUS_OKAY(ZPT_ROUTE_BEHAVIOR_DEFINE)
