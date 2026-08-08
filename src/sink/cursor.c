/* SPDX-License-Identifier: MIT */

#include <errno.h>
#include <limits.h>

#include <zephyr/dt-bindings/input/input-event-codes.h>
#include <zephyr/input/input.h>
#include <zephyr/kernel.h>

#include <zmk/pointing_tools/sink/cursor.h>

static int cursor_sink_emit(struct zpt_sink *sink, const struct zpt_signal *signal) {
    const struct zpt_cursor_sink_config *config = sink->config;
    if (config == NULL || config->output_device == NULL ||
        !device_is_ready(config->output_device)) {
        return -ENODEV;
    }

    zpt_fixed_t fixed_x = signal->data.fixed_vector.x;
    zpt_fixed_t fixed_y = signal->data.fixed_vector.y;
    if (fixed_x % ZPT_FIXED_ONE != 0 || fixed_y % ZPT_FIXED_ONE != 0) {
        return -EDOM;
    }

    int64_t x = zpt_fixed_trunc_to_int(fixed_x);
    int64_t y = zpt_fixed_trunc_to_int(fixed_y);
    if (x < INT16_MIN || x > INT16_MAX || y < INT16_MIN || y > INT16_MAX) {
        return -ERANGE;
    }

    int ret = input_report(config->output_device, INPUT_EV_REL, INPUT_REL_X, (int32_t)x, false,
                           K_NO_WAIT);
    if (ret < 0) {
        return ret;
    }
    return input_report(config->output_device, INPUT_EV_REL, INPUT_REL_Y, (int32_t)y, true,
                        K_NO_WAIT);
}

const struct zpt_sink_api zpt_cursor_sink_api = {
    .type_id = "zmk-cursor",
    .accepted_kinds = ZPT_SIGNAL_KIND_MASK(ZPT_SIGNAL_POINTER_DELTA),
    .emit = cursor_sink_emit,
};
