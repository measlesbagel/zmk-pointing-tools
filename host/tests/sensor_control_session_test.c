/* SPDX-License-Identifier: MIT */

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>

#include <zmk/pointing_tools/source/sensor_control_session.h>
#include <zmk/pointing_tools/source/sensor_control_wire.h>

/* What the peripheral agent does with a request: echo its sequence through
 * the four-bit request field. */
static uint8_t wire_round_trip(uint8_t seq) {
    return zpt_scw_param_seq(zpt_scw_param1(seq, ZPT_SCW_OPCODE_GET_CPI, 0));
}

/* Regression: sequences used to run 1..16, and 16 went out as 0 in the
 * four-bit field, so every sixteenth request timed out. */
static void test_sequences_survive_the_wire_across_wraps(void) {
    struct zpt_scw_session session;
    zpt_scw_session_init(&session);

    for (unsigned int request = 0; request < 3U * ZPT_SCW_SEQ_MAX + 2U; request++) {
        const uint8_t seq = zpt_scw_session_begin(&session);
        assert(seq >= 1U && seq <= ZPT_SCW_SEQ_MAX);
        const uint8_t echoed = wire_round_trip(seq);
        assert(echoed == seq);
        assert(zpt_scw_session_offer(&session, zpt_scw_encode(0, echoed, (uint16_t)request)));

        uint16_t value = 0;
        assert(zpt_scw_session_result(&session, &value) == 0);
        assert(value == (uint16_t)request);
    }
}

static void test_errors_and_values_are_reported(void) {
    struct zpt_scw_session session;
    zpt_scw_session_init(&session);

    const uint8_t seq = zpt_scw_session_begin(&session);
    assert(zpt_scw_session_offer(&session, zpt_scw_encode(ENOSYS, seq, 684)));
    uint16_t value = 0;
    assert(zpt_scw_session_result(&session, &value) == -ENOSYS);
    assert(value == 684);
    assert(zpt_scw_session_result(&session, NULL) == -ENOSYS);
}

static void test_foreign_stale_and_unsolicited_frames_are_dropped(void) {
    struct zpt_scw_session session;
    zpt_scw_session_init(&session);

    /* Nothing outstanding. */
    assert(!zpt_scw_session_offer(&session, zpt_scw_encode(0, 1, 800)));

    const uint8_t seq = zpt_scw_session_begin(&session);
    /* Untagged motion-like values and other sequences don't answer it. */
    assert(!zpt_scw_session_offer(&session, 0x00000005u));
    assert(!zpt_scw_session_offer(&session, (uint32_t)(int32_t)-3));
    assert(!zpt_scw_session_offer(&session, zpt_scw_encode(0, (uint8_t)(seq + 1U), 800)));
    assert(zpt_scw_session_offer(&session, zpt_scw_encode(0, seq, 800)));

    /* A duplicate of the answer arrives after completion. */
    assert(!zpt_scw_session_offer(&session, zpt_scw_encode(0, seq, 800)));
}

static void test_late_response_after_timeout_is_dropped(void) {
    struct zpt_scw_session session;
    zpt_scw_session_init(&session);

    const uint8_t timed_out = zpt_scw_session_begin(&session);
    zpt_scw_session_cancel(&session);
    assert(!zpt_scw_session_offer(&session, zpt_scw_encode(0, timed_out, 400)));

    /* The next request ignores the late answer to the abandoned one. */
    const uint8_t next = zpt_scw_session_begin(&session);
    assert(next != timed_out);
    assert(!zpt_scw_session_offer(&session, zpt_scw_encode(0, timed_out, 400)));
    assert(zpt_scw_session_offer(&session, zpt_scw_encode(0, next, 1200)));
    uint16_t value = 0;
    assert(zpt_scw_session_result(&session, &value) == 0 && value == 1200);
}

int main(void) {
    test_sequences_survive_the_wire_across_wraps();
    test_errors_and_values_are_reported();
    test_foreign_stale_and_unsolicited_frames_are_dropped();
    test_late_response_after_timeout_is_dropped();
    puts("sensor control session tests passed");
    return 0;
}
