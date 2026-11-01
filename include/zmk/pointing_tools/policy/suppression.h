/* SPDX-License-Identifier: MIT */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <zmk/pointing_tools/core/signal.h>

/*
 * Shared external suppression policy.
 *
 * A policy participates in a stage's pass, buffer, or drop decision without
 * making the algorithm depend on ZMK key events. Host replay can provide
 * deterministic synthetic context, while a ZMK adapter can provide physical
 * keypress context to every pipeline that references it.
 */

typedef bool (*zpt_suppressed_t)(void *context, const struct zpt_signal *signal, uint32_t now_ms);

struct zpt_suppression_policy {
    zpt_suppressed_t is_suppressed;
    void *context;
    /* How long a keypress suppresses motion, for input capture to describe
     * the policy; zero when it isn't a keypress window. */
    uint32_t keypress_window_ms;
    /* Optional, for input capture. freeze_keypress reports the latest key
     * press (false when there was none) and makes is_suppressed decide with
     * exactly that one until thaw_keypress, so a key press landing while a
     * frame runs counts from the next frame on. Other pipelines sharing the
     * policy see it a frame later as well. */
    bool (*freeze_keypress)(void *context, uint32_t *time_ms);
    void (*thaw_keypress)(void *context);
};
