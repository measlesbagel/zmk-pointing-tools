/* SPDX-License-Identifier: MIT */

#include <errno.h>
#include <limits.h>

#include <zmk/pointing_tools/pointer_identity.h>

static bool count_fits_fixed(int64_t value) {
    return value >= INT64_MIN / ZPT_FIXED_ONE && value <= INT64_MAX / ZPT_FIXED_ONE;
}

static int raw_pointer_identity_process(struct zpt_stage *stage, const struct zpt_signal *signal,
                                        struct zpt_stage_context *context) {
    (void)stage;

    if (!count_fits_fixed(signal->data.raw_motion.x_counts) ||
        !count_fits_fixed(signal->data.raw_motion.y_counts)) {
        return -ERANGE;
    }

    struct zpt_signal output = *signal;
    output.kind = ZPT_SIGNAL_POINTER_DELTA;
    output.data.fixed_vector.x = signal->data.raw_motion.x_counts * ZPT_FIXED_ONE;
    output.data.fixed_vector.y = signal->data.raw_motion.y_counts * ZPT_FIXED_ONE;
    return zpt_stage_emit(context, &output);
}

const struct zpt_stage_api zpt_raw_pointer_identity_stage_api = {
    .strategy_id = "raw-pointer-identity",
    .accepted_kinds = ZPT_SIGNAL_KIND_MASK(ZPT_SIGNAL_RAW_MOTION),
    .output_kind = ZPT_SIGNAL_POINTER_DELTA,
    .process = raw_pointer_identity_process,
};
