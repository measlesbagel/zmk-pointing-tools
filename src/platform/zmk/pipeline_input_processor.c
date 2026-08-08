/* SPDX-License-Identifier: MIT */
#define DT_DRV_COMPAT measlesbagel_zpt_input_processor_pipeline

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/dt-bindings/input/input-event-codes.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <drivers/input_processor.h>

#include <zmk/pointing_tools/platform/zmk/pipeline_executor.h>
#include <zmk/pointing_tools/platform/zmk/stage_provider.h>
#include <zmk/pointing_tools/sink/cursor.h>
#include <zmk/pointing_tools/source/motion_source.h>
#include <zmk/pointing_tools/source/transport/compact_split_codec.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

struct zpt_pipeline_processor_config {
    const char *stable_id;
    struct zpt_motion_source_config source;
    const struct device *const *stage_devices;
    size_t stage_count;
    uint32_t dispatch_budget;
    uint16_t compact_event_code;
    bool compact_transport;
    bool compact_transport_coalesced;
};

struct zpt_pipeline_processor_data {
    struct k_spinlock frame_lock;
    struct zpt_motion_source_state source;
    struct zpt_compact_split_decoder compact_decoder;
    struct zpt_cursor_sink_config sink_config;
    struct zpt_stage **stages;
    struct zpt_sink sink;
    struct zpt_pipeline pipeline;
    struct zpt_zmk_pipeline_executor executor;
};

static int zpt_pipeline_processor_init(const struct device *dev) {
    const struct zpt_pipeline_processor_config *config = dev->config;
    struct zpt_pipeline_processor_data *data = dev->data;

    int ret = zpt_motion_source_init(&data->source, &config->source);
    if (ret < 0) {
        LOG_ERR("Failed to initialize motion source for %s: %d", config->stable_id, ret);
        return ret;
    }
    zpt_compact_split_decoder_init(&data->compact_decoder);

    for (size_t index = 0; index < config->stage_count; index++) {
        ret = zpt_zmk_stage_provider_get(config->stage_devices[index], &data->stages[index]);
        if (ret < 0) {
            LOG_ERR("Failed to resolve stage %u for %s: %d", (unsigned int)index, config->stable_id,
                    ret);
            return ret;
        }
    }

    data->sink_config.output_device = dev;
    data->sink = (struct zpt_sink){
        .stable_id = "cursor",
        .api = &zpt_cursor_sink_api,
        .config = &data->sink_config,
    };
    data->pipeline = (struct zpt_pipeline){
        .stable_id = config->stable_id,
        .input_kind = ZPT_SIGNAL_RAW_MOTION,
        .stages = data->stages,
        .stage_count = config->stage_count,
        .sink = &data->sink,
        .dispatch_budget = config->dispatch_budget,
    };

    ret = zpt_zmk_pipeline_executor_init(&data->executor, &data->pipeline);
    if (ret < 0) {
        LOG_ERR("Failed to validate motion pipeline %s: %d", config->stable_id, ret);
        return ret;
    }
    ret = zpt_zmk_pipeline_executor_activate(&data->executor, ZPT_RESET_PIPELINE_ENTERED);
    if (ret < 0) {
        LOG_ERR("Failed to activate motion pipeline %s: %d", config->stable_id, ret);
    }
    return ret;
}

static int zpt_pipeline_processor_handle_event(const struct device *dev, struct input_event *event,
                                               uint32_t param1, uint32_t param2,
                                               struct zmk_input_processor_state *state) {
    ARG_UNUSED(param1);
    ARG_UNUSED(param2);
    ARG_UNUSED(state);

    const struct zpt_pipeline_processor_config *config = dev->config;
    struct zpt_pipeline_processor_data *data = dev->data;
    struct zpt_signal signal;
    bool frame_ready = false;
    uint32_t now = event->sync ? k_uptime_get_32() : 0U;

    k_spinlock_key_t key = k_spin_lock(&data->frame_lock);
    if (config->compact_transport && event->type == INPUT_EV_REL &&
        event->code == config->compact_event_code) {
        struct zpt_compact_split_frame frame;
        now = k_uptime_get_32();
        int ret = zpt_compact_split_decode(&data->compact_decoder, (uint32_t)event->value, &frame);
        event->value = 0;
        if (ret == 0) {
            zpt_motion_source_add(&data->source, ZPT_MOTION_AXIS_X, frame.motion.x_counts);
            zpt_motion_source_add(&data->source, ZPT_MOTION_AXIS_Y, frame.motion.y_counts);
            uint32_t flags = frame.flags;
            if (config->compact_transport_coalesced) {
                flags |= ZPT_SIGNAL_FLAG_COALESCED;
            }
            frame_ready = zpt_motion_source_take_at_sequence(
                &data->source, now, frame.sample_span_us, frame.sequence, flags, &signal);
        } else {
            LOG_ERR("Motion pipeline %s rejected compact split packet: %d", config->stable_id, ret);
        }
    } else if (event->type == INPUT_EV_REL && event->code == INPUT_REL_X) {
        zpt_motion_source_add(&data->source, ZPT_MOTION_AXIS_X, event->value);
        event->value = 0;
    } else if (event->type == INPUT_EV_REL && event->code == INPUT_REL_Y) {
        zpt_motion_source_add(&data->source, ZPT_MOTION_AXIS_Y, event->value);
        event->value = 0;
    }
    if (event->sync) {
        frame_ready = zpt_motion_source_take(&data->source, now, 0U, 0U, &signal);
    }
    k_spin_unlock(&data->frame_lock, key);

    if (!frame_ready) {
        return ZMK_INPUT_PROC_CONTINUE;
    }

    struct zpt_pipeline_result result;
    int ret = zpt_zmk_pipeline_executor_push(&data->executor, &signal, &result);
    if (ret < 0) {
        LOG_ERR("Motion pipeline %s rejected frame: %d", config->stable_id, ret);
        return ret;
    }

    return ZMK_INPUT_PROC_CONTINUE;
}

