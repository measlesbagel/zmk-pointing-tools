/* SPDX-License-Identifier: MIT */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Model retained until motion gating is migrated to a composable stage. */

enum zpt_noise_filter_phase {
    ZPT_NOISE_FILTER_IDLE = 0,
    ZPT_NOISE_FILTER_PENDING = 1,
    ZPT_NOISE_FILTER_ACTIVE = 2,
    ZPT_NOISE_FILTER_BYPASS = 3,
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
    int32_t pending_x;
    int32_t pending_y;
    uint64_t squared_energy;
    uint32_t sample_count;
    uint32_t pending_started_ms;
    uint32_t last_frame_ms;
    bool have_last_frame;
    bool active;
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
