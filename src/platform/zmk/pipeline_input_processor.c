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

#include <zmk/pointing_tools/sink/cursor.h>
#include <zmk/pointing_tools/source/frame_assembler.h>
#include <zmk/pointing_tools/stage/pointer_identity.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

struct zpt_pipeline_processor_config {
    const char *stable_id;
    uint32_t source_flags;
    uint16_t source_id;
};

struct zpt_pipeline_processor_data {
    struct k_spinlock frame_lock;
    struct zpt_frame_assembler frame;
    uint16_t sequence;
    struct zpt_cursor_sink_config sink_config;
    struct zpt_stage stages[1];
    struct zpt_sink sink;
    struct zpt_pipeline pipeline;
};

static int zpt_pipeline_processor_init(const struct device *dev) {
    const struct zpt_pipeline_processor_config *config = dev->config;
    struct zpt_pipeline_processor_data *data = dev->data;

    data->sink_config.output_device = dev;
    data->stages[0] = (struct zpt_stage){
        .stable_id = "raw-pointer-identity",
        .api = &zpt_raw_pointer_identity_stage_api,
    };
    data->sink = (struct zpt_sink){
        .stable_id = "cursor",
        .api = &zpt_cursor_sink_api,
        .config = &data->sink_config,
    };
    data->pipeline = (struct zpt_pipeline){
        .stable_id = config->stable_id,
        .input_kind = ZPT_SIGNAL_RAW_MOTION,
        .stages = data->stages,
        .stage_count = ARRAY_SIZE(data->stages),
        .sink = &data->sink,
        .dispatch_budget = 4,
    };

    int ret = zpt_pipeline_validate(&data->pipeline);
    if (ret < 0) {
        LOG_ERR("Failed to validate motion pipeline %s: %d", config->stable_id, ret);
        return ret;
    }
    ret = zpt_pipeline_activate(&data->pipeline, ZPT_RESET_PIPELINE_ENTERED);
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
    struct zpt_raw_motion motion;
    uint32_t frame_flags;
    uint16_t sequence = 0;
    bool frame_ready = false;

    k_spinlock_key_t key = k_spin_lock(&data->frame_lock);
    if (event->type == INPUT_EV_REL && event->code == INPUT_REL_X) {
        zpt_frame_assembler_add(&data->frame, ZPT_MOTION_AXIS_X, event->value);
        event->value = 0;
    } else if (event->type == INPUT_EV_REL && event->code == INPUT_REL_Y) {
        zpt_frame_assembler_add(&data->frame, ZPT_MOTION_AXIS_Y, event->value);
        event->value = 0;
    }
    if (event->sync) {
        frame_ready = zpt_frame_assembler_take(&data->frame, &motion, &frame_flags);
        if (frame_ready) {
            sequence = data->sequence++;
        }
    }
    k_spin_unlock(&data->frame_lock, key);

    if (!frame_ready) {
        return ZMK_INPUT_PROC_CONTINUE;
    }

    const struct zpt_signal signal = {
        .kind = ZPT_SIGNAL_RAW_MOTION,
        .metadata =
            {
                .observed_at_ms = k_uptime_get_32(),
                .flags = config->source_flags | frame_flags,
                .source_id = config->source_id,
                .sequence = sequence,
            },
        .annotations =
            {
                .axis_intent = ZPT_SIGNAL_AXIS_UNDECIDED,
            },
        .data.raw_motion = motion,
    };
    struct zpt_pipeline_result result;
    int ret = zpt_pipeline_push(&data->pipeline, &signal, &result);
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
    BUILD_ASSERT(DT_INST_PROP(inst, source_id) >= 0, "source-id cannot be negative");              \
    BUILD_ASSERT(DT_INST_PROP(inst, source_id) <= UINT16_MAX, "source-id must fit in 16 bits");    \
    static struct zpt_pipeline_processor_data zpt_pipeline_processor_data_##inst;                  \
    static const struct zpt_pipeline_processor_config zpt_pipeline_processor_config_##inst = {     \
        .stable_id = DT_INST_PROP(inst, stable_id),                                                \
        .source_flags = COND_CODE_1(DT_INST_PROP(inst, transported),                               \
                                    (ZPT_SIGNAL_FLAG_TRANSPORTED), (ZPT_SIGNAL_FLAG_LOCAL)),       \
        .source_id = DT_INST_PROP(inst, source_id),                                                \
    };                                                                                             \
    DEVICE_DT_INST_DEFINE(                                                                         \
        inst, zpt_pipeline_processor_init, NULL, &zpt_pipeline_processor_data_##inst,              \
        &zpt_pipeline_processor_config_##inst, POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,   \
        &zpt_pipeline_processor_driver_api);

DT_INST_FOREACH_STATUS_OKAY(ZPT_PIPELINE_PROCESSOR_DEFINE)