static const struct zmk_input_processor_driver_api zpt_pipeline_processor_driver_api = {
    .handle_event = zpt_pipeline_processor_handle_event,
};

#define ZPT_PIPELINE_PROCESSOR_DEFINE(inst)                                                        \
    BUILD_ASSERT(DT_INST_PROP_LEN(inst, stages) > 0, "pipeline requires at least one stage");      \
    BUILD_ASSERT(DT_INST_PROP(inst, source_id) >= 0, "source-id cannot be negative");              \
    BUILD_ASSERT(DT_INST_PROP(inst, source_id) <= UINT16_MAX, "source-id must fit in 16 bits");    \
    BUILD_ASSERT(DT_INST_PROP(inst, resolution_cpi) > 0, "resolution-cpi must be positive");       \
    BUILD_ASSERT(DT_INST_PROP(inst, resolution_cpi) <= UINT16_MAX,                                 \
                 "resolution-cpi must fit in 16 bits");                                            \
    BUILD_ASSERT(!DT_INST_NODE_HAS_PROP(inst, compact_event_code) ||                               \
                     DT_INST_PROP(inst, transported),                                              \
                 "compact-event-code requires a transported source");                              \
    BUILD_ASSERT(!DT_INST_NODE_HAS_PROP(inst, compact_event_code) ||                               \
                     (DT_INST_PROP_OR(inst, compact_event_code, 0) >= 0 &&                         \
                      DT_INST_PROP_OR(inst, compact_event_code, 0) <= UINT16_MAX),                 \
                 "compact-event-code must fit in 16 bits");                                        \
    BUILD_ASSERT(!DT_INST_NODE_HAS_PROP(inst, compact_event_code) ||                               \
                     (DT_INST_PROP_OR(inst, compact_event_code, 0) != INPUT_REL_X &&               \
                      DT_INST_PROP_OR(inst, compact_event_code, 0) != INPUT_REL_Y),                \
                 "compact-event-code must not overlap a native axis");                             \
    static const struct device *const zpt_pipeline_stage_devices_##inst[] = {                      \
        DT_INST_FOREACH_PROP_ELEM_SEP(inst, stages, ZPT_PIPELINE_STAGE_DEVICE, (, ))};             \
    static struct zpt_stage *zpt_pipeline_stages_##inst[DT_INST_PROP_LEN(inst, stages)];           \
    static struct zpt_pipeline_processor_data zpt_pipeline_processor_data_##inst = {               \
        .stages = zpt_pipeline_stages_##inst,                                                      \
    };                                                                                             \
    static const struct zpt_pipeline_processor_config zpt_pipeline_processor_config_##inst = {     \
        .stable_id = DT_INST_PROP(inst, stable_id),                                                \
        .source =                                                                                  \
            {                                                                                      \
                .flags = COND_CODE_1(DT_INST_PROP(inst, transported),                              \
                                     (ZPT_SIGNAL_FLAG_TRANSPORTED), (ZPT_SIGNAL_FLAG_LOCAL)),      \
                .source_id = DT_INST_PROP(inst, source_id),                                        \
                .resolution_cpi = DT_INST_PROP(inst, resolution_cpi),                              \
            },                                                                                     \
        .stage_devices = zpt_pipeline_stage_devices_##inst,                                        \
        .stage_count = ARRAY_SIZE(zpt_pipeline_stage_devices_##inst),                              \
        .dispatch_budget = DT_INST_PROP(inst, dispatch_budget),                                    \
        .compact_event_code = DT_INST_PROP_OR(inst, compact_event_code, 0),                        \
        .compact_transport = DT_INST_NODE_HAS_PROP(inst, compact_event_code),                      \
        .compact_transport_coalesced = DT_INST_PROP(inst, compact_transport_coalesced),            \
    };                                                                                             \
    DEVICE_DT_INST_DEFINE(                                                                         \
        inst, zpt_pipeline_processor_init, NULL, &zpt_pipeline_processor_data_##inst,              \
        &zpt_pipeline_processor_config_##inst, POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,   \
        &zpt_pipeline_processor_driver_api);

#define ZPT_PIPELINE_STAGE_DEVICE(node_id, prop, index)                                            \
    DEVICE_DT_GET(DT_PHANDLE_BY_IDX(node_id, prop, index))

DT_INST_FOREACH_STATUS_OKAY(ZPT_PIPELINE_PROCESSOR_DEFINE)
