/* SPDX-License-Identifier: MIT */
#define DT_DRV_COMPAT measlesbagel_zpt_stage_text_nav

#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/sys/util.h>

#include <zmk/pointing_tools/platform/zmk/stage_provider.h>

#include "provider_define.h"
#include <zmk/pointing_tools/stage/text_nav.h>

struct zpt_text_nav_provider_config {
    const char *stable_id;
    struct zpt_text_nav_config stage;
};

struct zpt_text_nav_provider_data {
    struct zpt_zmk_stage_provider_data provider;
    struct zpt_text_nav_state state;
};

ZPT_STAGE_PROVIDER_INIT_SIMPLE(text_nav, &zpt_text_nav_stage_api, &config->stage, &data->state)
#define ZPT_TEXT_NAV_PROVIDER_DEFINE(inst)                                                         \
    BUILD_ASSERT(DT_INST_PROP(inst, horizontal_threshold_micrometers) > 0,                         \
                 "horizontal-threshold-micrometers must be positive");                             \
    BUILD_ASSERT(DT_INST_PROP(inst, vertical_threshold_micrometers) > 0,                           \
                 "vertical-threshold-micrometers must be positive");                               \
    BUILD_ASSERT(DT_INST_PROP(inst, idle_timeout_ms) > 0 &&                                        \
                     DT_INST_PROP(inst, idle_timeout_ms) <= UINT16_MAX,                            \
                 "idle-timeout-ms must fit in 16 bits and be positive");                           \
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
            },                                                                                     \
    };                                                                                             \
    ZPT_STAGE_PROVIDER_DEVICE_DEFINE(inst, text_nav)

DT_INST_FOREACH_STATUS_OKAY(ZPT_TEXT_NAV_PROVIDER_DEFINE)
