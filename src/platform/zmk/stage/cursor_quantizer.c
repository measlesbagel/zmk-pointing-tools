/* SPDX-License-Identifier: MIT */
#define DT_DRV_COMPAT measlesbagel_zpt_stage_cursor_quantizer

#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/sys/util.h>

#include <zmk/pointing_tools/platform/zmk/stage_provider.h>
#include <zmk/pointing_tools/stage/cursor_quantizer.h>

struct zpt_cursor_quantizer_provider_config {
    const char *stable_id;
    struct zpt_cursor_quantizer_config stage;
};

struct zpt_cursor_quantizer_provider_data {
    struct zpt_zmk_stage_provider_data provider;
    struct zpt_cursor_quantizer_state state;
};

static int cursor_quantizer_provider_init(const struct device *dev) {
    const struct zpt_cursor_quantizer_provider_config *config = dev->config;
    struct zpt_cursor_quantizer_provider_data *data = dev->data;
    return zpt_zmk_stage_provider_init(dev, config->stable_id, &zpt_cursor_quantizer_stage_api,
                                       &config->stage, &data->state);
}

#define ZPT_CURSOR_QUANTIZER_PROVIDER_DEFINE(inst)                                                 \
    BUILD_ASSERT(DT_INST_PROP(inst, units_per_meter) > 0, "units-per-meter must be positive");     \
    static struct zpt_cursor_quantizer_provider_data zpt_cursor_quantizer_provider_data_##inst;    \
    static const struct zpt_cursor_quantizer_provider_config                                       \
        zpt_cursor_quantizer_provider_config_##inst = {                                            \
            .stable_id = DT_INST_PROP(inst, stable_id),                                            \
            .stage =                                                                               \
                {                                                                                  \
                    .units_per_millimeter = ZPT_PER_METER_TO_FIXED_PER_MILLIMETER(                 \
                        DT_INST_PROP(inst, units_per_meter)),                                      \
                },                                                                                 \
    };                                                                                             \
    DEVICE_DT_INST_DEFINE(inst, cursor_quantizer_provider_init, NULL,                              \
                          &zpt_cursor_quantizer_provider_data_##inst,                              \
                          &zpt_cursor_quantizer_provider_config_##inst, POST_KERNEL,               \
                          CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &zpt_stage_provider_api);

DT_INST_FOREACH_STATUS_OKAY(ZPT_CURSOR_QUANTIZER_PROVIDER_DEFINE)
