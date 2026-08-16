/* SPDX-License-Identifier: MIT */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <zmk/pointing_tools/core/signal.h>
#include <zmk/pointing_tools/policy/suppression.h>

enum zpt_motion_gate_phase {
    ZPT_MOTION_GATE_IDLE = 0,
    ZPT_MOTION_GATE_PENDING,
    ZPT_MOTION_GATE_ACTIVE,
    ZPT_MOTION_GATE_BYPASS,
};
