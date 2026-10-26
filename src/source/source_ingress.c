/* SPDX-License-Identifier: MIT */

#include <errno.h>
#include <stddef.h>

#include <zmk/pointing_tools/source/source_ingress.h>

int zpt_source_ingress_init(struct zpt_source_ingress *ingress,
                            const struct zpt_source_ingress_config *config) {
    if (ingress == NULL || config == NULL) {
        return -EINVAL;
    }
    int ret = zpt_motion_source_init(&ingress->source, &config->source);
    if (ret < 0) {
        return ret;
    }
    zpt_compact_split_decoder_init(&ingress->compact_decoder);
    return 0;
}

static bool take_compact_frame(struct zpt_source_ingress *ingress,
                               const struct zpt_source_ingress_config *config, int32_t value,
                               uint32_t now_ms, struct zpt_signal *signal, int *decode_error) {
    struct zpt_compact_split_frame frame;
    int ret = zpt_compact_split_decode(&ingress->compact_decoder, (uint32_t)value, &frame);
    if (ret < 0) {
        *decode_error = ret;
        return false;
    }

    zpt_motion_source_add(&ingress->source, ZPT_MOTION_AXIS_X, frame.motion.x_counts);
    zpt_motion_source_add(&ingress->source, ZPT_MOTION_AXIS_Y, frame.motion.y_counts);
    uint32_t flags = frame.flags;
    if (config->compact_transport_coalesced) {
        flags |= ZPT_SIGNAL_FLAG_COALESCED;
    }
    return zpt_motion_source_take_at_sequence(&ingress->source, now_ms, frame.sample_span_us,
                                              frame.sequence, flags, signal);
}

bool zpt_source_ingress_handle(struct zpt_source_ingress *ingress,
                               const struct zpt_source_ingress_config *config,
                               enum zpt_source_event_kind kind, int32_t value, bool sync,
                               uint32_t now_ms, struct zpt_signal *signal, int *decode_error) {
    if (decode_error != NULL) {
        *decode_error = 0;
    }
    if (ingress == NULL || config == NULL || signal == NULL || decode_error == NULL) {
        return false;
    }

    switch (kind) {
    case ZPT_SOURCE_EVENT_COMPACT:
        /* The packet carries a complete frame; its sync flag marks that same
         * boundary and must not take a second, empty frame. */
        return take_compact_frame(ingress, config, value, now_ms, signal, decode_error);
    case ZPT_SOURCE_EVENT_AXIS_X:
        zpt_motion_source_add(&ingress->source, ZPT_MOTION_AXIS_X, value);
        break;
    case ZPT_SOURCE_EVENT_AXIS_Y:
        zpt_motion_source_add(&ingress->source, ZPT_MOTION_AXIS_Y, value);
        break;
    case ZPT_SOURCE_EVENT_OTHER:
    default:
        break;
    }
    return sync && zpt_motion_source_take(&ingress->source, now_ms, 0U, 0U, signal);
}

void zpt_source_ingress_follow_resolution(struct zpt_source_ingress *ingress, uint16_t cpi) {
    if (ingress == NULL || cpi == 0U) {
        return;
    }
    const struct zpt_motion_source_state *source = &ingress->source;
    const uint16_t upcoming = source->staged_resolution_cpi != 0U ? source->staged_resolution_cpi
                                                                  : source->resolution_cpi;
    if (cpi != upcoming) {
        (void)zpt_motion_source_set_resolution_cpi(&ingress->source, cpi);
    }
}
