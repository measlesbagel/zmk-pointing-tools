/* SPDX-License-Identifier: MIT */
#define DT_DRV_COMPAT measlesbagel_zpt_sink_scroll

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/dt-bindings/input/input-event-codes.h>
#include <zephyr/input/input.h>
#include <zephyr/kernel.h>

#include <zmk/pointing_tools/platform/zmk/sink_provider.h>

struct zpt_scroll_sink_config {
    const struct device *output_device;
};

struct zpt_scroll_sink_provider_config {
    const char *stable_id;
};

struct zpt_scroll_sink_provider_data {
    struct zpt_zmk_sink_provider_data provider;
    struct zpt_scroll_sink_config sink_config;
};

static int scroll_sink_emit(struct zpt_sink *sink, const struct zpt_signal *signal) {
    if (signal->kind != ZPT_SIGNAL_SCROLL_STEPS) {
        return -EPROTOTYPE;
    }
    const struct zpt_scroll_sink_config *config = sink->config;
    if (config == NULL || config->output_device == NULL ||
        !device_is_ready(config->output_device)) {
        return -ENODEV;
    }

    int ret = input_report(config->output_device, INPUT_EV_REL, INPUT_REL_HWHEEL,
                           signal->data.steps.x, false, K_NO_WAIT);
    if (ret < 0) {
        return ret;
    }
    return input_report(config->output_device, INPUT_EV_REL, INPUT_REL_WHEEL, signal->data.steps.y,
                        true, K_NO_WAIT);
}

static const struct zpt_sink_api scroll_sink_api = {
    .type_id = "zmk-scroll",
    .accepted_kinds = ZPT_SIGNAL_KIND_MASK(ZPT_SIGNAL_SCROLL_STEPS),
    .emit = scroll_sink_emit,
};

static struct zpt_sink *scroll_sink_get(const struct device *dev) {
    struct zpt_scroll_sink_provider_data *data = dev->data;
    return &data->provider.sink;
}

static int scroll_sink_bind_output(const struct device *dev, const struct device *output_device) {
    struct zpt_scroll_sink_provider_data *data = dev->data;
    if (data->sink_config.output_device != NULL) {
        return -EBUSY;
    }
    data->sink_config.output_device = output_device;
    return 0;
}

static DEVICE_API(zpt_sink_provider, scroll_sink_provider_api) = {
    .get_sink = scroll_sink_get,
    .bind_output = scroll_sink_bind_output,
};

static int scroll_sink_provider_init(const struct device *dev) {
    const struct zpt_scroll_sink_provider_config *config = dev->config;
    struct zpt_scroll_sink_provider_data *data = dev->data;
    return zpt_zmk_sink_provider_init(dev, config->stable_id, &scroll_sink_api, &data->sink_config,
                                      NULL);
}

#define ZPT_SCROLL_SINK_PROVIDER_DEFINE(inst)                                                      \
    static struct zpt_scroll_sink_provider_data zpt_scroll_sink_provider_data_##inst;              \
    static const struct zpt_scroll_sink_provider_config zpt_scroll_sink_provider_config_##inst = { \
        .stable_id = DT_INST_PROP(inst, stable_id),                                                \
    };                                                                                             \
    DEVICE_DT_INST_DEFINE(inst, scroll_sink_provider_init, NULL,                                   \
                          &zpt_scroll_sink_provider_data_##inst,                                   \
                          &zpt_scroll_sink_provider_config_##inst, POST_KERNEL,                    \
                          CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &scroll_sink_provider_api);

DT_INST_FOREACH_STATUS_OKAY(ZPT_SCROLL_SINK_PROVIDER_DEFINE)
