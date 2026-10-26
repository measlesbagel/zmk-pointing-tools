/* SPDX-License-Identifier: MIT */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Correlates split sensor-control requests with their packed responses (see
 * sensor_control_wire.h). One request is outstanding at a time, and the
 * caller serializes access, so the central proxy only adds a lock, a
 * semaphore, and the transport around this host-testable core. */

/* Sequence numbers run 1..15: the wire field is four bits wide and zero
 * marks "no request outstanding". */
#define ZPT_SCW_SEQ_MAX 15u

struct zpt_scw_session {
    uint8_t last_seq;
    uint8_t expected_seq;
    uint8_t status_errno;
    uint16_t value;
};

void zpt_scw_session_init(struct zpt_scw_session *session);

/* Start a request and return its sequence number, never zero. Any earlier
 * outstanding request is abandoned. */
uint8_t zpt_scw_session_begin(struct zpt_scw_session *session);

/* Offer one received response frame. Returns true when it answers the
 * outstanding request, which then completes. Frames without the tag, for
 * another sequence, or arriving with nothing outstanding are dropped. */
bool zpt_scw_session_offer(struct zpt_scw_session *session, uint32_t raw);

/* Abandon the outstanding request after a timeout or a failed send, so that
 * a late response is dropped rather than answering a later request. */
void zpt_scw_session_cancel(struct zpt_scw_session *session);

/* Outcome of the most recently completed request: 0 or a negative errno,
 * with the echoed value in *value. */
int zpt_scw_session_result(const struct zpt_scw_session *session, uint16_t *value);
