/* SPDX-License-Identifier: MIT */
#pragma once

#include <limits.h>
#include <stdint.h>

#include <zmk/pointing_tools/core/signal.h>

/* Shared signed fixed-point helpers used by the transfer and quantizer
 * stages. All operations saturate instead of wrapping. */

/* INT64_MIN-safe magnitude of a signed value. */
static inline uint64_t zpt_fixed_magnitude(int64_t value) {
    if (value >= 0) {
        return (uint64_t)value;
    }
    return (uint64_t)(-(value + 1)) + 1U;
}

static inline int64_t zpt_fixed_saturating_add(int64_t left, int64_t right) {
    if (right > 0 && left > INT64_MAX - right) {
        return INT64_MAX;
    }
    if (right < 0 && left < INT64_MIN - right) {
        return INT64_MIN;
    }
    return left + right;
}

static inline uint64_t zpt_fixed_saturating_add_u64(uint64_t left, uint64_t right) {
    return UINT64_MAX - left < right ? UINT64_MAX : left + right;
}

/* major * 100 >= minor * ratio, evaluated without overflow. */
static inline bool zpt_fixed_ratio_dominates(uint64_t major, uint64_t minor,
                                             uint16_t ratio_percent) {
    if (minor != 0U && ratio_percent != 0U && minor > UINT64_MAX / ratio_percent) {
        return true;
    }
    uint64_t product = minor * ratio_percent;
    uint64_t required = product / 100U + (product % 100U != 0U ? 1U : 0U);
    return major >= required;
}

/* Q16 by Q16 multiply with saturation: (left * right) >> 16. */
static inline int64_t zpt_fixed_multiply(int64_t left, int64_t right) {
    if (left == 0 || right == 0) {
        return 0;
    }
    if (left > 0 && right > 0) {
        if (left > INT64_MAX / right) {
            return INT64_MAX;
        }
        return (left * right) >> ZPT_FIXED_FRACTION_BITS;
    }
    if (left < 0 && right < 0) {
        if (left < INT64_MAX / right) {
            return INT64_MAX;
        }
        return (left * right) >> ZPT_FIXED_FRACTION_BITS;
    }
    if (left > 0) {
        if (right < INT64_MIN / left) {
            return INT64_MIN;
        }
    } else if ((uint64_t)right > (UINT64_C(1) << 63) / zpt_fixed_magnitude(left)) {
        return INT64_MIN;
    }
    return (left * right) >> ZPT_FIXED_FRACTION_BITS;
}

/* Convert Q16 fixed-point to a clamped int32, truncating toward zero. */
static inline int32_t zpt_fixed_to_int32(int64_t value) {
    value /= ZPT_FIXED_ONE;
    return value > INT32_MAX ? INT32_MAX : (value < INT32_MIN ? INT32_MIN : (int32_t)value);
}
