/* SPDX-License-Identifier: MIT */
#define DT_DRV_COMPAT measlesbagel_zpt_stage_coherent_displacement

#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/sys/util.h>

#include <zmk/pointing_tools/platform/zmk/stage_provider.h>
#include <zmk/pointing_tools/stage/motion_gate/coherent_displacement.h>

struct zpt_coherent_displacement_provider_config {
    const char *stable_id;
    struct zpt_coherent_displacement_stage_config stage;
};

struct zpt_coherent_displacement_provider_data {
    struct zpt_zmk_stage_provider_data provider;
    struct zpt_coherent_displacement_stage_state state;
};

static int coherent_displacement_provider_init(const struct device *dev) {
    const struct zpt_coherent_displacement_provider_config *config = dev->config;
    struct zpt_coherent_displacement_provider_data *data = dev->data;
    return zpt_zmk_stage_provider_init(dev, config->stable_id, &zpt_coherent_displacement_stage_api,
                                       &config->stage, &data->state);
}

#define ZPT_COHERENT_DISPLACEMENT_PROVIDER_DEFINE(inst)                                            \
    BUILD_ASSERT(DT_INST_PROP(inst, activation_distance_micrometers) > 0,                          \
                 "activation-distance-micrometers must be positive");                              \
    BUILD_ASSERT(DT_INST_PROP(inst, coherence_percent) <= 100,                                     \
                 "coherence-percent cannot exceed 100");                                           \
    BUILD_ASSERT(DT_INST_PROP(inst, qualification_timeout_ms) > 0 &&                               \
                     DT_INST_PROP(inst, qualification_timeout_ms) <= UINT16_MAX,                   \
                 "qualification-timeout-ms must fit in 16 bits and be positive");                  \
    BUILD_ASSERT(DT_INST_PROP(inst, idle_timeout_ms) > 0 &&                                        \
                     DT_INST_PROP(inst, idle_timeout_ms) <= UINT16_MAX,                            \
                 "idle-timeout-ms must fit in 16 bits and be positive");                           \
    static struct zpt_coherent_displacement_provider_data                                          \
        zpt_coherent_displacement_provider_data_##inst;                                            \
    static const struct zpt_coherent_displacement_provider_config                                  \
        zpt_coherent_displacement_provider_config_##inst = {                                       \
            .stable_id = DT_INST_PROP(inst, stable_id),                                            \
            .stage =                                                                               \
                {                                                                                  \
                    .settings =                                                                    \
                        {                                                                          \
                            .enabled = DT_INST_PROP(inst, enabled),                                \
                            .activation_distance = ZPT_MICROMETERS_TO_FIXED_MILLIMETERS(           \
                                DT_INST_PROP(inst, activation_distance_micrometers)),              \
                            .coherence_percent = DT_INST_PROP(inst, coherence_percent),            \
                            .qualification_timeout_ms =                                            \
                                DT_INST_PROP(inst, qualification_timeout_ms),                      \
                            .idle_timeout_ms = DT_INST_PROP(inst, idle_timeout_ms),                \
                        },                                                                         \
                },                                                                                 \
    };                                                                                             \
    DEVICE_DT_INST_DEFINE(inst, coherent_displacement_provider_init, NULL,                         \
                          &zpt_coherent_displacement_provider_data_##inst,                         \
                          &zpt_coherent_displacement_provider_config_##inst, POST_KERNEL,          \
                          CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &zpt_stage_provider_api);

DT_INST_FOREACH_STATUS_OKAY(ZPT_COHERENT_DISPLACEMENT_PROVIDER_DEFINE)
