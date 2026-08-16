/* SPDX-License-Identifier: MIT */
#define DT_DRV_COMPAT measlesbagel_zpt_pipeline

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/sys/util.h>

#include <zmk/pointing_tools/platform/zmk/pipeline_provider.h>
#include <zmk/pointing_tools/platform/zmk/sink_provider.h>
#include <zmk/pointing_tools/platform/zmk/stage_provider.h>

struct zpt_pipeline_provider_config {
    const char *stable_id;
    const struct device *const *stage_devices;
    size_t stage_count;
    const struct device *sink_device;
    uint32_t dispatch_budget;
};

struct zpt_pipeline_provider_data {
    struct zpt_stage **stages;
    struct zpt_sink *sink;
    struct zpt_pipeline pipeline;
    bool prepared;
};

static int pipeline_provider_init(const struct device *dev) {
    const struct zpt_pipeline_provider_config *config = dev->config;
    struct zpt_pipeline_provider_data *data = dev->data;

    for (size_t index = 0; index < config->stage_count; index++) {
        int ret = zpt_zmk_stage_provider_get(config->stage_devices[index], &data->stages[index]);
        if (ret < 0) {
            return ret;
        }
    }
    int ret = zpt_zmk_sink_provider_get(config->sink_device, &data->sink);
    if (ret < 0) {
        return ret;
    }

    data->pipeline = (struct zpt_pipeline){
        .stable_id = config->stable_id,
        .input_kind = ZPT_SIGNAL_RAW_MOTION,
        .stages = data->stages,
        .stage_count = config->stage_count,
        .sink = data->sink,
        .dispatch_budget = config->dispatch_budget,
    };
    return 0;
}

static int pipeline_provider_prepare(const struct device *dev, const struct device *output_device,
                                     struct zpt_pipeline **pipeline) {
    struct zpt_pipeline_provider_data *data = dev->data;
    const struct zpt_pipeline_provider_config *config = dev->config;
    if (data->prepared) {
        return -EBUSY;
    }
    int ret = zpt_zmk_sink_provider_bind_output(config->sink_device, output_device);
    if (ret < 0) {
        return ret;
    }
    data->prepared = true;
    *pipeline = &data->pipeline;
    return 0;
}

static int pipeline_provider_get(const struct device *dev, struct zpt_pipeline **pipeline) {
    if (pipeline == NULL) {
        return -EINVAL;
    }
    struct zpt_pipeline_provider_data *data = dev->data;
    if (data->pipeline.stable_id == NULL) {
        return -EAGAIN;
    }
    *pipeline = &data->pipeline;
    return 0;
}

static DEVICE_API(zpt_pipeline_provider, pipeline_provider_api) = {
    .prepare = pipeline_provider_prepare,
    .get = pipeline_provider_get,
};

int zpt_zmk_pipeline_provider_prepare(const struct device *dev, const struct device *output_device,
                                      struct zpt_pipeline **pipeline) {
    if (dev == NULL || output_device == NULL || pipeline == NULL) {
        return -EINVAL;
    }
    if (!device_is_ready(dev)) {
        return -ENODEV;
    }
    if (!DEVICE_API_IS(zpt_pipeline_provider, dev)) {
        return -EPROTOTYPE;
    }
    const struct zpt_pipeline_provider_driver_api *api = DEVICE_API_GET(zpt_pipeline_provider, dev);
    return api->prepare == NULL ? -ENOSYS : api->prepare(dev, output_device, pipeline);
}

int zpt_zmk_pipeline_provider_get(const struct device *dev, struct zpt_pipeline **pipeline) {
    if (pipeline == NULL) {
        return -EINVAL;
    }
    if (!device_is_ready(dev)) {
        return -ENODEV;
    }
    if (!DEVICE_API_IS(zpt_pipeline_provider, dev)) {
        return -EPROTOTYPE;
    }
    const struct zpt_pipeline_provider_driver_api *api = DEVICE_API_GET(zpt_pipeline_provider, dev);
    return api->get == NULL ? -ENOSYS : api->get(dev, pipeline);
}

#define ZPT_PIPELINE_STAGE_DEVICE(node_id, prop, index)                                            \
    DEVICE_DT_GET(DT_PHANDLE_BY_IDX(node_id, prop, index))

#define ZPT_PIPELINE_PROVIDER_DEFINE(inst)                                                         \
    BUILD_ASSERT(DT_INST_PROP_LEN(inst, stages) > 0, "pipeline requires at least one stage");      \
    static const struct device *const zpt_pipeline_stage_devices_##inst[] = {                      \
        DT_INST_FOREACH_PROP_ELEM_SEP(inst, stages, ZPT_PIPELINE_STAGE_DEVICE, (, ))};             \
    static struct zpt_stage *zpt_pipeline_stages_##inst[DT_INST_PROP_LEN(inst, stages)];           \
    static struct zpt_pipeline_provider_data zpt_pipeline_provider_data_##inst = {                 \
        .stages = zpt_pipeline_stages_##inst,                                                      \
    };                                                                                             \
    static const struct zpt_pipeline_provider_config zpt_pipeline_provider_config_##inst = {       \
        .stable_id = DT_INST_PROP(inst, stable_id),                                                \
        .stage_devices = zpt_pipeline_stage_devices_##inst,                                        \
        .stage_count = ARRAY_SIZE(zpt_pipeline_stage_devices_##inst),                              \
        .sink_device = DEVICE_DT_GET(DT_INST_PHANDLE(inst, sink)),                                 \
        .dispatch_budget = DT_INST_PROP(inst, dispatch_budget),                                    \
    };                                                                                             \
    DEVICE_DT_INST_DEFINE(inst, pipeline_provider_init, NULL, &zpt_pipeline_provider_data_##inst,  \
                          &zpt_pipeline_provider_config_##inst, POST_KERNEL,                       \
                          CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &pipeline_provider_api);

DT_INST_FOREACH_STATUS_OKAY(ZPT_PIPELINE_PROVIDER_DEFINE)
