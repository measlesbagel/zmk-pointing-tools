/* SPDX-License-Identifier: MIT */
#pragma once

#include <zephyr/device.h>

#include <zmk/pointing_tools/core/pipeline.h>

/*
 * Stage observer telemetry for composed pipelines.
 *
 * A measlesbagel,zpt-pipeline-telemetry device attaches an observer to every
 * stage of its configured pipelines, allocates a state-telemetry target id
 * per stage, and reports stage decisions as state samples. Stages remain
 * discoverable by stable id through the lookup API.
 */

__subsystem struct zpt_pipeline_telemetry_driver_api {
    int (*lookup_stage)(const struct device *dev, const char *stable_id,
                        const struct zpt_stage **stage);
};

int zpt_zmk_pipeline_telemetry_lookup(const struct device *dev, const char *stable_id,
                                      const struct zpt_stage **stage);
