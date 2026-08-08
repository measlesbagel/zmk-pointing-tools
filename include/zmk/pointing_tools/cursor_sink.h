/* SPDX-License-Identifier: MIT */
#pragma once

#include <zephyr/device.h>

#include <zmk/pointing_tools/pipeline.h>

struct zpt_cursor_sink_config {
    const struct device *output_device;
};

extern const struct zpt_sink_api zpt_cursor_sink_api;
