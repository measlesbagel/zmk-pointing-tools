/* SPDX-License-Identifier: MIT */
#define DT_DRV_COMPAT measlesbagel_zpt_stage_orientation

#include <zephyr/device.h>
#include <zephyr/devicetree.h>

#include <zmk/pointing_tools/platform/zmk/stage_provider.h>

#include "provider_define.h"
#include <zmk/pointing_tools/stage/orientation.h>

struct zpt_orientation_provider_config {
    const char *stable_id;
    struct zpt_orientation_config stage;
};

/* The init function is referenced by ZPT_STAGE_PROVIDER_DEVICE_DEFINE only
 * when the devicetree contains an orientation instance, so it reads as
 * unused in configurations without one. */
// NOLINTNEXTLINE(clang-diagnostic-unused-function)
ZPT_STAGE_PROVIDER_INIT_SIMPLE(orientation, &zpt_orthogonal_orientation_stage_api, &config->stage,
                               NULL)
#define ZPT_ORIENTATION_PROVIDER_DEFINE(inst)                                                      \
    static struct zpt_zmk_stage_provider_data zpt_orientation_provider_data_##inst;                \
    static const struct zpt_orientation_provider_config zpt_orientation_provider_config_##inst = { \
        .stable_id = DT_INST_PROP(inst, stable_id),                                                \
        .stage =                                                                                   \
            {                                                                                      \
                .swap_xy = DT_INST_PROP(inst, swap_xy),                                            \
                .invert_x = DT_INST_PROP(inst, invert_x),                                          \
                .invert_y = DT_INST_PROP(inst, invert_y),                                          \
            },                                                                                     \
    };                                                                                             \
    ZPT_STAGE_PROVIDER_DEVICE_DEFINE(inst, orientation)

DT_INST_FOREACH_STATUS_OKAY(ZPT_ORIENTATION_PROVIDER_DEFINE)
