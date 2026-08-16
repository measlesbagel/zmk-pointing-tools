/* SPDX-License-Identifier: MIT */
#define DT_DRV_COMPAT measlesbagel_zpt_pipeline_telemetry

#if defined(CONFIG_ZMK_POINTING_TOOLS_STATE_TELEMETRY)

#include <errno.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <zmk/pointing_tools/observer/state.h>
#include <zmk/pointing_tools/platform/zmk/pipeline_provider.h>
#include <zmk/pointing_tools/platform/zmk/pipeline_telemetry.h>
#include <zmk/pointing_tools/service/tuning.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

struct zpt_pipeline_telemetry_config {
    const struct device *const *pipeline_devices;
    size_t pipeline_count;
};

struct zpt_pipeline_telemetry_entry {
    const struct zpt_stage *stage;
    uint8_t target_id;
};

struct zpt_pipeline_telemetry_data {
    struct zpt_pipeline_telemetry_entry *entries;
    size_t entry_count;
};

static void stage_observer(const struct zpt_stage *stage, enum zpt_stage_event event, int64_t value,
                           uint32_t now_ms, void *user_data) {
    (void)stage;
    struct zpt_pipeline_telemetry_entry *entry = user_data;

    uint8_t flags = 0;
    switch (event) {
    case ZPT_STAGE_EVENT_SUPPRESSED:
        flags = ZPT_STATE_FLAG_SUPPRESSED;
        break;
    case ZPT_STAGE_EVENT_DISCARDED:
        flags = ZPT_STATE_FLAG_DISCARDED;
        break;
    case ZPT_STAGE_EVENT_QUALIFIED:
        flags = ZPT_STATE_FLAG_QUALIFIED;
        break;
    case ZPT_STAGE_EVENT_INTENT_CHANGED:
        flags = ZPT_STATE_FLAG_INTENT_CHANGED;
        break;
    case ZPT_STAGE_EVENT_FLUSHED:
    case ZPT_STAGE_EVENT_ACTION:
        flags = ZPT_STATE_FLAG_OUTPUT;
        break;
    }

    struct zpt_state_sample sample = {
        .timestamp_ms = now_ms,
        .target_id = entry->target_id,
        .target_kind = ZPT_TUNING_TARGET_PIPELINE_STAGE,
        .event = event == ZPT_STAGE_EVENT_FLUSHED ? ZPT_STATE_EVENT_FLUSH : ZPT_STATE_EVENT_FRAME,
        .flags = flags,
        .values = {(int32_t)event, (int32_t)value},
    };
    zpt_state_telemetry_submit(&sample);
}

static int lookup_stage(const struct device *dev, const char *stable_id,
                        const struct zpt_stage **stage) {
    struct zpt_pipeline_telemetry_data *data = dev->data;
    if (stable_id == NULL || stage == NULL) {
        return -EINVAL;
    }
    for (size_t index = 0; index < data->entry_count; index++) {
        if (strcmp(data->entries[index].stage->stable_id, stable_id) == 0) {
            *stage = data->entries[index].stage;
            return 0;
        }
    }
    return -ENOENT;
}

static DEVICE_API(zpt_pipeline_telemetry, pipeline_telemetry_api) = {
    .lookup_stage = lookup_stage,
};

int zpt_zmk_pipeline_telemetry_lookup(const struct device *dev, const char *stable_id,
                                      const struct zpt_stage **stage) {
    if (stable_id == NULL || stage == NULL) {
        return -EINVAL;
    }
    if (dev == NULL || !device_is_ready(dev)) {
        return -ENODEV;
    }
    if (!DEVICE_API_IS(zpt_pipeline_telemetry, dev)) {
        return -EPROTOTYPE;
    }
    const struct zpt_pipeline_telemetry_driver_api *api =
        DEVICE_API_GET(zpt_pipeline_telemetry, dev);
    return api->lookup_stage == NULL ? -ENOSYS : api->lookup_stage(dev, stable_id, stage);
}

static int pipeline_telemetry_init(const struct device *dev) {
    const struct zpt_pipeline_telemetry_config *config = dev->config;
    struct zpt_pipeline_telemetry_data *data = dev->data;

    for (size_t pipeline_index = 0; pipeline_index < config->pipeline_count; pipeline_index++) {
        struct zpt_pipeline *pipeline;
        int ret =
            zpt_zmk_pipeline_provider_get(config->pipeline_devices[pipeline_index], &pipeline);
        if (ret < 0) {
            LOG_ERR("Failed to resolve telemetry pipeline %u: %d", (unsigned int)pipeline_index,
                    ret);
            return ret;
        }
        for (size_t stage_index = 0; stage_index < pipeline->stage_count; stage_index++) {
            struct zpt_stage *stage = pipeline->stages[stage_index];
            uint8_t target_id;
            ret = zpt_state_telemetry_register_target(&target_id);
            if (ret < 0) {
                LOG_ERR("Telemetry target table exhausted while observing stage %s of pipeline %u",
                        stage->stable_id, (unsigned int)pipeline_index);
                return ret;
            }
            data->entries[data->entry_count] = (struct zpt_pipeline_telemetry_entry){
                .stage = stage,
                .target_id = target_id,
            };
            stage->observer = (struct zpt_stage_observer){
                .callback = stage_observer,
                .user_data = &data->entries[data->entry_count],
            };
            data->entry_count++;
        }
    }
    return 0;
}

#define ZPT_TELEMETRY_STAGE_COUNT(node_id, prop, index)                                            \
    +DT_PROP_LEN(DT_PHANDLE_BY_IDX(node_id, prop, index), stages)

#define ZPT_TELEMETRY_TOTAL_STAGES(node_id)                                                        \
    (0 DT_INST_FOREACH_PROP_ELEM(node_id, pipelines, ZPT_TELEMETRY_STAGE_COUNT))

#define ZPT_PIPELINE_TELEMETRY_DEFINE(inst)                                                        \
    static struct zpt_pipeline_telemetry_entry                                                     \
        zpt_pipeline_telemetry_entries_##inst[ZPT_TELEMETRY_TOTAL_STAGES(inst)];                   \
    static struct zpt_pipeline_telemetry_data zpt_pipeline_telemetry_data_##inst = {               \
        .entries = zpt_pipeline_telemetry_entries_##inst,                                          \
    };                                                                                             \
    static const struct device *const zpt_pipeline_telemetry_devices_##inst[] = {                  \
        DT_INST_FOREACH_PROP_ELEM_SEP(inst, pipelines, ZPT_PIPELINE_TELEMETRY_DEVICE, (, ))};      \
    static const struct zpt_pipeline_telemetry_config zpt_pipeline_telemetry_config_##inst = {     \
        .pipeline_devices = zpt_pipeline_telemetry_devices_##inst,                                 \
        .pipeline_count = ARRAY_SIZE(zpt_pipeline_telemetry_devices_##inst),                       \
    };                                                                                             \
    DEVICE_DT_INST_DEFINE(inst, pipeline_telemetry_init, NULL,                                     \
                          &zpt_pipeline_telemetry_data_##inst,                                     \
                          &zpt_pipeline_telemetry_config_##inst, POST_KERNEL,                      \
                          CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &pipeline_telemetry_api);

#define ZPT_PIPELINE_TELEMETRY_DEVICE(node_id, prop, index)                                        \
    DEVICE_DT_GET(DT_PHANDLE_BY_IDX(node_id, prop, index))

DT_INST_FOREACH_STATUS_OKAY(ZPT_PIPELINE_TELEMETRY_DEFINE)

#endif /* defined(CONFIG_ZMK_POINTING_TOOLS_STATE_TELEMETRY) */
