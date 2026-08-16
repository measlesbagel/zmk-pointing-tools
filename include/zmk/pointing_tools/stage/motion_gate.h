/* SPDX-License-Identifier: MIT */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <zmk/pointing_tools/core/signal.h>

enum zpt_motion_gate_phase {
    ZPT_MOTION_GATE_IDLE = 0,
    ZPT_MOTION_GATE_PENDING,
    ZPT_MOTION_GATE_ACTIVE,
    ZPT_MOTION_GATE_BYPASS,
};

typedef bool (*zpt_motion_gate_suppressed_t)(void *context, const struct zpt_signal *signal,
                                             uint32_t now_ms);

/* Optional external condition participating in the gate's pass/drop decision. */
struct zpt_motion_gate_suppression_policy {
    zpt_motion_gate_suppressed_t is_suppressed;
    void *context;
};
