/* SPDX-License-Identifier: MIT */

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>

#include <zmk/pointing_tools/source/transport/compact_split_codec.h>

static uint32_t encode(struct zpt_compact_split_encoder *encoder, int64_t x, int64_t y,
                       uint32_t span_us) {
    const struct zpt_raw_motion motion = {.x_counts = x, .y_counts = y};
    uint32_t packet = 0U;
    assert(zpt_compact_split_encode(encoder, &motion, span_us, &packet) == 0);
    return packet;
}

static struct zpt_compact_split_frame decode(struct zpt_compact_split_decoder *decoder,
                                             uint32_t packet) {
    struct zpt_compact_split_frame frame;
    assert(zpt_compact_split_decode(decoder, packet, &frame) == 0);
    return frame;
}

static void test_signed_boundaries_and_observed_ranges_round_trip(void) {
    struct zpt_compact_split_encoder encoder;
    struct zpt_compact_split_decoder decoder;
    zpt_compact_split_encoder_init(&encoder);
    zpt_compact_split_decoder_init(&decoder);

    struct zpt_compact_split_frame frame = decode(
        &decoder, encode(&encoder, ZPT_COMPACT_SPLIT_AXIS_MIN, ZPT_COMPACT_SPLIT_AXIS_MAX, 16000));
    assert(frame.motion.x_counts == ZPT_COMPACT_SPLIT_AXIS_MIN);
    assert(frame.motion.y_counts == ZPT_COMPACT_SPLIT_AXIS_MAX);
    assert(frame.sample_span_us == 16000);
    assert(frame.sequence == 0);
    assert(frame.flags == ZPT_SIGNAL_FLAG_TIMING_ESTIMATED);

    /* Extremes observed in the PAW3222 and repaired Bridges split traces. */
    frame = decode(&decoder, encode(&encoder, -112, 127, 15000));
    assert(frame.motion.x_counts == -112 && frame.motion.y_counts == 127);
    assert(frame.sample_span_us == 16000);
    assert(frame.sequence == 1);
    assert((frame.flags & ZPT_SIGNAL_FLAG_SEQUENCE_GAP) == 0U);
}

static void test_partial_and_empty_frames_are_representable(void) {
    struct zpt_compact_split_encoder encoder;
    struct zpt_compact_split_decoder decoder;
    zpt_compact_split_encoder_init(&encoder);
    zpt_compact_split_decoder_init(&decoder);

    struct zpt_compact_split_frame frame = decode(&decoder, encode(&encoder, 0, -9, 0));
    assert(frame.motion.x_counts == 0 && frame.motion.y_counts == -9);
    assert(frame.sample_span_us == 0);

    frame = decode(&decoder, encode(&encoder, 0, 0, 8000));
    assert(frame.motion.x_counts == 0 && frame.motion.y_counts == 0);
    assert(frame.sample_span_us == 8000);
}

static void test_range_failure_is_not_clipped_and_exposes_gap(void) {
    struct zpt_compact_split_encoder encoder;
    struct zpt_compact_split_decoder decoder;
    zpt_compact_split_encoder_init(&encoder);
    zpt_compact_split_decoder_init(&decoder);

    decode(&decoder, encode(&encoder, 1, 2, 8000));

    const struct zpt_raw_motion too_large = {
        .x_counts = ZPT_COMPACT_SPLIT_AXIS_MAX + 1,
        .y_counts = 0,
    };
    uint32_t unchanged = UINT32_C(0x12345678);
    assert(zpt_compact_split_encode(&encoder, &too_large, 8000, &unchanged) == -ERANGE);
    assert(unchanged == UINT32_C(0x12345678));
    assert(encoder.diagnostics.range_errors == 1);

    struct zpt_compact_split_frame frame = decode(&decoder, encode(&encoder, 3, 4, 8000));
    assert(frame.motion.x_counts == 3 && frame.motion.y_counts == 4);
    assert(frame.sequence == 2);
    assert((frame.flags & ZPT_SIGNAL_FLAG_SEQUENCE_GAP) != 0U);
    assert((frame.flags & ZPT_SIGNAL_FLAG_DISCONTINUITY) != 0U);
    assert(decoder.diagnostics.sequence_discontinuities == 1);
    assert(decoder.diagnostics.estimated_dropped_packets == 1);
}

