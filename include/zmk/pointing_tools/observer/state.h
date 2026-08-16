/* SPDX-License-Identifier: MIT */
#pragma once

#include <stdint.h>

/* Semantic state records consumed by observer and telemetry integrations. */

#define ZPT_STATE_VALUE_COUNT 10
#define ZPT_STATE_ALL_TARGETS UINT8_MAX

enum zpt_state_level {
    ZPT_STATE_LEVEL_OFF = 0,
    ZPT_STATE_LEVEL_DECISIONS = 1,
    ZPT_STATE_LEVEL_VERBOSE = 2,
};

enum zpt_state_event {
    ZPT_STATE_EVENT_FRAME = 1,
    ZPT_STATE_EVENT_FLUSH = 2,
};

enum zpt_state_flag {
    ZPT_STATE_FLAG_INTENT_CHANGED = 1 << 0,
    ZPT_STATE_FLAG_SUPPRESSED = 1 << 1,
    ZPT_STATE_FLAG_DISCARDED = 1 << 2,
    ZPT_STATE_FLAG_OUTPUT = 1 << 3,
    ZPT_STATE_FLAG_QUALIFIED = 1 << 4,
};

struct zpt_state_sample {
    uint32_t timestamp_ms;
    uint32_t sequence;
    uint8_t target_id;
    uint8_t target_kind;
    uint8_t event;
    uint8_t intent;
    uint16_t flags;
    int32_t values[ZPT_STATE_VALUE_COUNT];
};

enum zpt_state_level zpt_state_telemetry_level(uint8_t target_id);
void zpt_state_telemetry_submit(const struct zpt_state_sample *sample);

/* Allocate the next free state-telemetry target id for non-tunable
 * observers (pipeline stages); returns -ENOSPC when exhausted. */
int zpt_state_telemetry_register_target(uint8_t *target_id);
