/* SPDX-License-Identifier: MIT */
#define DT_DRV_COMPAT measlesbagel_zpt_stage_cursor_quantizer

#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/sys/util.h>

#include <zmk/pointing_tools/platform/zmk/stage_provider.h>

#include "provider_define.h"
#include <zmk/pointing_tools/stage/cursor_quantizer.h>

struct zpt_cursor_quantizer_provider_config {
    const char *stable_id;
};

struct zpt_cursor_quantizer_provider_data {
    struct zpt_zmk_stage_provider_data provider;
    struct zpt_cursor_quantizer_state state;
};

ZPT_STAGE_PROVIDER_INIT_SIMPLE(cursor_quantizer, &zpt_cursor_quantizer_stage_api, NULL,
                               &data->state)
#define ZPT_CURSOR_QUANTIZER_PROVIDER_DEFINE(inst)                                                 \
    static struct zpt_cursor_quantizer_provider_data zpt_cursor_quantizer_provider_data_##inst;    \
    static const struct zpt_cursor_quantizer_provider_config                                       \
        zpt_cursor_quantizer_provider_config_##inst = {                                            \
            .stable_id = DT_INST_PROP(inst, stable_id),                                            \
    };                                                                                             \
    ZPT_STAGE_PROVIDER_DEVICE_DEFINE(inst, cursor_quantizer)

DT_INST_FOREACH_STATUS_OKAY(ZPT_CURSOR_QUANTIZER_PROVIDER_DEFINE)
