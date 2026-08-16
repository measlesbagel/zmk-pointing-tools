/* SPDX-License-Identifier: MIT */
#pragma once

#include <zephyr/device.h>

#include <zmk/pointing_tools/core/pipeline.h>

__subsystem struct zpt_pipeline_provider_driver_api {
    int (*prepare)(const struct device *dev, const struct device *output_device,
                   struct zpt_pipeline **pipeline);
    int (*get)(const struct device *dev, struct zpt_pipeline **pipeline);
};

int zpt_zmk_pipeline_provider_prepare(const struct device *dev, const struct device *output_device,
                                      struct zpt_pipeline **pipeline);

/* Return the pipeline without binding an output device; works before and
 * after prepare, so observers can attach without owning the output. */
int zpt_zmk_pipeline_provider_get(const struct device *dev, struct zpt_pipeline **pipeline);
