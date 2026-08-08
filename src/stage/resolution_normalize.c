/* SPDX-License-Identifier: MIT */

#include <errno.h>
#include <limits.h>
#include <stddef.h>

#include <zmk/pointing_tools/stage/resolution_normalize.h>

#define ZPT_TENTHS_OF_MILLIMETERS_PER_INCH 254

static int64_t divide_round_nearest(int64_t numerator, int64_t denominator) {
    int64_t quotient = numerator / denominator;
    int64_t remainder = numerator % denominator;
    int64_t threshold = (denominator + 1) / 2;
    if (remainder >= threshold) {
        quotient++;
    } else if (remainder <= -threshold) {
        quotient--;
    }
    return quotient;
}

int zpt_counts_to_millimeters(int64_t counts, uint16_t resolution_cpi, zpt_fixed_t *millimeters) {
    if (resolution_cpi == 0U || millimeters == NULL) {
        return -EINVAL;
    }

    const int64_t factor = ZPT_TENTHS_OF_MILLIMETERS_PER_INCH * ZPT_FIXED_ONE;
    if (counts > INT64_MAX / factor || counts < INT64_MIN / factor) {
        return -ERANGE;
    }

    const int64_t denominator = (int64_t)resolution_cpi * 10;
    *millimeters = divide_round_nearest(counts * factor, denominator);
    return 0;
}

static int resolution_normalize_process(struct zpt_stage *stage, const struct zpt_signal *signal,
                                        struct zpt_stage_context *context) {
    (void)stage;

    struct zpt_signal output = *signal;
    zpt_fixed_t x;
    zpt_fixed_t y;
    int ret = zpt_counts_to_millimeters(signal->data.raw_motion.x_counts,
                                        signal->metadata.resolution_cpi, &x);
    if (ret < 0) {
        return ret;
    }
    ret = zpt_counts_to_millimeters(signal->data.raw_motion.y_counts,
                                    signal->metadata.resolution_cpi, &y);
    if (ret < 0) {
        return ret;
    }

    output.kind = ZPT_SIGNAL_NORMALIZED_MOTION;
    output.data.fixed_vector.x = x;
    output.data.fixed_vector.y = y;
    return zpt_stage_emit(context, &output);
}

const struct zpt_stage_api zpt_resolution_normalize_stage_api = {
    .strategy_id = "resolution-normalize-mm",
    .accepted_kinds = ZPT_SIGNAL_KIND_MASK(ZPT_SIGNAL_RAW_MOTION),
    .output_kind = ZPT_SIGNAL_NORMALIZED_MOTION,
    .process = resolution_normalize_process,
};
