/* SPDX-License-Identifier: MIT */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <zmk/pointing_tools/core/pipeline.h>

/*
 * Cursor report coalescer over pointer deltas.
 *
 * Sends at most one cursor report per report interval. Motion that arrives
 * after a quiet interval goes out at once, so coalescing adds no latency to
 * the start of a movement; motion that arrives sooner is summed and sent
 * when the interval ends. A sum beyond the signed 16-bit HID range is sent
 * over several reports instead of being dropped. Run it after the quantizer
 * with an interval matched to the host's report rate, so fast sensors don't
 * queue reports faster than Bluetooth or USB can carry them. An interval of
 * zero passes every frame through.
 */

struct zpt_cursor_coalescer_config {
    uint16_t report_interval_ms;
};

struct zpt_cursor_coalescer_state {
    int64_t pending_x;
    int64_t pending_y;
    /* Metadata of the newest frame folded into the pending sum. */
    struct zpt_signal_metadata metadata;
    uint32_t pending_frames;
    uint32_t last_report_ms;
    bool have_report;
    bool armed;
};

extern const struct zpt_stage_api zpt_cursor_coalescer_stage_api;
