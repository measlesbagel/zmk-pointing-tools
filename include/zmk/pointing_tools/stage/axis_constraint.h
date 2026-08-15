/* SPDX-License-Identifier: MIT */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <zmk/pointing_tools/core/pipeline.h>
#include <zmk/pointing_tools/policy/suppression.h>

/*
 * Axis-constraint stage over normalized motion.
 *
 * Buffers undecided motion and applies the axis-intent annotation carried by
 * an upstream estimator: horizontal intent drops vertical motion, vertical
 * intent drops horizontal motion, free intent keeps both, and undecided
 * motion is buffered until an intent engages, an idle gap expires it, or a
 * flush folds it (as free motion or discarded, per configuration). Mirrors
 * the legacy scroll processor's unclassified-motion handling.
 */

struct zpt_axis_constraint_config {
    bool discard_unclassified;
    /* Gap after which buffered undecided motion is folded or discarded. */
    uint16_t idle_timeout_ms;
    /* Report cadence at which buffered undecided motion is folded or
     * discarded, mirroring the legacy scroll flush; 0 uses the idle timeout. */
    uint16_t fold_interval_ms;
    /* Optional external condition clearing buffered motion and dropping the
     * frame, mirroring the legacy keypress guard. */
    const struct zpt_suppression_policy *suppression;
};

struct zpt_axis_constraint_state {
    int64_t undecided_x;
    int64_t undecided_y;
    uint8_t previous_intent;
    uint32_t last_frame_ms;
    bool have_last_frame;
    bool have_undecided;
};

extern const struct zpt_stage_api zpt_axis_constraint_stage_api;
