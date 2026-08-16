/* SPDX-License-Identifier: MIT */
#pragma once

#include <zephyr/device.h>

#include <zmk/pointing_tools/core/pipeline.h>

/* Common prefix for every devicetree-backed sink provider's device data. */
struct zpt_zmk_sink_provider_data {
    struct zpt_sink sink;
};

__subsystem struct zpt_sink_provider_driver_api {
    struct zpt_sink *(*get_sink)(const struct device *dev);
    int (*bind_output)(const struct device *dev, const struct device *output_device);
};

int zpt_zmk_sink_provider_init(const struct device *dev, const char *stable_id,
                               const struct zpt_sink_api *api, const void *config, void *state);
int zpt_zmk_sink_provider_get(const struct device *dev, struct zpt_sink **sink);
int zpt_zmk_sink_provider_bind_output(const struct device *dev, const struct device *output_device);
