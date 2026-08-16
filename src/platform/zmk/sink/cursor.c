/* SPDX-License-Identifier: MIT */
#define DT_DRV_COMPAT measlesbagel_zpt_sink_cursor

/*
 * ZMK output adapter for the cursor sink: converts canonical pointer deltas
 * into Zephyr input reports on the bound output device. Excluded from host
 * builds; fractional fixed-point values are rejected until a quantizer stage
 * owns rounding and remainders.
 */

#include <errno.h>
#include <limits.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/dt-bindings/input/input-event-codes.h>
#include <zephyr/input/input.h>
#include <zephyr/kernel.h>

#include <zmk/pointing_tools/platform/zmk/sink_provider.h>

struct zpt_cursor_sink_config {
    const struct device *output_device;
};

struct zpt_cursor_sink_provider_config {
    const char *stable_id;
};

struct zpt_cursor_sink_provider_data {
    struct zpt_zmk_sink_provider_data provider;
    struct zpt_cursor_sink_config sink_config;
};

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

static const struct zpt_sink_api cursor_sink_api = {
    .type_id = "zmk-cursor",
    .accepted_kinds = ZPT_SIGNAL_KIND_MASK(ZPT_SIGNAL_POINTER_DELTA),
    .emit = cursor_sink_emit,
};

static struct zpt_sink *cursor_sink_get(const struct device *dev) {
    struct zpt_cursor_sink_provider_data *data = dev->data;
    return &data->provider.sink;
}

static int cursor_sink_bind_output(const struct device *dev, const struct device *output_device) {
    struct zpt_cursor_sink_provider_data *data = dev->data;
    if (data->sink_config.output_device != NULL) {
        return -EBUSY;
    }
    data->sink_config.output_device = output_device;
    return 0;
}

static DEVICE_API(zpt_sink_provider, cursor_sink_provider_api) = {
    .get_sink = cursor_sink_get,
    .bind_output = cursor_sink_bind_output,
};

static int cursor_sink_provider_init(const struct device *dev) {
    const struct zpt_cursor_sink_provider_config *config = dev->config;
    struct zpt_cursor_sink_provider_data *data = dev->data;
    return zpt_zmk_sink_provider_init(dev, config->stable_id, &cursor_sink_api, &data->sink_config,
                                      NULL);
}

#define ZPT_CURSOR_SINK_PROVIDER_DEFINE(inst)                                                      \
    static struct zpt_cursor_sink_provider_data zpt_cursor_sink_provider_data_##inst;              \
    static const struct zpt_cursor_sink_provider_config zpt_cursor_sink_provider_config_##inst = { \
        .stable_id = DT_INST_PROP(inst, stable_id),                                                \
    };                                                                                             \
    DEVICE_DT_INST_DEFINE(inst, cursor_sink_provider_init, NULL,                                   \
                          &zpt_cursor_sink_provider_data_##inst,                                   \
                          &zpt_cursor_sink_provider_config_##inst, POST_KERNEL,                    \
                          CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &cursor_sink_provider_api);

DT_INST_FOREACH_STATUS_OKAY(ZPT_CURSOR_SINK_PROVIDER_DEFINE)
