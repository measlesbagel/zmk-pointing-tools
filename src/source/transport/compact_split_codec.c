/* SPDX-License-Identifier: MIT */

#include <errno.h>
#include <stddef.h>

#include <zmk/pointing_tools/source/transport/compact_split_codec.h>

#define AXIS_BITS 11U
#define AXIS_MASK UINT32_C(0x7ff)
#define AXIS_SIGN_BIT UINT32_C(0x400)
#define X_SHIFT 0U
#define Y_SHIFT 11U
#define SEQUENCE_SHIFT 22U
#define SEQUENCE_MASK UINT32_C(0x0f)
#define SPAN_SHIFT 26U
#define SPAN_MASK UINT32_C(0x0f)
#define SPAN_SATURATED_CODE UINT32_C(0x0f)
#define FORMAT_SHIFT 30U
#define FORMAT_MASK UINT32_C(0x03)
#define CURRENT_FORMAT UINT32_C(0x02)

_Static_assert(Y_SHIFT == X_SHIFT + AXIS_BITS, "compact axes must not overlap");
_Static_assert(FORMAT_SHIFT + 2U == 32U, "compact packet must use exactly 32 bits");

static uint32_t encode_axis(int64_t value) { return (uint32_t)value & AXIS_MASK; }

static int64_t decode_axis(uint32_t value) {
    value &= AXIS_MASK;
    if ((value & AXIS_SIGN_BIT) != 0U) {
        return (int64_t)value - (INT64_C(1) << AXIS_BITS);
    }
    return value;
}

static uint32_t encode_span(uint32_t sample_span_us, bool *saturated) {
    *saturated = false;
    if (sample_span_us == 0U) {
        return 0U;
    }

    uint32_t code = ((uint64_t)sample_span_us + (ZPT_COMPACT_SPLIT_SPAN_QUANTUM_US / 2U)) /
                    ZPT_COMPACT_SPLIT_SPAN_QUANTUM_US;
    if (code == 0U) {
        code = 1U;
    }
    if (code >= SPAN_SATURATED_CODE) {
        *saturated = true;
        return SPAN_SATURATED_CODE;
    }
    return code;
}

void zpt_compact_split_encoder_init(struct zpt_compact_split_encoder *encoder) {
    if (encoder != NULL) {
        *encoder = (struct zpt_compact_split_encoder){0};
    }
}

void zpt_compact_split_decoder_init(struct zpt_compact_split_decoder *decoder) {
    if (decoder != NULL) {
        *decoder = (struct zpt_compact_split_decoder){0};
    }
}

int zpt_compact_split_encode(struct zpt_compact_split_encoder *encoder,
                             const struct zpt_raw_motion *motion, uint32_t sample_span_us,
                             uint32_t *packet) {
    if (encoder == NULL || motion == NULL || packet == NULL) {
        return -EINVAL;
    }

    uint32_t sequence = encoder->next_sequence++ & SEQUENCE_MASK;
    if (motion->x_counts < ZPT_COMPACT_SPLIT_AXIS_MIN ||
        motion->x_counts > ZPT_COMPACT_SPLIT_AXIS_MAX ||
        motion->y_counts < ZPT_COMPACT_SPLIT_AXIS_MIN ||
        motion->y_counts > ZPT_COMPACT_SPLIT_AXIS_MAX) {
        encoder->diagnostics.range_errors++;
        return -ERANGE;
    }

    bool span_saturated;
    uint32_t span = encode_span(sample_span_us, &span_saturated);
    if (span_saturated) {
        encoder->diagnostics.sample_span_saturations++;
    }

    *packet = (CURRENT_FORMAT << FORMAT_SHIFT) | (span << SPAN_SHIFT) |
              (sequence << SEQUENCE_SHIFT) | (encode_axis(motion->y_counts) << Y_SHIFT) |
              (encode_axis(motion->x_counts) << X_SHIFT);
    encoder->diagnostics.encoded_packets++;
    return 0;
}

int zpt_compact_split_decode(struct zpt_compact_split_decoder *decoder, uint32_t packet,
                             struct zpt_compact_split_frame *frame) {
    if (decoder == NULL || frame == NULL) {
        return -EINVAL;
    }
    if (((packet >> FORMAT_SHIFT) & FORMAT_MASK) != CURRENT_FORMAT) {
        decoder->diagnostics.format_errors++;
        return -EBADMSG;
    }

    uint8_t wire_sequence = (packet >> SEQUENCE_SHIFT) & SEQUENCE_MASK;
    uint32_t flags = ZPT_SIGNAL_FLAG_TIMING_ESTIMATED;
    uint16_t expanded_sequence;

    if (!decoder->saw_sequence) {
        expanded_sequence = wire_sequence;
        decoder->saw_sequence = true;
    } else {
        uint8_t delta = (wire_sequence - decoder->previous_wire_sequence) & SEQUENCE_MASK;
        if (delta == 1U) {
            expanded_sequence = decoder->expanded_sequence + 1U;
        } else {
            flags |= ZPT_SIGNAL_FLAG_SEQUENCE_GAP | ZPT_SIGNAL_FLAG_DISCONTINUITY;
            decoder->diagnostics.sequence_discontinuities++;
            if (delta > 1U) {
                decoder->diagnostics.estimated_dropped_packets += delta - 1U;
                expanded_sequence = decoder->expanded_sequence + delta;
            } else {
                /* Duplicate or a gap of a whole wire-sequence modulus. */
                expanded_sequence = decoder->expanded_sequence + 1U;
            }
        }
    }

    uint32_t span_code = (packet >> SPAN_SHIFT) & SPAN_MASK;
    uint32_t sample_span_us = span_code * ZPT_COMPACT_SPLIT_SPAN_QUANTUM_US;
    if (span_code == SPAN_SATURATED_CODE) {
        flags |= ZPT_SIGNAL_FLAG_SAMPLE_SPAN_CLIPPED;
        decoder->diagnostics.sample_span_saturations++;
    }

    *frame = (struct zpt_compact_split_frame){
        .motion =
            {
                .x_counts = decode_axis(packet >> X_SHIFT),
                .y_counts = decode_axis(packet >> Y_SHIFT),
            },
        .sample_span_us = sample_span_us,
        .flags = flags,
        .sequence = expanded_sequence,
    };
    decoder->previous_wire_sequence = wire_sequence;
    decoder->expanded_sequence = expanded_sequence;
    decoder->diagnostics.decoded_packets++;
    return 0;
}
