/* SPDX-License-Identifier: MIT */

#include <errno.h>
#include <stddef.h>

#include <zmk/pointing_tools/source/motion_source.h>

#define ZPT_SOURCE_LOCATION_FLAGS (ZPT_SIGNAL_FLAG_LOCAL | ZPT_SIGNAL_FLAG_TRANSPORTED)

int zpt_motion_source_init(struct zpt_motion_source_state *state,
                           const struct zpt_motion_source_config *config) {
    if (state == NULL || config == NULL || config->resolution_cpi == 0U) {
        return -EINVAL;
    }

    uint32_t location = config->flags & ZPT_SOURCE_LOCATION_FLAGS;
    if (location != ZPT_SIGNAL_FLAG_LOCAL && location != ZPT_SIGNAL_FLAG_TRANSPORTED) {
        return -EINVAL;
    }

    *state = (struct zpt_motion_source_state){
        .flags = config->flags,
        .source_id = config->source_id,
        .resolution_cpi = config->resolution_cpi,
    };
    return 0;
}

int zpt_motion_source_set_resolution(struct zpt_motion_source_state *state,
                                     uint16_t resolution_cpi) {
    if (state == NULL || resolution_cpi == 0U) {
        return -EINVAL;
    }
    if (state->frame.saw_axis) {
        return -EBUSY;
    }
    state->resolution_cpi = resolution_cpi;
    return 0;
}

uint16_t zpt_motion_source_get_resolution(const struct zpt_motion_source_state *state) {
    return state == NULL ? 0U : state->resolution_cpi;
}

void zpt_motion_source_add(struct zpt_motion_source_state *state, enum zpt_motion_axis axis,
                           int64_t counts) {
    if (state != NULL) {
        zpt_frame_assembler_add(&state->frame, axis, counts);
    }
}

bool zpt_motion_source_take(struct zpt_motion_source_state *state, uint32_t observed_at_ms,
                            uint32_t sample_span_us, uint32_t additional_flags,
                            struct zpt_signal *signal) {
    if (state == NULL || signal == NULL) {
        return false;
    }

    struct zpt_raw_motion motion;
    uint32_t frame_flags;
    if (!zpt_frame_assembler_take(&state->frame, &motion, &frame_flags)) {
        return false;
    }

    *signal = (struct zpt_signal){
        .kind = ZPT_SIGNAL_RAW_MOTION,
        .metadata =
            {
                .observed_at_ms = observed_at_ms,
                .sample_span_us = sample_span_us,
                .flags =
                    state->flags | (additional_flags & ~ZPT_SOURCE_LOCATION_FLAGS) | frame_flags,
                .source_id = state->source_id,
                .sequence = state->sequence++,
                .resolution_cpi = state->resolution_cpi,
            },
        .annotations =
            {
                .axis_intent = ZPT_SIGNAL_AXIS_UNDECIDED,
            },
        .data.raw_motion = motion,
    };
    return true;
}
