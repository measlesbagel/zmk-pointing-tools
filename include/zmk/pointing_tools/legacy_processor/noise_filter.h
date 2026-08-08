/* SPDX-License-Identifier: MIT */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <zmk/pointing_tools/stage/motion_gate/coherent_displacement.h>

/* Transitional count-domain adapter retained by the current Bridges firmware. */

enum zpt_noise_filter_phase {
    ZPT_NOISE_FILTER_IDLE = ZPT_MOTION_GATE_IDLE,
    ZPT_NOISE_FILTER_PENDING = ZPT_MOTION_GATE_PENDING,
    ZPT_NOISE_FILTER_ACTIVE = ZPT_MOTION_GATE_ACTIVE,
    ZPT_NOISE_FILTER_BYPASS = ZPT_MOTION_GATE_BYPASS,
};

struct zpt_noise_filter_settings {
    bool enabled;
    uint16_t activation_distance;
    uint16_t coherence_percent;
    uint16_t qualification_timeout_ms;
    uint16_t idle_timeout_ms;
    uint16_t suppress_after_keypress_ms;
};

struct zpt_noise_filter_state {
    struct zpt_coherent_displacement_state strategy;
};

struct zpt_noise_filter_result {
    int32_t x;
    int32_t y;
    enum zpt_noise_filter_phase phase;
    bool reset_for_idle;
    bool reset_for_timeout;
    bool suppressed;
    bool qualified;
    bool discarded;
};

void zpt_noise_filter_reset(struct zpt_noise_filter_state *state);

struct zpt_noise_filter_result
zpt_noise_filter_update(struct zpt_noise_filter_state *state,
                        const struct zpt_noise_filter_settings *settings, int32_t x, int32_t y,
                        uint32_t now_ms, bool suppress);
