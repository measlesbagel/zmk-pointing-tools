/* SPDX-License-Identifier: MIT */
#pragma once

#include <zephyr/device.h>

#include <zmk/pointing_tools/core/pipeline.h>

__subsystem struct zpt_pipeline_provider_driver_api {
    int (*prepare)(const struct device *dev, const struct device *output_device,
                   struct zpt_pipeline **pipeline);
};

int zpt_zmk_pipeline_provider_prepare(const struct device *dev, const struct device *output_device,
                                      struct zpt_pipeline **pipeline);
