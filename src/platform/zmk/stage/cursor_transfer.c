/* SPDX-License-Identifier: MIT */
#define DT_DRV_COMPAT measlesbagel_zpt_stage_cursor_transfer

#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/sys/util.h>

#include <zmk/pointing_tools/platform/zmk/stage_provider.h>

#include "provider_define.h"
#include <zmk/pointing_tools/stage/cursor_transfer.h>

struct zpt_cursor_transfer_provider_config {
    const char *stable_id;
    struct zpt_cursor_transfer_config stage;
};

ZPT_STAGE_PROVIDER_INIT_SIMPLE(cursor_transfer, &zpt_cursor_transfer_stage_api, &config->stage,
                               NULL)
#define ZPT_CURSOR_TRANSFER_PROVIDER_DEFINE(inst)                                                  \
    BUILD_ASSERT(DT_INST_PROP(inst, scale_multiplier) <= UINT16_MAX,                               \
                 "scale-multiplier must fit in 16 bits");                                          \
    BUILD_ASSERT(DT_INST_PROP(inst, scale_divisor) > 0 &&                                          \
                     DT_INST_PROP(inst, scale_divisor) <= UINT16_MAX,                              \
                 "scale-divisor must fit in 16 bits and be positive");                             \
    static struct zpt_zmk_stage_provider_data zpt_cursor_transfer_provider_data_##inst;            \
    static const struct zpt_cursor_transfer_provider_config                                        \
        zpt_cursor_transfer_provider_config_##inst = {                                             \
            .stable_id = DT_INST_PROP(inst, stable_id),                                            \
            .stage =                                                                               \
                {                                                                                  \
                    .scale_multiplier = DT_INST_PROP(inst, scale_multiplier),                      \
                    .scale_divisor = DT_INST_PROP(inst, scale_divisor),                            \
                },                                                                                 \
    };                                                                                             \
    ZPT_STAGE_PROVIDER_DEVICE_DEFINE(inst, cursor_transfer)

DT_INST_FOREACH_STATUS_OKAY(ZPT_CURSOR_TRANSFER_PROVIDER_DEFINE)
