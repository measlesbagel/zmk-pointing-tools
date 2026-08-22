/* SPDX-License-Identifier: MIT */

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <zmk/pointing_tools/source/transport/compact_split_codec.h>

/*
 * Deterministic soak test for the compact split codec.
 *
 * The unit tests in compact_split_codec_test.c cover the codec's semantics
 * (round trip, gaps, duplicates, format rejection, wire-sequence wrap). This
 * test adds what hand-picked cases cannot: long stateful runs through the
 * decoder's uint16 expanded-sequence wrap and adversarial bit-level input,
 * verified against an independent model of the state machine. It is cheap
 * (~1s) and runs in the regular and sanitizer test builds, where it also
 * guards the codec against memory and undefined behavior under random input.
 */

#define SOAK_RANDOM_PACKETS (UINT32_C(1) << 24)
#define SOAK_ROUND_TRIP_PACKETS (UINT32_C(1) << 22)

/* The current wire format, mirrored from the codec's packet layout. */
#define MODEL_FORMAT_SHIFT 30U
#define MODEL_FORMAT_MASK UINT32_C(3)
#define MODEL_FORMAT UINT32_C(2)
#define MODEL_SEQUENCE_SHIFT 22U
#define MODEL_SEQUENCE_MASK UINT32_C(0x0f)
#define MODEL_SPAN_SHIFT 26U
#define MODEL_SPAN_MASK UINT32_C(0x0f)
#define MODEL_SPAN_SATURATED_CODE UINT32_C(0x0f)
#define MODEL_AXIS_MASK UINT32_C(0x7ff)
#define MODEL_AXIS_SIGN UINT32_C(0x400)

static uint64_t rng_state;

static uint32_t next_bits(void) {
    uint64_t z = (rng_state += UINT64_C(0x9E3779B97F4A7C15));
    z = (z ^ (z >> 30)) * UINT64_C(0xBF58476D1CE4E5B9);
    z = (z ^ (z >> 27)) * UINT64_C(0x94D049BB133111EB);
    return (uint32_t)(z ^ (z >> 31));
}

static int64_t model_decode_axis(uint32_t value) {
    value &= MODEL_AXIS_MASK;
    if ((value & MODEL_AXIS_SIGN) != 0U) {
        return (int64_t)value - (INT64_C(1) << 11);
    }
    return value;
}

/* The codec quantizes spans to 2 ms; code 15 is the saturation code. */
static uint32_t model_span_us(uint32_t span_code) {
    return span_code * ZPT_COMPACT_SPLIT_SPAN_QUANTUM_US;
}

/*
 * Feed random bit patterns to the decoder. Every accepted frame is checked
 * field by field against an independent model of the sequence state machine,
 * and the diagnostics are reconciled at the end.
 */
