/* SPDX-License-Identifier: MIT */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <zmk/pointing_tools/core/signal.h>
#include <zmk/pointing_tools/source/motion_source.h>
#include <zmk/pointing_tools/source/transport/compact_split_codec.h>

/* Platform-neutral frame reconstruction for one motion source. The ZMK input
 * processor classifies each input event and feeds it here; everything that
 * decides when a frame is complete lives in this host-testable unit. */

enum zpt_source_event_kind {
    ZPT_SOURCE_EVENT_OTHER = 0,
    ZPT_SOURCE_EVENT_AXIS_X,
    ZPT_SOURCE_EVENT_AXIS_Y,
    /* One complete compact-split packet in the event value. */
    ZPT_SOURCE_EVENT_COMPACT,
};

struct zpt_source_ingress_config {
    struct zpt_motion_source_config source;
    bool compact_transport_coalesced;
};

struct zpt_source_ingress {
    struct zpt_motion_source_state source;
    struct zpt_compact_split_decoder compact_decoder;
};

int zpt_source_ingress_init(struct zpt_source_ingress *ingress,
                            const struct zpt_source_ingress_config *config);

/* Feed one classified input event observed at now_ms.
 *
 * Returns true and fills *signal when the event completes a frame: a native
 * axis frame completes on sync, a compact packet is a whole frame by itself
 * (its own sync never takes a second, empty frame). A rejected compact
 * packet reports the codec error through *decode_error (zero otherwise) and
 * completes nothing. */
bool zpt_source_ingress_handle(struct zpt_source_ingress *ingress,
                               const struct zpt_source_ingress_config *config,
                               enum zpt_source_event_kind kind, int32_t value, bool sync,
                               uint32_t now_ms, struct zpt_signal *signal, int *decode_error);

/* Follow a resolution owned elsewhere, such as a pointing-device table
 * entry, by calling this before each event. It does nothing while cpi
 * matches the resolution the next frame will carry. A change applies at
 * once between frames, or after the frame in progress, so no frame mixes
 * counts from two resolutions. A zero cpi is ignored. */
void zpt_source_ingress_follow_resolution(struct zpt_source_ingress *ingress, uint16_t cpi);
