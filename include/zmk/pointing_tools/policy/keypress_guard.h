/* SPDX-License-Identifier: MIT */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Settings of a keypress guard: pointer motion is held back for a window
 * after any physical key press. Runtime tuning swaps the whole struct. */
struct zpt_keypress_guard_config {
    uint32_t suppress_after_keypress_ms;
};

/* Whether a guard with config suppresses motion at now_ms, given the time
 * of the last key press. Unsigned subtraction keeps it right across the
 * millisecond counter wrapping. */
static inline bool zpt_keypress_guard_active(const struct zpt_keypress_guard_config *config,
                                             bool have_keypress, uint32_t last_keypress_ms,
                                             uint32_t now_ms) {
    return have_keypress && now_ms - last_keypress_ms < config->suppress_after_keypress_ms;
}
