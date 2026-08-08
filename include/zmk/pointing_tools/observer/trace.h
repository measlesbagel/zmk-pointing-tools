/* SPDX-License-Identifier: MIT */
#pragma once

#include <stddef.h>
#include <stdint.h>

/* Pass-through observations captured at pointing pipeline boundaries. */

struct zpt_trace_descriptor {
    uint8_t pointing_device_id;
    uint8_t stage;
    const char *label;
};

struct zpt_trace_sample {
    uint32_t timestamp_ms;
    uint32_t sequence;
    uint8_t pointing_device_id;
    uint8_t stage;
    int32_t x;
    int32_t y;
    int32_t wheel;
    int32_t h_wheel;
};

size_t zpt_trace_stream_count(void);
const struct zpt_trace_descriptor *zpt_trace_stream_descriptor(size_t index);

void zpt_telemetry_submit(const struct zpt_trace_sample *sample);
