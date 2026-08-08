/* SPDX-License-Identifier: MIT */
#define DT_DRV_COMPAT measlesbagel_zpt_sink_cursor

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>

#include <zmk/pointing_tools/platform/zmk/sink_provider.h>
#include <zmk/pointing_tools/sink/cursor.h>

struct zpt_cursor_sink_provider_config {
    const char *stable_id;
};

struct zpt_cursor_sink_provider_data {
    struct zpt_zmk_sink_provider_data provider;
    struct zpt_cursor_sink_config sink_config;
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
    return zpt_zmk_sink_provider_init(dev, config->stable_id, &zpt_cursor_sink_api,
                                      &data->sink_config, NULL);
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
