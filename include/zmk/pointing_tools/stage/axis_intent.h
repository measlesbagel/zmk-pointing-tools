/* SPDX-License-Identifier: MIT */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <zmk/pointing_tools/core/pipeline.h>
#include <zmk/pointing_tools/policy/suppression.h>

/*
 * Reusable semantic-neutral axis-intent estimator.
 *
 * Distinguishes free, horizontal, and vertical intent from windowed energy
 * accumulation with engage/release hysteresis. The strategy is unit-
 * independent: inputs and the activation distance share the caller's units
 * (sensor counts for legacy adapters, Q16 millimetres for normalized motion
 * stages). The stage wrapper consumes NORMALIZED_MOTION and carries the
 * result as signal annotations.
 */

enum zpt_axis_policy {
    ZPT_AXIS_POLICY_FREE = 0,
    ZPT_AXIS_POLICY_ADAPTIVE = 1,
    ZPT_AXIS_POLICY_HORIZONTAL = 2,
    ZPT_AXIS_POLICY_VERTICAL = 3,
};

enum zpt_axis_intent {
    ZPT_AXIS_INTENT_UNDECIDED = 0,
    ZPT_AXIS_INTENT_FREE,
    ZPT_AXIS_INTENT_HORIZONTAL,
    ZPT_AXIS_INTENT_VERTICAL,
};

/* The estimator values are stored directly in signal annotations; keep the
 * two enums interchangeable at compile time. */
_Static_assert((int)ZPT_SIGNAL_AXIS_UNDECIDED == (int)ZPT_AXIS_INTENT_UNDECIDED,
               "annotation and estimator intents must agree");
_Static_assert((int)ZPT_SIGNAL_AXIS_FREE == (int)ZPT_AXIS_INTENT_FREE,
               "annotation and estimator intents must agree");
_Static_assert((int)ZPT_SIGNAL_AXIS_HORIZONTAL == (int)ZPT_AXIS_INTENT_HORIZONTAL,
               "annotation and estimator intents must agree");
_Static_assert((int)ZPT_SIGNAL_AXIS_VERTICAL == (int)ZPT_AXIS_INTENT_VERTICAL,
               "annotation and estimator intents must agree");

struct zpt_axis_intent_settings {
    uint16_t engage_ratio_percent;
    uint16_t release_ratio_percent;
    int64_t activation_distance;
    uint16_t window_ms;
};

struct zpt_axis_intent_state {
    uint64_t horizontal_energy;
    uint64_t vertical_energy;
    enum zpt_axis_intent intent;
};

int zpt_axis_intent_validate(const struct zpt_axis_intent_settings *settings);
void zpt_axis_intent_reset(struct zpt_axis_intent_state *state);

enum zpt_axis_intent zpt_axis_intent_estimate(struct zpt_axis_intent_state *state,
                                              const struct zpt_axis_intent_settings *settings,
                                              enum zpt_axis_policy policy, int64_t x, int64_t y,
                                              uint32_t elapsed_ms);

/* Dominance of the engaged axis from 0 through 100, or 0 while undecided. */
uint16_t zpt_axis_intent_confidence(const struct zpt_axis_intent_state *state);

struct zpt_axis_intent_stage_config {
    struct zpt_axis_intent_settings settings;
    enum zpt_axis_policy policy;
    /* Gap after which the estimator resets; 0 disables the idle reset. */
    uint16_t idle_timeout_ms;
    /* Optional external condition resetting the estimator and dropping the
     * frame, mirroring the legacy keypress guard. */
    const struct zpt_suppression_policy *suppression;
};

struct zpt_axis_intent_stage_state {
    struct zpt_axis_intent_state estimator;
    uint32_t last_frame_ms;
    bool have_last_frame;
    uint8_t last_notified_intent;
};

/* Normalized-motion variant carrying Q16 millimetre activation distances. */
extern const struct zpt_stage_api zpt_axis_intent_stage_api;
/* Raw-count variant carrying count-domain activation distances. */
extern const struct zpt_stage_api zpt_axis_intent_raw_stage_api;
