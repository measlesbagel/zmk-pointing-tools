/* SPDX-License-Identifier: MIT */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <zmk/pointing_tools/core/pipeline.h>
#include <zmk/pointing_tools/policy/suppression.h>

/*
 * Scroll batcher stage over raw motion.
 *
 * Accumulates constrained motion and emits wheel steps on a report-interval
 * deadline, combining the legacy scroll transfer function (scale multiplier
 * and divisor), quantizer (fractional remainder carried between reports), and
 * batcher (single armed report flush) semantics. The remainder survives
 * suppression, mirroring the legacy processor.
 */

struct zpt_scroll_batcher_config {
    uint16_t scale_multiplier;
    uint16_t scale_divisor;
    uint16_t report_interval_ms;
    /* Optional external condition clearing pending motion and dropping the
     * frame, mirroring the legacy keypress guard. */
    const struct zpt_suppression_policy *suppression;
};

struct zpt_scroll_batcher_state {
    int64_t pending_x;
    int64_t pending_y;
    int64_t remainder_x;
    int64_t remainder_y;
    bool armed;
};

extern const struct zpt_stage_api zpt_scroll_batcher_stage_api;
