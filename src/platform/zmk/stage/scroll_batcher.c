/* SPDX-License-Identifier: MIT */
#define DT_DRV_COMPAT measlesbagel_zpt_stage_scroll_batcher

#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/sys/util.h>

#include <zmk/pointing_tools/platform/zmk/keypress_suppression.h>
#include <zmk/pointing_tools/platform/zmk/stage_provider.h>

#include "provider_define.h"
#include <zmk/pointing_tools/policy/suppression.h>
#include <zmk/pointing_tools/stage/scroll_batcher.h>

struct zpt_scroll_batcher_provider_config {
    const char *stable_id;
    struct zpt_scroll_batcher_config stage;
    const struct device *suppression_device;
};

struct zpt_scroll_batcher_provider_data {
    struct zpt_zmk_stage_provider_data provider;
    struct zpt_scroll_batcher_config stage;
    struct zpt_scroll_batcher_state state;
};

ZPT_STAGE_PROVIDER_INIT_WITH_SUPPRESSION(scroll_batcher, &zpt_scroll_batcher_stage_api)
#define ZPT_SCROLL_BATCHER_PROVIDER_DEFINE(inst)                                                   \
    BUILD_ASSERT(DT_INST_PROP(inst, steps_per_meter) > 0, "steps-per-meter must be positive");     \
    BUILD_ASSERT(DT_INST_PROP(inst, report_interval_ms) > 0 &&                                     \
                     DT_INST_PROP(inst, report_interval_ms) <= UINT16_MAX,                         \
                 "report-interval-ms must fit in 16 bits and be positive");                        \
    static struct zpt_scroll_batcher_provider_data zpt_scroll_batcher_provider_data_##inst;        \
    static const struct zpt_scroll_batcher_provider_config                                         \
        zpt_scroll_batcher_provider_config_##inst = {                                              \
            .stable_id = DT_INST_PROP(inst, stable_id),                                            \
            .stage =                                                                               \
                {                                                                                  \
                    .steps_per_millimeter = ZPT_PER_METER_TO_FIXED_PER_MILLIMETER(                 \
                        DT_INST_PROP(inst, steps_per_meter)),                                      \
                    .report_interval_ms = DT_INST_PROP(inst, report_interval_ms),                  \
                },                                                                                 \
            .suppression_device =                                                                  \
                COND_CODE_1(DT_INST_NODE_HAS_PROP(inst, suppression),                              \
                            (DEVICE_DT_GET(DT_INST_PHANDLE(inst, suppression))), (NULL)),          \
    };                                                                                             \
    ZPT_STAGE_PROVIDER_DEVICE_DEFINE(inst, scroll_batcher)

DT_INST_FOREACH_STATUS_OKAY(ZPT_SCROLL_BATCHER_PROVIDER_DEFINE)
