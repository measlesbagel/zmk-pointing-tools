/* SPDX-License-Identifier: MIT */
#define DT_DRV_COMPAT measlesbagel_zpt_stage_cursor_coalescer

#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/sys/util.h>

#include <zmk/pointing_tools/platform/zmk/stage_provider.h>

#include "provider_define.h"
#include <zmk/pointing_tools/stage/cursor_coalescer.h>

struct zpt_cursor_coalescer_provider_config {
    const char *stable_id;
    const char *devicetree_path;
    struct zpt_cursor_coalescer_config stage;
};

struct zpt_cursor_coalescer_provider_data {
    struct zpt_zmk_stage_provider_data provider;
    struct zpt_cursor_coalescer_config stage;
    struct zpt_cursor_coalescer_state state;
#if defined(CONFIG_ZMK_POINTING_TOOLS_RUNTIME_TUNING)
    struct zpt_zmk_stage_tuning tuning;
#endif
};

ZPT_STAGE_PROVIDER_INIT_TUNABLE(cursor_coalescer, &zpt_cursor_coalescer_stage_api,
                                &zpt_cursor_coalescer_tuning_ops, &data->state)
#define ZPT_CURSOR_COALESCER_PROVIDER_DEFINE(inst)                                                 \
    BUILD_ASSERT(DT_INST_PROP(inst, report_interval_ms) <= UINT16_MAX,                             \
                 "report-interval-ms must fit in 16 bits");                                        \
    static struct zpt_cursor_coalescer_provider_data zpt_cursor_coalescer_provider_data_##inst;    \
    static const struct zpt_cursor_coalescer_provider_config                                       \
        zpt_cursor_coalescer_provider_config_##inst = {                                            \
            .stable_id = DT_INST_PROP(inst, stable_id),                                            \
            .devicetree_path = DT_NODE_PATH(DT_DRV_INST(inst)),                                    \
            .stage =                                                                               \
                {                                                                                  \
                    .report_interval_ms = DT_INST_PROP(inst, report_interval_ms),                  \
                },                                                                                 \
    };                                                                                             \
    ZPT_STAGE_PROVIDER_DEVICE_DEFINE(inst, cursor_coalescer)

DT_INST_FOREACH_STATUS_OKAY(ZPT_CURSOR_COALESCER_PROVIDER_DEFINE)
