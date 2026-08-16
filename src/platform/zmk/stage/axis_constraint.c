/* SPDX-License-Identifier: MIT */
#define DT_DRV_COMPAT measlesbagel_zpt_stage_axis_constraint

#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/sys/util.h>

#include <zmk/pointing_tools/platform/zmk/keypress_suppression.h>
#include <zmk/pointing_tools/platform/zmk/stage_provider.h>

#include "provider_define.h"
#include <zmk/pointing_tools/policy/suppression.h>
#include <zmk/pointing_tools/stage/axis_constraint.h>

struct zpt_axis_constraint_provider_config {
    const char *stable_id;
    struct zpt_axis_constraint_config stage;
    const struct device *suppression_device;
};

struct zpt_axis_constraint_provider_data {
    struct zpt_zmk_stage_provider_data provider;
    struct zpt_axis_constraint_config stage;
    struct zpt_axis_constraint_state state;
};

ZPT_STAGE_PROVIDER_INIT_WITH_SUPPRESSION(axis_constraint, &zpt_axis_constraint_stage_api)
#define ZPT_AXIS_CONSTRAINT_PROVIDER_DEFINE(inst)                                                  \
    BUILD_ASSERT(DT_INST_PROP_OR(inst, idle_timeout_ms, 0) <= UINT16_MAX,                          \
                 "idle-timeout-ms must fit in 16 bits");                                           \
    static struct zpt_axis_constraint_provider_data zpt_axis_constraint_provider_data_##inst;      \
    static const struct zpt_axis_constraint_provider_config                                        \
        zpt_axis_constraint_provider_config_##inst = {                                             \
            .stable_id = DT_INST_PROP(inst, stable_id),                                            \
            .stage =                                                                               \
                {                                                                                  \
                    .discard_unclassified = DT_INST_PROP_OR(inst, discard_unclassified, false),    \
                    .idle_timeout_ms = DT_INST_PROP_OR(inst, idle_timeout_ms, 0),                  \
                    .fold_interval_ms = DT_INST_PROP_OR(inst, fold_interval_ms, 0),                \
                },                                                                                 \
            .suppression_device =                                                                  \
                COND_CODE_1(DT_INST_NODE_HAS_PROP(inst, suppression),                              \
                            (DEVICE_DT_GET(DT_INST_PHANDLE(inst, suppression))), (NULL)),          \
    };                                                                                             \
    ZPT_STAGE_PROVIDER_DEVICE_DEFINE(inst, axis_constraint)

DT_INST_FOREACH_STATUS_OKAY(ZPT_AXIS_CONSTRAINT_PROVIDER_DEFINE)
