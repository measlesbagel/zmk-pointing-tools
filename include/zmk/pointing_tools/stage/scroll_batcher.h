/* SPDX-License-Identifier: MIT */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <zmk/pointing_tools/core/pipeline.h>
#include <zmk/pointing_tools/policy/suppression.h>

/*
 * Scroll batcher stage over normalized motion.
 *
 * Accumulates constrained Q16 millimetre motion and emits wheel steps on a
 * report-interval deadline: the fixed-point steps-per-millimetre factor is
 * the transfer function, the fractional remainder carries between reports,
 * and a single armed flush provides the batching. The remainder survives
 * suppression.
 */

struct zpt_scroll_batcher_config {
    /* Q16 wheel steps per millimetre of constrained motion. */
    zpt_fixed_t steps_per_millimeter;
    uint16_t report_interval_ms;
    /* Optional external condition clearing pending motion and dropping the
     * frame, mirroring the physical-keypress guard. */
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
