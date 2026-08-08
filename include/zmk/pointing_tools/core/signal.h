/* SPDX-License-Identifier: MIT */
#pragma once

#include <stdint.h>

/* Canonical typed values exchanged by the composable pipeline runtime. */

#define ZPT_FIXED_FRACTION_BITS 16U
#define ZPT_FIXED_ONE (INT64_C(1) << ZPT_FIXED_FRACTION_BITS)

typedef int64_t zpt_fixed_t;

enum zpt_signal_kind {
    ZPT_SIGNAL_INVALID = 0,
    ZPT_SIGNAL_RAW_MOTION,
    ZPT_SIGNAL_NORMALIZED_MOTION,
    ZPT_SIGNAL_POINTER_DELTA,
    ZPT_SIGNAL_SCROLL_DELTA,
    ZPT_SIGNAL_SCROLL_STEPS,
    ZPT_SIGNAL_ACTION,
    ZPT_SIGNAL_KIND_COUNT,
};

_Static_assert(ZPT_SIGNAL_KIND_COUNT < 32, "signal kinds must fit the 32-bit kind mask");

#define ZPT_SIGNAL_KIND_MASK(kind) (UINT32_C(1) << (kind))

enum zpt_signal_flag {
    ZPT_SIGNAL_FLAG_LOCAL = UINT32_C(1) << 0,
    ZPT_SIGNAL_FLAG_TRANSPORTED = UINT32_C(1) << 1,
    ZPT_SIGNAL_FLAG_COALESCED = UINT32_C(1) << 2,
    ZPT_SIGNAL_FLAG_TIMING_ESTIMATED = UINT32_C(1) << 3,
    ZPT_SIGNAL_FLAG_CLIPPED = UINT32_C(1) << 4,
    ZPT_SIGNAL_FLAG_SEQUENCE_GAP = UINT32_C(1) << 5,
    ZPT_SIGNAL_FLAG_MALFORMED = UINT32_C(1) << 6,
    ZPT_SIGNAL_FLAG_DISCONTINUITY = UINT32_C(1) << 7,
};

enum zpt_signal_axis_intent {
    ZPT_SIGNAL_AXIS_UNDECIDED = 0,
    ZPT_SIGNAL_AXIS_FREE,
    ZPT_SIGNAL_AXIS_HORIZONTAL,
    ZPT_SIGNAL_AXIS_VERTICAL,
};

struct zpt_signal_metadata {
    uint32_t observed_at_ms;
    uint32_t sample_span_us;
    uint32_t flags;
    uint16_t source_id;
    uint16_t sequence;
    uint16_t resolution_cpi;
};

struct zpt_signal_annotations {
    zpt_fixed_t speed_per_second;
    uint16_t axis_confidence_percent;
    uint8_t axis_intent;
    uint8_t reserved;
};

struct zpt_raw_motion {
    int64_t x_counts;
    int64_t y_counts;
};

struct zpt_fixed_vector {
    zpt_fixed_t x;
    zpt_fixed_t y;
};

struct zpt_step_vector {
    int32_t x;
    int32_t y;
};

struct zpt_action {
    uint32_t id;
    int32_t value;
    uint32_t duration_ms;
};

struct zpt_signal {
    enum zpt_signal_kind kind;
    struct zpt_signal_metadata metadata;
    struct zpt_signal_annotations annotations;
    union {
        struct zpt_raw_motion raw_motion;
        struct zpt_fixed_vector fixed_vector;
        struct zpt_step_vector steps;
        struct zpt_action action;
    } data;
};

_Static_assert(sizeof(struct zpt_signal) <= 64, "signals must remain cheap to copy and buffer");

static inline int zpt_signal_kind_valid(enum zpt_signal_kind kind) {
    return kind > ZPT_SIGNAL_INVALID && kind < ZPT_SIGNAL_KIND_COUNT;
}

static inline zpt_fixed_t zpt_fixed_from_int(int32_t value) {
    return (zpt_fixed_t)value * ZPT_FIXED_ONE;
}

static inline int64_t zpt_fixed_trunc_to_int(zpt_fixed_t value) { return value / ZPT_FIXED_ONE; }
