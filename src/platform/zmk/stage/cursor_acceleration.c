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
#define ZPT_CURSOR_ACCELERATION_SPEED(inst, point)                                                 \
    DT_INST_PROP_BY_IDX(inst, curve_speeds_mm_per_s, point)
#define ZPT_CURSOR_ACCELERATION_PERCENT(inst, point)                                               \
    DT_INST_PROP_BY_IDX(inst, curve_sensitivity_percent, point)
#define ZPT_CURSOR_ACCELERATION_ELEMENT(node_id, prop, idx) DT_PROP_BY_IDX(node_id, prop, idx),

#define ZPT_CURSOR_ACCELERATION_PROVIDER_DEFINE(inst)                                              \
    BUILD_ASSERT(                                                                                  \
        DT_INST_PROP_LEN(inst, curve_speeds_mm_per_s) == ZPT_CURSOR_ACCELERATION_POINTS &&         \
            DT_INST_PROP_LEN(inst, curve_sensitivity_percent) == ZPT_CURSOR_ACCELERATION_POINTS,   \
        "curve-speeds-mm-per-s and curve-sensitivity-percent need 8 points each");                 \
    BUILD_ASSERT(ZPT_CURSOR_ACCELERATION_SPEED(inst, 0) >= 0 &&                                    \
                     ZPT_CURSOR_ACCELERATION_SPEED(inst, 7) <=                                     \
                         ZPT_CURSOR_ACCELERATION_MAX_SPEED_MM_PER_S,                               \
                 "curve-speeds-mm-per-s must lie from 0 to 10000");                                \
    BUILD_ASSERT(                                                                                  \
        ZPT_CURSOR_ACCELERATION_SPEED(inst, 0) < ZPT_CURSOR_ACCELERATION_SPEED(inst, 1) &&         \
            ZPT_CURSOR_ACCELERATION_SPEED(inst, 1) < ZPT_CURSOR_ACCELERATION_SPEED(inst, 2) &&     \
            ZPT_CURSOR_ACCELERATION_SPEED(inst, 2) < ZPT_CURSOR_ACCELERATION_SPEED(inst, 3) &&     \
            ZPT_CURSOR_ACCELERATION_SPEED(inst, 3) < ZPT_CURSOR_ACCELERATION_SPEED(inst, 4) &&     \
            ZPT_CURSOR_ACCELERATION_SPEED(inst, 4) < ZPT_CURSOR_ACCELERATION_SPEED(inst, 5) &&     \
            ZPT_CURSOR_ACCELERATION_SPEED(inst, 5) < ZPT_CURSOR_ACCELERATION_SPEED(inst, 6) &&     \
            ZPT_CURSOR_ACCELERATION_SPEED(inst, 6) < ZPT_CURSOR_ACCELERATION_SPEED(inst, 7),       \
        "curve-speeds-mm-per-s must strictly increase");                                           \
    BUILD_ASSERT(IN_RANGE(ZPT_CURSOR_ACCELERATION_PERCENT(inst, 0), 1,                             \
                          ZPT_CURSOR_ACCELERATION_MAX_PERCENT) &&                                  \
                     IN_RANGE(ZPT_CURSOR_ACCELERATION_PERCENT(inst, 1), 1,                         \
                              ZPT_CURSOR_ACCELERATION_MAX_PERCENT) &&                              \
                     IN_RANGE(ZPT_CURSOR_ACCELERATION_PERCENT(inst, 2), 1,                         \
                              ZPT_CURSOR_ACCELERATION_MAX_PERCENT) &&                              \
                     IN_RANGE(ZPT_CURSOR_ACCELERATION_PERCENT(inst, 3), 1,                         \
                              ZPT_CURSOR_ACCELERATION_MAX_PERCENT) &&                              \
                     IN_RANGE(ZPT_CURSOR_ACCELERATION_PERCENT(inst, 4), 1,                         \
                              ZPT_CURSOR_ACCELERATION_MAX_PERCENT) &&                              \
                     IN_RANGE(ZPT_CURSOR_ACCELERATION_PERCENT(inst, 5), 1,                         \
                              ZPT_CURSOR_ACCELERATION_MAX_PERCENT) &&                              \
                     IN_RANGE(ZPT_CURSOR_ACCELERATION_PERCENT(inst, 6), 1,                         \
                              ZPT_CURSOR_ACCELERATION_MAX_PERCENT) &&                              \
                     IN_RANGE(ZPT_CURSOR_ACCELERATION_PERCENT(inst, 7), 1,                         \
                              ZPT_CURSOR_ACCELERATION_MAX_PERCENT),                                \
                 "curve-sensitivity-percent values must be from 1 to 1000");                       \
    BUILD_ASSERT(IN_RANGE(DT_INST_PROP(inst, speed_rise_ms), 1,                                    \
                          ZPT_CURSOR_ACCELERATION_MAX_TIME_CONSTANT_MS) &&                         \
                     IN_RANGE(DT_INST_PROP(inst, speed_fall_ms), 1,                                \
                              ZPT_CURSOR_ACCELERATION_MAX_TIME_CONSTANT_MS),                       \
                 "speed-rise-ms and speed-fall-ms must be from 1 to 200");                         \
    static struct zpt_cursor_acceleration_provider_data                                            \
        zpt_cursor_acceleration_provider_data_##inst;                                              \
    static const struct zpt_cursor_acceleration_provider_config                                    \
        zpt_cursor_acceleration_provider_config_##inst = {                                         \
            .stable_id = DT_INST_PROP(inst, stable_id),                                            \
            .devicetree_path = DT_NODE_PATH(DT_DRV_INST(inst)),                                    \
            .stage =                                                                               \
                {                                                                                  \
                    .curve_speed_mm_per_s = {DT_INST_FOREACH_PROP_ELEM(                            \
                        inst, curve_speeds_mm_per_s, ZPT_CURSOR_ACCELERATION_ELEMENT)},            \
                    .curve_percent = {DT_INST_FOREACH_PROP_ELEM(inst, curve_sensitivity_percent,   \
                                                                ZPT_CURSOR_ACCELERATION_ELEMENT)}, \
                    .speed_rise_ms = DT_INST_PROP(inst, speed_rise_ms),                            \
                    .speed_fall_ms = DT_INST_PROP(inst, speed_fall_ms),                            \
                },                                                                                 \
    };                                                                                             \
    ZPT_STAGE_PROVIDER_DEVICE_DEFINE(inst, cursor_acceleration)

DT_INST_FOREACH_STATUS_OKAY(ZPT_CURSOR_ACCELERATION_PROVIDER_DEFINE)
