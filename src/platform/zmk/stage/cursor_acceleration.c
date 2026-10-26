/* SPDX-License-Identifier: MIT */
#define DT_DRV_COMPAT measlesbagel_zpt_stage_cursor_acceleration

#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/sys/util.h>

#include <zmk/pointing_tools/platform/zmk/stage_provider.h>

#include "provider_define.h"
#include <zmk/pointing_tools/stage/cursor_acceleration.h>

struct zpt_cursor_acceleration_provider_config {
    const char *stable_id;
    const char *devicetree_path;
    struct zpt_cursor_acceleration_config stage;
};

struct zpt_cursor_acceleration_provider_data {
    struct zpt_zmk_stage_provider_data provider;
    struct zpt_cursor_acceleration_config stage;
    struct zpt_cursor_acceleration_state state;
#if defined(CONFIG_ZMK_POINTING_TOOLS_RUNTIME_TUNING)
    struct zpt_zmk_stage_tuning tuning;
#endif
};

ZPT_STAGE_PROVIDER_INIT_TUNABLE(cursor_acceleration, &zpt_cursor_acceleration_stage_api,
                                &zpt_cursor_acceleration_tuning_ops, &data->state)
#define ZPT_CURSOR_ACCELERATION_PROVIDER_DEFINE(inst)                                              \
    BUILD_ASSERT(DT_INST_PROP(inst, fast_speed_mm_per_s) >                                         \
                     DT_INST_PROP(inst, slow_speed_mm_per_s),                                      \
                 "fast-speed-mm-per-s must exceed slow-speed-mm-per-s");                           \
    BUILD_ASSERT(DT_INST_PROP(inst, fast_speed_mm_per_s) <= UINT16_MAX,                            \
                 "fast-speed-mm-per-s must fit in 16 bits");                                       \
    BUILD_ASSERT(DT_INST_PROP(inst, slow_gain_percent) > 0 &&                                      \
                     DT_INST_PROP(inst, slow_gain_percent) <= UINT16_MAX,                          \
                 "slow-gain-percent must fit in 16 bits and be positive");                         \
    BUILD_ASSERT(DT_INST_PROP(inst, fast_gain_percent) > 0 &&                                      \
                     DT_INST_PROP(inst, fast_gain_percent) <= UINT16_MAX,                          \
                 "fast-gain-percent must fit in 16 bits and be positive");                         \
    BUILD_ASSERT(DT_INST_PROP(inst, speed_window_ms) > 0 &&                                        \
                     DT_INST_PROP(inst, speed_window_ms) <= UINT16_MAX,                            \
                 "speed-window-ms must fit in 16 bits and be positive");                           \
    static struct zpt_cursor_acceleration_provider_data                                            \
        zpt_cursor_acceleration_provider_data_##inst;                                              \
    static const struct zpt_cursor_acceleration_provider_config                                    \
        zpt_cursor_acceleration_provider_config_##inst = {                                         \
            .stable_id = DT_INST_PROP(inst, stable_id),                                            \
            .devicetree_path = DT_NODE_PATH(DT_DRV_INST(inst)),                                    \
            .stage =                                                                               \
                {                                                                                  \
                    .slow_speed =                                                                  \
                        (zpt_fixed_t)DT_INST_PROP(inst, slow_speed_mm_per_s) * ZPT_FIXED_ONE,      \
                    .fast_speed =                                                                  \
                        (zpt_fixed_t)DT_INST_PROP(inst, fast_speed_mm_per_s) * ZPT_FIXED_ONE,      \
                    .slow_gain_percent = DT_INST_PROP(inst, slow_gain_percent),                    \
                    .fast_gain_percent = DT_INST_PROP(inst, fast_gain_percent),                    \
                    .speed_window_ms = DT_INST_PROP(inst, speed_window_ms),                        \
                },                                                                                 \
    };                                                                                             \
    ZPT_STAGE_PROVIDER_DEVICE_DEFINE(inst, cursor_acceleration)

DT_INST_FOREACH_STATUS_OKAY(ZPT_CURSOR_ACCELERATION_PROVIDER_DEFINE)
