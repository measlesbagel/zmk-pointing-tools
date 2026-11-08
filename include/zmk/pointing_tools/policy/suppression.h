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
};
