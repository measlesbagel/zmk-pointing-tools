/* SPDX-License-Identifier: MIT */

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>

#include <zmk/pointing_tools/source/source_ingress.h>

static const struct zpt_source_ingress_config native_config = {
    .source =
        {
            .flags = ZPT_SIGNAL_FLAG_LOCAL,
            .source_id = 1,
            .resolution_cpi = 684,
        },
};

static const struct zpt_source_ingress_config compact_config = {
    .source =
        {
            .flags = ZPT_SIGNAL_FLAG_TRANSPORTED,
            .source_id = 2,
            .resolution_cpi = 684,
        },
    .compact_transport_coalesced = true,
};

static uint32_t encode(struct zpt_compact_split_encoder *encoder, int64_t x, int64_t y,
                       uint32_t span_us) {
    const struct zpt_raw_motion motion = {.x_counts = x, .y_counts = y};
    uint32_t packet = 0U;
    assert(zpt_compact_split_encode(encoder, &motion, span_us, &packet) == 0);
    return packet;
}

static void test_native_axes_complete_on_sync(void) {
    struct zpt_source_ingress ingress;
    struct zpt_signal signal;
    int error = -1;
    assert(zpt_source_ingress_init(&ingress, &native_config) == 0);

    assert(!zpt_source_ingress_handle(&ingress, &native_config, ZPT_SOURCE_EVENT_AXIS_X, 5, false,
                                      10, &signal, &error));
    assert(error == 0);
    assert(zpt_source_ingress_handle(&ingress, &native_config, ZPT_SOURCE_EVENT_AXIS_Y, -3, true,
                                     11, &signal, &error));
    assert(signal.kind == ZPT_SIGNAL_RAW_MOTION);
    assert(signal.data.raw_motion.x_counts == 5 && signal.data.raw_motion.y_counts == -3);
    assert(signal.metadata.observed_at_ms == 11 && signal.metadata.sequence == 0);
    assert(signal.metadata.resolution_cpi == 684);

    /* An unrelated synced event with nothing pending completes no frame. */
    assert(!zpt_source_ingress_handle(&ingress, &native_config, ZPT_SOURCE_EVENT_OTHER, 0, true, 12,
                                      &signal, &error));
}

/* Regression: the compact encoder always reports its packet with sync set.
 * The adapter used to decode the frame and then let the sync take a second,
 * empty frame, overwriting the result, so no compact frame ever reached the
 * router. */
static void test_synced_compact_packet_yields_its_frame(void) {
    struct zpt_compact_split_encoder encoder;
    struct zpt_source_ingress ingress;
    struct zpt_signal signal;
    int error = -1;
    zpt_compact_split_encoder_init(&encoder);
    assert(zpt_source_ingress_init(&ingress, &compact_config) == 0);

    assert(zpt_source_ingress_handle(&ingress, &compact_config, ZPT_SOURCE_EVENT_COMPACT,
                                     (int32_t)encode(&encoder, -112, 45, 16000), true, 100, &signal,
                                     &error));
    assert(error == 0);
    assert(signal.data.raw_motion.x_counts == -112 && signal.data.raw_motion.y_counts == 45);
    assert(signal.metadata.sample_span_us == 16000);
    assert(signal.metadata.sequence == 0);
    assert((signal.metadata.flags & ZPT_SIGNAL_FLAG_TRANSPORTED) != 0U);
    assert((signal.metadata.flags & ZPT_SIGNAL_FLAG_COALESCED) != 0U);
    assert((signal.metadata.flags & ZPT_SIGNAL_FLAG_TIMING_ESTIMATED) != 0U);

    assert(zpt_source_ingress_handle(&ingress, &compact_config, ZPT_SOURCE_EVENT_COMPACT,
                                     (int32_t)encode(&encoder, 3, -4, 16000), true, 116, &signal,
                                     &error));
    assert(signal.data.raw_motion.x_counts == 3 && signal.data.raw_motion.y_counts == -4);
    assert(signal.metadata.sequence == 1);
}

