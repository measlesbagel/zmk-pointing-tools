/* SPDX-License-Identifier: MIT */
#pragma once

#include <zephyr/device.h>

#include <zmk/pointing_tools/core/pipeline.h>

/* Common prefix for every devicetree-backed stage provider's device data. */
struct zpt_zmk_stage_provider_data {
    struct zpt_stage stage;
};

__subsystem struct zpt_stage_provider_driver_api {
    struct zpt_stage *(*get_stage)(const struct device *dev);
};

extern const struct zpt_stage_provider_driver_api zpt_stage_provider_api;

int zpt_zmk_stage_provider_init(const struct device *dev, const char *stable_id,
                                const struct zpt_stage_api *api, const void *config, void *state);
int zpt_zmk_stage_provider_get(const struct device *dev, struct zpt_stage **stage);
