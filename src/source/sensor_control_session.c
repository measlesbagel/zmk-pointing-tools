/* SPDX-License-Identifier: MIT */

#include <stddef.h>

#include <zmk/pointing_tools/source/sensor_control_session.h>
#include <zmk/pointing_tools/source/sensor_control_wire.h>

void zpt_scw_session_init(struct zpt_scw_session *session) {
    if (session != NULL) {
        *session = (struct zpt_scw_session){0};
    }
}

uint8_t zpt_scw_session_begin(struct zpt_scw_session *session) {
    session->last_seq = (uint8_t)(session->last_seq % ZPT_SCW_SEQ_MAX + 1U);
    session->expected_seq = session->last_seq;
    return session->last_seq;
}

bool zpt_scw_session_offer(struct zpt_scw_session *session, uint32_t raw) {
    uint8_t status = 0;
    uint8_t seq = 0;
    uint16_t value = 0;
    if (!zpt_scw_decode(raw, &status, &seq, &value)) {
        return false;
    }
    if (session->expected_seq == 0U || seq != session->expected_seq) {
        return false;
    }
    session->status_errno = status;
    session->value = value;
    session->expected_seq = 0U;
    return true;
}

void zpt_scw_session_cancel(struct zpt_scw_session *session) { session->expected_seq = 0U; }

int zpt_scw_session_result(const struct zpt_scw_session *session, uint16_t *value) {
    if (value != NULL) {
        *value = session->value;
    }
    return -(int)session->status_errno;
}
