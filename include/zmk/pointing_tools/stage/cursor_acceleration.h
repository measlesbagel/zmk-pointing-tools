/* SPDX-License-Identifier: MIT */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <zmk/pointing_tools/core/pipeline.h>

/*
 * Cursor acceleration stage over normalized motion.
 *
 * Scales each frame by a gain that depends on ball speed, so slow motion
 * stays precise and fast motion covers distance. The gain is the slow gain
 * at or below the slow speed, the fast gain at or above the fast speed, and
 * linear in between; equal gains leave motion untouched.
 *
 * Speed is the Euclidean distance travelled within a sliding window that
 * decays linearly, divided by the window. Irregular frame timing and
 * millisecond timestamps therefore don't make the speed jitter, a frame
 * after a pause starts slow, and one gain scales both axes, so diagonal
 * motion keeps its direction and accelerates exactly like cardinal motion.
 * Place the stage before cursor-transfer so speeds are ball speeds.
 */

struct zpt_cursor_acceleration_config {
    /* Ball speeds bounding the gain ramp, Q16 millimetres per second. */
    zpt_fixed_t slow_speed;
    zpt_fixed_t fast_speed;
    uint16_t slow_gain_percent;
    uint16_t fast_gain_percent;
    uint16_t speed_window_ms;
};

struct zpt_cursor_acceleration_state {
    /* Distance within the window, Q16 millimetres. */
    zpt_fixed_t window_distance;
    uint32_t last_frame_ms;
    bool have_last_frame;
};

int zpt_cursor_acceleration_validate(const struct zpt_cursor_acceleration_config *config);

/* Gain for a speed in Q16 millimetres per second, as Q16 (ZPT_FIXED_ONE is
 * 100 percent). */
zpt_fixed_t zpt_cursor_acceleration_gain(const struct zpt_cursor_acceleration_config *config,
                                         zpt_fixed_t speed);

/* Add a frame of displacement at now_ms and return the speed estimate in Q16
 * millimetres per second. */
zpt_fixed_t zpt_cursor_acceleration_observe(struct zpt_cursor_acceleration_state *state,
                                            const struct zpt_cursor_acceleration_config *config,
                                            zpt_fixed_t x, zpt_fixed_t y, uint32_t now_ms);

extern const struct zpt_stage_api zpt_cursor_acceleration_stage_api;