static void soak_random_decode(void) {
    rng_state = UINT64_C(0x0123456789ABCDEF);
    struct zpt_compact_split_decoder decoder;
    zpt_compact_split_decoder_init(&decoder);

    bool model_saw_sequence = false;
    uint8_t model_previous_wire = 0;
    uint16_t model_expanded = 0;
    uint32_t model_format_errors = 0;
    uint32_t model_discontinuities = 0;
    uint32_t model_dropped = 0;
    uint32_t model_saturation = 0;

    for (uint32_t i = 0; i < SOAK_RANDOM_PACKETS; i++) {
        uint32_t packet = next_bits();
        struct zpt_compact_split_frame frame;
        const int rc = zpt_compact_split_decode(&decoder, packet, &frame);

        if (((packet >> MODEL_FORMAT_SHIFT) & MODEL_FORMAT_MASK) != MODEL_FORMAT) {
            assert(rc == -EBADMSG);
            model_format_errors++;
            continue; /* format errors must not change the decoder state */
        }
        assert(rc == 0);

        const uint8_t wire = (packet >> MODEL_SEQUENCE_SHIFT) & MODEL_SEQUENCE_MASK;
        const uint32_t span_code = (packet >> MODEL_SPAN_SHIFT) & MODEL_SPAN_MASK;
        bool model_gap = false;

        if (!model_saw_sequence) {
            model_saw_sequence = true;
            model_expanded = wire;
        } else {
            const uint8_t delta = (uint8_t)(wire - model_previous_wire) & MODEL_SEQUENCE_MASK;
            if (delta == 1U) {
                model_expanded++;
            } else {
                model_gap = true;
                model_discontinuities++;
                if (delta > 1U) {
                    model_dropped += delta - 1U;
                    model_expanded += delta;
                } else {
                    model_expanded++;
                }
            }
        }
        model_previous_wire = wire;

        assert(frame.sequence == model_expanded);
        assert(frame.motion.x_counts == model_decode_axis(packet >> 0U));
        assert(frame.motion.y_counts == model_decode_axis(packet >> 11U));
        assert(frame.sample_span_us == model_span_us(span_code));

        uint32_t expected_flags = ZPT_SIGNAL_FLAG_TIMING_ESTIMATED;
        if (model_gap) {
            expected_flags |= ZPT_SIGNAL_FLAG_SEQUENCE_GAP | ZPT_SIGNAL_FLAG_DISCONTINUITY;
        }
        if (span_code == MODEL_SPAN_SATURATED_CODE) {
            expected_flags |= ZPT_SIGNAL_FLAG_SAMPLE_SPAN_CLIPPED;
            model_saturation++;
        }
        assert(frame.flags == expected_flags);
    }

    assert(decoder.diagnostics.decoded_packets == SOAK_RANDOM_PACKETS - model_format_errors);
    assert(decoder.diagnostics.format_errors == model_format_errors);
    assert(decoder.diagnostics.sequence_discontinuities == model_discontinuities);
    assert(decoder.diagnostics.estimated_dropped_packets == model_dropped);
    assert(decoder.diagnostics.sample_span_saturations == model_saturation);
    printf("random decode: %u packets, %u format errors, %u discontinuities, %u dropped\n",
           SOAK_RANDOM_PACKETS, model_format_errors, model_discontinuities, model_dropped);
}

/*
 * Encode and decode long random motion streams. The encoder and decoder
 * sequence counters wrap at 2^16; running well past the wrap proves the
 * round trip stays exact through it.
 */
static void soak_round_trip(void) {
    rng_state = UINT64_C(0xFEDCBA9876543210);
    struct zpt_compact_split_encoder encoder;
    struct zpt_compact_split_decoder decoder;
    zpt_compact_split_encoder_init(&encoder);
    zpt_compact_split_decoder_init(&decoder);

    for (uint32_t i = 0; i < SOAK_ROUND_TRIP_PACKETS; i++) {
        const int64_t x = (int64_t)(next_bits() % 2048U) - 1024;
        const int64_t y = (int64_t)(next_bits() % 2048U) - 1024;
        const uint32_t span_us = next_bits() % 40U * ZPT_COMPACT_SPLIT_SPAN_QUANTUM_US;
        const struct zpt_raw_motion motion = {.x_counts = x, .y_counts = y};

        uint32_t packet = 0U;
        assert(zpt_compact_split_encode(&encoder, &motion, span_us, &packet) == 0);

        struct zpt_compact_split_frame frame;
        assert(zpt_compact_split_decode(&decoder, packet, &frame) == 0);
        assert(frame.motion.x_counts == x);
        assert(frame.motion.y_counts == y);
        assert(frame.sample_span_us ==
               model_span_us((packet >> MODEL_SPAN_SHIFT) & MODEL_SPAN_MASK));
        /* The encoder has already consumed the sequence for this packet. */
        assert(frame.sequence == (uint16_t)(encoder.next_sequence - 1U));
        assert((frame.flags & ZPT_SIGNAL_FLAG_SEQUENCE_GAP) == 0U);
        assert((frame.flags & ZPT_SIGNAL_FLAG_DISCONTINUITY) == 0U);
    }

    assert(encoder.diagnostics.encoded_packets == decoder.diagnostics.decoded_packets);
    assert(encoder.diagnostics.encoded_packets == SOAK_ROUND_TRIP_PACKETS);
    assert(decoder.diagnostics.sequence_discontinuities == 0);
    assert(decoder.diagnostics.estimated_dropped_packets == 0);
    printf("round trip: %u packets across %u expanded-sequence wraps\n", SOAK_ROUND_TRIP_PACKETS,
           SOAK_ROUND_TRIP_PACKETS / UINT32_C(65536));
}

int main(void) {
    soak_random_decode();
    soak_round_trip();
    puts("compact split codec soak tests passed");
    return 0;
}
