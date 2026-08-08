/* SPDX-License-Identifier: MIT */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <zmk/pointing_tools/core/signal.h>
#include <zmk/pointing_tools/source/frame_assembler.h>

struct zpt_motion_source_config {
    /* Must contain exactly one of LOCAL or TRANSPORTED. */
    uint32_t flags;
    uint16_t source_id;
    uint16_t resolution_cpi;
};

struct zpt_motion_source_state {
    struct zpt_frame_assembler frame;
    uint32_t flags;
    uint16_t source_id;
    uint16_t sequence;
    uint16_t resolution_cpi;
};

/* Source state is allocation-free and externally synchronized by its adapter. */
int zpt_motion_source_init(struct zpt_motion_source_state *state,
                           const struct zpt_motion_source_config *config);

/* Update CPI between frames; returns -EBUSY rather than relabel pending motion. */
int zpt_motion_source_set_resolution(struct zpt_motion_source_state *state,
                                     uint16_t resolution_cpi);
uint16_t zpt_motion_source_get_resolution(const struct zpt_motion_source_state *state);

/* Accumulate scalar axis events until the adapter observes a frame sync. */
void zpt_motion_source_add(struct zpt_motion_source_state *state, enum zpt_motion_axis axis,
                           int64_t counts);

/* Finish a non-empty frame and assign its source-local sequence number. */
bool zpt_motion_source_take(struct zpt_motion_source_state *state, uint32_t observed_at_ms,
                            uint32_t sample_span_us, uint32_t additional_flags,
                            struct zpt_signal *signal);