static void test_rejected_compact_packet_reports_error(void) {
    struct zpt_source_ingress ingress;
    struct zpt_signal signal;
    int error = 0;
    assert(zpt_source_ingress_init(&ingress, &compact_config) == 0);

    /* Format bits 00 are not the current packet format. */
    assert(!zpt_source_ingress_handle(&ingress, &compact_config, ZPT_SOURCE_EVENT_COMPACT, 0x1234,
                                      true, 5, &signal, &error));
    assert(error == -EBADMSG);
}

static void test_followed_resolution_waits_for_frame_boundary(void) {
    struct zpt_source_ingress ingress;
    struct zpt_signal signal;
    int error = 0;
    assert(zpt_source_ingress_init(&ingress, &native_config) == 0);

    /* Between frames a new resolution applies to the very next frame. */
    zpt_source_ingress_follow_resolution(&ingress, 912);
    assert(zpt_source_ingress_handle(&ingress, &native_config, ZPT_SOURCE_EVENT_AXIS_X, 2, true, 1,
                                     &signal, &error));
    assert(signal.metadata.resolution_cpi == 912);

    /* Mid-frame, the counts already read keep the old label. */
    assert(!zpt_source_ingress_handle(&ingress, &native_config, ZPT_SOURCE_EVENT_AXIS_X, 3, false,
                                      2, &signal, &error));
    zpt_source_ingress_follow_resolution(&ingress, 1216);
    zpt_source_ingress_follow_resolution(&ingress, 1216);
    assert(zpt_source_ingress_handle(&ingress, &native_config, ZPT_SOURCE_EVENT_AXIS_Y, 1, true, 3,
                                     &signal, &error));
    assert(signal.metadata.resolution_cpi == 912);
    assert(zpt_source_ingress_handle(&ingress, &native_config, ZPT_SOURCE_EVENT_AXIS_Y, 1, true, 4,
                                     &signal, &error));
    assert(signal.metadata.resolution_cpi == 1216);

    /* Zero means unknown and never replaces a real resolution. */
    zpt_source_ingress_follow_resolution(&ingress, 0);
    zpt_source_ingress_follow_resolution(NULL, 800);
    assert(zpt_source_ingress_handle(&ingress, &native_config, ZPT_SOURCE_EVENT_AXIS_Y, 1, true, 5,
                                     &signal, &error));
    assert(signal.metadata.resolution_cpi == 1216);
}

static void test_followed_resolution_labels_compact_frames(void) {
    struct zpt_compact_split_encoder encoder;
    struct zpt_source_ingress ingress;
    struct zpt_signal signal;
    int error = 0;
    zpt_compact_split_encoder_init(&encoder);
    assert(zpt_source_ingress_init(&ingress, &compact_config) == 0);

    /* A compact packet is a whole frame, so nothing is ever pending. */
    zpt_source_ingress_follow_resolution(&ingress, 1596);
    assert(zpt_source_ingress_handle(&ingress, &compact_config, ZPT_SOURCE_EVENT_COMPACT,
                                     (int32_t)encode(&encoder, 7, 7, 8000), true, 10, &signal,
                                     &error));
    assert(signal.metadata.resolution_cpi == 1596);
}

static void test_invalid_arguments(void) {
    struct zpt_source_ingress ingress;
    int error = 0;
    assert(zpt_source_ingress_init(NULL, &native_config) == -EINVAL);
    assert(zpt_source_ingress_init(&ingress, NULL) == -EINVAL);
    assert(zpt_source_ingress_init(&ingress, &native_config) == 0);
    assert(!zpt_source_ingress_handle(&ingress, &native_config, ZPT_SOURCE_EVENT_AXIS_X, 1, true, 0,
                                      NULL, &error));
}

int main(void) {
    test_native_axes_complete_on_sync();
    test_synced_compact_packet_yields_its_frame();
    test_rejected_compact_packet_reports_error();
    test_followed_resolution_waits_for_frame_boundary();
    test_followed_resolution_labels_compact_frames();
    test_invalid_arguments();
    puts("source ingress tests passed");
    return 0;
}
