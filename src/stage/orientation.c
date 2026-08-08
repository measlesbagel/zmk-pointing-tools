/* SPDX-License-Identifier: MIT */

#include <errno.h>
#include <limits.h>
#include <stddef.h>

#include <zmk/pointing_tools/stage/orientation.h>

static int invert_if_requested(int64_t value, bool invert, int64_t *output) {
    if (invert && value == INT64_MIN) {
        return -ERANGE;
    }
    *output = invert ? -value : value;
    return 0;
}

int zpt_orientation_apply(const struct zpt_orientation_config *config,
                          const struct zpt_raw_motion *input, struct zpt_raw_motion *output) {
    if (config == NULL || input == NULL || output == NULL) {
        return -EINVAL;
    }

    int64_t x = config->swap_xy ? input->y_counts : input->x_counts;
    int64_t y = config->swap_xy ? input->x_counts : input->y_counts;
    struct zpt_raw_motion candidate;
    int ret = invert_if_requested(x, config->invert_x, &candidate.x_counts);
    if (ret < 0) {
        return ret;
    }
    ret = invert_if_requested(y, config->invert_y, &candidate.y_counts);
    if (ret < 0) {
        return ret;
    }
    *output = candidate;
    return 0;
}

static int orthogonal_orientation_process(struct zpt_stage *stage, const struct zpt_signal *signal,
                                          struct zpt_stage_context *context) {
    struct zpt_signal output = *signal;
    int ret =
        zpt_orientation_apply(stage->config, &signal->data.raw_motion, &output.data.raw_motion);
    if (ret < 0) {
        return ret;
    }
    return zpt_stage_emit(context, &output);
}

const struct zpt_stage_api zpt_orthogonal_orientation_stage_api = {
    .strategy_id = "orthogonal-orientation",
    .accepted_kinds = ZPT_SIGNAL_KIND_MASK(ZPT_SIGNAL_RAW_MOTION),
    .output_kind = ZPT_SIGNAL_RAW_MOTION,
    .process = orthogonal_orientation_process,
};