static void test_sequence_wrap_is_continuous(void) {
    struct zpt_compact_split_encoder encoder;
    struct zpt_compact_split_decoder decoder;
    zpt_compact_split_encoder_init(&encoder);
    zpt_compact_split_decoder_init(&decoder);
    encoder.next_sequence = 15;

    struct zpt_compact_split_frame frame = decode(&decoder, encode(&encoder, 1, 0, 8000));
    assert(frame.sequence == 15);
    frame = decode(&decoder, encode(&encoder, 2, 0, 8000));
    assert(frame.sequence == 16);
    assert((frame.flags & ZPT_SIGNAL_FLAG_SEQUENCE_GAP) == 0U);
}

static void test_timing_saturation_is_explicit(void) {
    struct zpt_compact_split_encoder encoder;
    struct zpt_compact_split_decoder decoder;
    zpt_compact_split_encoder_init(&encoder);
    zpt_compact_split_decoder_init(&decoder);

    struct zpt_compact_split_frame frame = decode(&decoder, encode(&encoder, 1, 1, 29000));
    assert(frame.sample_span_us == 30000);
    assert((frame.flags & ZPT_SIGNAL_FLAG_SAMPLE_SPAN_CLIPPED) != 0U);
    assert(encoder.diagnostics.sample_span_saturations == 1);
    assert(decoder.diagnostics.sample_span_saturations == 1);
}

static void test_wrong_format_is_rejected_without_changing_state(void) {
    struct zpt_compact_split_encoder encoder;
    struct zpt_compact_split_decoder decoder;
    zpt_compact_split_encoder_init(&encoder);
    zpt_compact_split_decoder_init(&decoder);

    uint32_t packet = encode(&encoder, -5, 7, 16000);
    packet &= ~(UINT32_C(3) << 30);
    struct zpt_compact_split_frame unchanged = {
        .motion = {.x_counts = 99, .y_counts = 88},
    };
    assert(zpt_compact_split_decode(&decoder, packet, &unchanged) == -EBADMSG);
    assert(unchanged.motion.x_counts == 99 && unchanged.motion.y_counts == 88);
    assert(!decoder.saw_sequence);
    assert(decoder.diagnostics.format_errors == 1);
}

static void test_distance_is_exact_across_packets(void) {
    struct zpt_compact_split_encoder encoder;
    struct zpt_compact_split_decoder decoder;
    zpt_compact_split_encoder_init(&encoder);
    zpt_compact_split_decoder_init(&decoder);

    int64_t input_x = 0;
    int64_t input_y = 0;
    int64_t output_x = 0;
    int64_t output_y = 0;
    for (int32_t i = -45; i <= 45; i += 3) {
        int64_t x = i;
        int64_t y = 45 - i;
        input_x += x;
        input_y += y;
        struct zpt_compact_split_frame frame = decode(&decoder, encode(&encoder, x, y, 16000));
        output_x += frame.motion.x_counts;
        output_y += frame.motion.y_counts;
    }
    assert(output_x == input_x && output_y == input_y);
    assert(encoder.diagnostics.encoded_packets == decoder.diagnostics.decoded_packets);
}

int main(void) {
    test_signed_boundaries_and_observed_ranges_round_trip();
    test_partial_and_empty_frames_are_representable();
    test_range_failure_is_not_clipped_and_exposes_gap();
    test_sequence_wrap_is_continuous();
    test_timing_saturation_is_explicit();
    test_wrong_format_is_rejected_without_changing_state();
    test_distance_is_exact_across_packets();
    puts("compact split codec tests passed");
    return 0;
}
