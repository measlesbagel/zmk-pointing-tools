/* SPDX-License-Identifier: MIT */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <zmk/pointing_tools/core/pipeline.h>

/*
 * Cursor acceleration stage over normalized motion.
 *
 * Scales each frame by a sensitivity that depends on ball speed, read from a
 * curve through eight control points. Each point pairs a ball speed with a
 * sensitivity; between points the curve is a monotone cubic (Fritsch-Carlson
 * PCHIP), so it never overshoots a point and never bends sharply at one, and
 * it stays flat before the first point and after the last. A curve with
 * every point at 100 percent leaves motion untouched.
 *
 * Speed comes from each frame's own interval: the sensor's sample span when
 * the frame carries one, otherwise the time since the previous frame. The
 * per-frame speed is smoothed with separate rise and fall time constants, so
 * a flick reaches its speed within a few frames and the cursor slows as soon
 * as the ball does. After a gap of ZPT_CURSOR_ACCELERATION_RESTART_MS or more
 * the estimate restarts from the new frame, judged over that whole gap, so
 * the first frame after a pause starts slow. One sensitivity scales both
 * axes, so diagonal motion keeps its direction and accelerates exactly like
 * cardinal motion. Place the stage before cursor-transfer so speeds are ball
 * speeds.
 */

#define ZPT_CURSOR_ACCELERATION_POINTS 8
#define ZPT_CURSOR_ACCELERATION_MAX_PERCENT 1000U
#define ZPT_CURSOR_ACCELERATION_RESTART_MS 40U

struct zpt_cursor_acceleration_config {
    /* Control point ball speeds in millimetres per second, strictly
     * increasing. */
    uint16_t curve_speed_mm_per_s[ZPT_CURSOR_ACCELERATION_POINTS];
    /* Sensitivity at each control point, in percent of the cursor-transfer
     * gain, from 1 to ZPT_CURSOR_ACCELERATION_MAX_PERCENT. */
    uint16_t curve_percent[ZPT_CURSOR_ACCELERATION_POINTS];
    /* Time constants with which the speed estimate follows the ball
     * speeding up and slowing down. */
    uint16_t speed_rise_ms;
    uint16_t speed_fall_ms;
};

struct zpt_cursor_acceleration_state {
    /* Smoothed ball speed, Q16 millimetres per second. */
    zpt_fixed_t speed;
    uint32_t last_frame_ms;
    bool have_last_frame;
};

int zpt_cursor_acceleration_validate(const struct zpt_cursor_acceleration_config *config);

/* Sensitivity for a speed in Q16 millimetres per second, as Q16
 * (ZPT_FIXED_ONE is 100 percent). config must be valid. */
zpt_fixed_t zpt_cursor_acceleration_sensitivity(const struct zpt_cursor_acceleration_config *config,
                                                zpt_fixed_t speed);

/* Fold a frame's displacement into the speed estimate and return the new
 * estimate in Q16 millimetres per second. frame supplies the arrival time,
 * sample span, and span-clipped flag. */
zpt_fixed_t zpt_cursor_acceleration_observe(struct zpt_cursor_acceleration_state *state,
                                            const struct zpt_cursor_acceleration_config *config,
                                            zpt_fixed_t x, zpt_fixed_t y,
                                            const struct zpt_signal_metadata *frame);

extern const struct zpt_stage_api zpt_cursor_acceleration_stage_api;
