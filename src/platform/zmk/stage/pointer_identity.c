/* SPDX-License-Identifier: MIT */
#define DT_DRV_COMPAT measlesbagel_zpt_stage_raw_pointer_identity

#include <zephyr/device.h>
#include <zephyr/devicetree.h>

#include <zmk/pointing_tools/platform/zmk/stage_provider.h>
#include <zmk/pointing_tools/stage/pointer_identity.h>

struct zpt_pointer_identity_provider_config {
    const char *stable_id;
};

static int pointer_identity_provider_init(const struct device *dev) {
    const struct zpt_pointer_identity_provider_config *config = dev->config;
    return zpt_zmk_stage_provider_init(dev, config->stable_id, &zpt_raw_pointer_identity_stage_api,
                                       NULL, NULL);
}

#define ZPT_POINTER_IDENTITY_PROVIDER_DEFINE(inst)                                                 \
    static struct zpt_zmk_stage_provider_data zpt_pointer_identity_provider_data_##inst;           \
    static const struct zpt_pointer_identity_provider_config                                       \
        zpt_pointer_identity_provider_config_##inst = {                                            \
            .stable_id = DT_INST_PROP(inst, stable_id),                                            \
    };                                                                                             \
    DEVICE_DT_INST_DEFINE(inst, pointer_identity_provider_init, NULL,                              \
                          &zpt_pointer_identity_provider_data_##inst,                              \
                          &zpt_pointer_identity_provider_config_##inst, POST_KERNEL,               \
                          CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &zpt_stage_provider_api);

DT_INST_FOREACH_STATUS_OKAY(ZPT_POINTER_IDENTITY_PROVIDER_DEFINE)
