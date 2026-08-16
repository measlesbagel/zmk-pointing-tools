/* SPDX-License-Identifier: MIT */
#pragma once

/* Private macros for the devicetree-backed stage providers. Each per-compat
 * file keeps its own config/data structs, BUILD_ASSERTs, and config
 * initializer; these macros supply the shared pieces:
 *
 *  - ZPT_STAGE_PROVIDER_INIT_* is invoked once per file and expands to the
 *    provider init function. The simple variant points
 *    zpt_zmk_stage_provider_init at member expressions evaluated inside the
 *    generated function, so callers pass e.g. &config->stage or NULL. The
 *    suppression variant copies the stage config into device data first,
 *    resolves the optional keypress-suppression policy into it, and
 *    registers that writable copy.
 *  - ZPT_STAGE_PROVIDER_DEVICE_DEFINE is invoked per devicetree instance
 *    (inside the per-compat DEFINE macro) and expands to the
 *    DEVICE_DT_INST_DEFINE shared by every provider.
 */

#include <zephyr/device.h>
#include <zephyr/devicetree.h>

#include <zmk/pointing_tools/platform/zmk/keypress_suppression.h>
#include <zmk/pointing_tools/platform/zmk/stage_provider.h>
#include <zmk/pointing_tools/policy/suppression.h>

#define ZPT_STAGE_PROVIDER_INIT_SIMPLE(prefix, stage_api, config_member, state_member)             \
    static int zpt_##prefix##_provider_init(const struct device *dev) {                            \
        const struct zpt_##prefix##_provider_config *config = dev->config;                         \
        struct zpt_##prefix##_provider_data *data = dev->data;                                     \
        (void)data;                                                                                \
        return zpt_zmk_stage_provider_init(dev, config->stable_id, (stage_api), config_member,     \
                                           state_member);                                          \
    }

#define ZPT_STAGE_PROVIDER_INIT_WITH_SUPPRESSION(prefix, stage_api)                                \
    static int zpt_##prefix##_provider_init(const struct device *dev) {                            \
        const struct zpt_##prefix##_provider_config *config = dev->config;                         \
        struct zpt_##prefix##_provider_data *data = dev->data;                                     \
        data->stage = config->stage;                                                               \
        if (config->suppression_device != NULL) {                                                  \
            int ret = zpt_zmk_keypress_suppression_get(config->suppression_device,                 \
                                                       &data->stage.suppression);                  \
            if (ret < 0) {                                                                         \
                return ret;                                                                        \
            }                                                                                      \
        }                                                                                          \
        return zpt_zmk_stage_provider_init(dev, config->stable_id, (stage_api), &data->stage,      \
                                           &data->state);                                          \
    }

#define ZPT_STAGE_PROVIDER_DEVICE_DEFINE(inst, prefix)                                             \
    DEVICE_DT_INST_DEFINE(inst, zpt_##prefix##_provider_init, NULL,                                \
                          &zpt_##prefix##_provider_data_##inst,                                    \
                          &zpt_##prefix##_provider_config_##inst, POST_KERNEL,                     \
                          CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &zpt_stage_provider_api)
