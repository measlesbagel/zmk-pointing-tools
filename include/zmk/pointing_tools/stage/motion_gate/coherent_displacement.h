/* SPDX-License-Identifier: MIT */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <zmk/pointing_tools/core/pipeline.h>
#include <zmk/pointing_tools/stage/motion_gate.h>

/* Unit-independent strategy settings. The pipeline stage uses Q16 millimetres. */
struct zpt_coherent_displacement_settings {
    bool enabled;
    int64_t activation_distance;
    uint16_t coherence_percent;
    uint16_t qualification_timeout_ms;
    uint16_t idle_timeout_ms;
};

struct zpt_coherent_displacement_state {
    int64_t pending_x;
    int64_t pending_y;
    uint64_t squared_energy;
    uint32_t sample_count;
    uint32_t pending_started_ms;
    uint32_t last_frame_ms;
    bool have_last_frame;
    bool active;
    bool pending_clipped;
};

struct zpt_coherent_displacement_result {
    int64_t x;
    int64_t y;
    enum zpt_motion_gate_phase phase;
    bool reset_for_idle;
    bool reset_for_timeout;
    bool suppressed;
    bool qualified;
    bool discarded;
    bool clipped;
};

int zpt_coherent_displacement_validate(const struct zpt_coherent_displacement_settings *settings);
void zpt_coherent_displacement_reset(struct zpt_coherent_displacement_state *state);

struct zpt_coherent_displacement_result
zpt_coherent_displacement_update(struct zpt_coherent_displacement_state *state,
                                 const struct zpt_coherent_displacement_settings *settings,
                                 int64_t x, int64_t y, uint32_t now_ms, bool suppress);

/* Advance idle/qualification timers without introducing a synthetic vector. */
struct zpt_coherent_displacement_result
zpt_coherent_displacement_expire(struct zpt_coherent_displacement_state *state,
                                 const struct zpt_coherent_displacement_settings *settings,
                                 uint32_t now_ms);

struct zpt_coherent_displacement_stage_config {
    struct zpt_coherent_displacement_settings settings;
    const struct zpt_suppression_policy *suppression;
};

struct zpt_coherent_displacement_stage_state {
    struct zpt_coherent_displacement_state strategy;
    struct zpt_signal pending_evidence;
    bool have_pending_evidence;
};

extern const struct zpt_stage_api zpt_coherent_displacement_stage_api;
/* Raw-count variant carrying count-domain activation distances. */
extern const struct zpt_stage_api zpt_coherent_displacement_raw_stage_api;
