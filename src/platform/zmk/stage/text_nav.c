/* SPDX-License-Identifier: MIT */
#define DT_DRV_COMPAT measlesbagel_zpt_stage_text_nav

#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/sys/util.h>

#include <zmk/pointing_tools/platform/zmk/stage_provider.h>
#include <zmk/pointing_tools/stage/text_nav.h>

struct zpt_text_nav_provider_config {
    const char *stable_id;
    struct zpt_text_nav_config stage;
};

struct zpt_text_nav_provider_data {
    struct zpt_zmk_stage_provider_data provider;
    struct zpt_text_nav_state state;
};

static int text_nav_provider_init(const struct device *dev) {
    const struct zpt_text_nav_provider_config *config = dev->config;
    struct zpt_text_nav_provider_data *data = dev->data;
    return zpt_zmk_stage_provider_init(dev, config->stable_id, &zpt_text_nav_stage_api,
                                       &config->stage, &data->state);
}

#define ZPT_TEXT_NAV_PROVIDER_DEFINE(inst)                                                         \
    BUILD_ASSERT(DT_INST_PROP(inst, horizontal_threshold_micrometers) > 0,                         \
                 "horizontal-threshold-micrometers must be positive");                             \
    BUILD_ASSERT(DT_INST_PROP(inst, vertical_threshold_micrometers) > 0,                           \
                 "vertical-threshold-micrometers must be positive");                               \
    BUILD_ASSERT(DT_INST_PROP(inst, idle_timeout_ms) > 0 &&                                        \
                     DT_INST_PROP(inst, idle_timeout_ms) <= UINT16_MAX,                            \
                 "idle-timeout-ms must fit in 16 bits and be positive");                           \
    BUILD_ASSERT(DT_INST_PROP(inst, activation_distance_micrometers) > 0,                          \
                 "activation-distance-micrometers must be positive");                              \
    BUILD_ASSERT(DT_INST_PROP(inst, engage_ratio_percent) > 0 &&                                   \
                     DT_INST_PROP(inst, engage_ratio_percent) <= UINT16_MAX,                       \
                 "engage-ratio-percent must fit in 16 bits and be positive");                      \
    static struct zpt_text_nav_provider_data zpt_text_nav_provider_data_##inst;                    \
    static const struct zpt_text_nav_provider_config zpt_text_nav_provider_config_##inst = {       \
        .stable_id = DT_INST_PROP(inst, stable_id),                                                \
        .stage =                                                                                   \
            {                                                                                      \
                .horizontal_threshold = ZPT_MICROMETERS_TO_FIXED_MILLIMETERS(                      \
                    DT_INST_PROP(inst, horizontal_threshold_micrometers)),                         \
                .vertical_threshold = ZPT_MICROMETERS_TO_FIXED_MILLIMETERS(                        \
                    DT_INST_PROP(inst, vertical_threshold_micrometers)),                           \
                .idle_timeout_ms = DT_INST_PROP(inst, idle_timeout_ms),                            \
                .activation_distance = ZPT_MICROMETERS_TO_FIXED_MILLIMETERS(                       \
                    DT_INST_PROP(inst, activation_distance_micrometers)),                          \
                .engage_ratio_percent = DT_INST_PROP(inst, engage_ratio_percent),                  \
            },                                                                                     \
    };                                                                                             \
    DEVICE_DT_INST_DEFINE(inst, text_nav_provider_init, NULL, &zpt_text_nav_provider_data_##inst,  \
                          &zpt_text_nav_provider_config_##inst, POST_KERNEL,                       \
                          CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &zpt_stage_provider_api);

DT_INST_FOREACH_STATUS_OKAY(ZPT_TEXT_NAV_PROVIDER_DEFINE)
