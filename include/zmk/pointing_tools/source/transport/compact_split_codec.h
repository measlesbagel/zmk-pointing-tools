/* SPDX-License-Identifier: MIT */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <zmk/pointing_tools/core/signal.h>

/* One complete two-axis source frame in one ZMK split input-event value. */
#define ZPT_COMPACT_SPLIT_AXIS_MIN (-1024)
#define ZPT_COMPACT_SPLIT_AXIS_MAX 1023
#define ZPT_COMPACT_SPLIT_SEQUENCE_MODULUS 16U
#define ZPT_COMPACT_SPLIT_SPAN_QUANTUM_US 2000U

struct zpt_compact_split_diagnostics {
    uint32_t encoded_packets;
    uint32_t decoded_packets;
    uint32_t range_errors;
    uint32_t format_errors;
    uint32_t sequence_discontinuities;
    uint32_t estimated_dropped_packets;
    uint32_t sample_span_saturations;
};

struct zpt_compact_split_encoder {
    uint16_t next_sequence;
    struct zpt_compact_split_diagnostics diagnostics;
};

struct zpt_compact_split_decoder {
    uint16_t expanded_sequence;
    uint8_t previous_wire_sequence;
    bool saw_sequence;
    struct zpt_compact_split_diagnostics diagnostics;
};

struct zpt_compact_split_frame {
    struct zpt_raw_motion motion;
    uint32_t sample_span_us;
    uint32_t flags;
    uint16_t sequence;
};

void zpt_compact_split_encoder_init(struct zpt_compact_split_encoder *encoder);
void zpt_compact_split_decoder_init(struct zpt_compact_split_decoder *decoder);

/*
 * Encode exact signed counts. A range failure emits no packet but consumes a
 * sequence number so the next successful packet exposes the discontinuity.
 */
int zpt_compact_split_encode(struct zpt_compact_split_encoder *encoder,
                             const struct zpt_raw_motion *motion, uint32_t sample_span_us,
                             uint32_t *packet);

/* Decode only the current format and expand its rolling sequence. */
int zpt_compact_split_decode(struct zpt_compact_split_decoder *decoder, uint32_t packet,
                             struct zpt_compact_split_frame *frame);
