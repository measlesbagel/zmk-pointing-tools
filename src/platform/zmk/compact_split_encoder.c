/* SPDX-License-Identifier: MIT */
#define DT_DRV_COMPAT measlesbagel_zpt_input_processor_compact_split_encoder

#include <errno.h>
#include <limits.h>

#include <zephyr/device.h>
#include <zephyr/dt-bindings/input/input-event-codes.h>
#include <zephyr/input/input.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <drivers/input_processor.h>

#include <zmk/pointing_tools/source/frame_assembler.h>
#include <zmk/pointing_tools/source/transport/compact_split_codec.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

struct zpt_compact_split_encoder_config {
    uint16_t event_code;
};

struct zpt_compact_split_encoder_data {
    struct k_spinlock lock;
    struct zpt_frame_assembler frame;
    struct zpt_compact_split_encoder encoder;
    uint32_t previous_frame_at_ms;
    bool saw_frame_time;
};

static uint32_t sample_span(struct zpt_compact_split_encoder_data *data, uint32_t now) {
    uint32_t span = 0U;
    if (data->saw_frame_time) {
        uint32_t elapsed_ms = now - data->previous_frame_at_ms;
        span = elapsed_ms > UINT32_MAX / 1000U ? UINT32_MAX : elapsed_ms * 1000U;
    }
    data->previous_frame_at_ms = now;
    data->saw_frame_time = true;
    return span;
}

static int zpt_compact_split_encoder_device_init(const struct device *dev) {
    struct zpt_compact_split_encoder_data *data = dev->data;
    zpt_frame_assembler_reset(&data->frame);
    zpt_compact_split_encoder_init(&data->encoder);
    return 0;
}

static int zpt_compact_split_encoder_handle_event(const struct device *dev,
                                                  struct input_event *event, uint32_t param1,
                                                  uint32_t param2,
                                                  struct zmk_input_processor_state *state) {
    ARG_UNUSED(param1);
    ARG_UNUSED(param2);
    ARG_UNUSED(state);

    const struct zpt_compact_split_encoder_config *config = dev->config;
    struct zpt_compact_split_encoder_data *data = dev->data;
    bool is_axis =
        event->type == INPUT_EV_REL && (event->code == INPUT_REL_X || event->code == INPUT_REL_Y);
    bool frame_ready = false;
    struct zpt_raw_motion motion;
    uint32_t frame_flags;
    uint32_t packet = 0U;
    int encode_result = 0;

    k_spinlock_key_t key = k_spin_lock(&data->lock);
    if (is_axis) {
        zpt_frame_assembler_add(&data->frame,
                                event->code == INPUT_REL_X ? ZPT_MOTION_AXIS_X : ZPT_MOTION_AXIS_Y,
                                event->value);
        event->value = 0;
    }
    if (event->sync) {
        frame_ready = zpt_frame_assembler_take(&data->frame, &motion, &frame_flags);
        if (frame_ready) {
            uint32_t now = k_uptime_get_32();
            encode_result =
                zpt_compact_split_encode(&data->encoder, &motion, sample_span(data, now), &packet);
        }
    }
    k_spin_unlock(&data->lock, key);

    if (!frame_ready) {
        return is_axis ? ZMK_INPUT_PROC_STOP : ZMK_INPUT_PROC_CONTINUE;
    }
    if ((frame_flags & ZPT_SIGNAL_FLAG_CLIPPED) != 0U || encode_result < 0) {
        LOG_ERR("Compact split encoder dropped an out-of-range frame");
        return ZMK_INPUT_PROC_STOP;
    }

    if (is_axis) {
        event->type = INPUT_EV_REL;
        event->code = config->event_code;
        event->value = (int32_t)packet;
        event->sync = true;
        return ZMK_INPUT_PROC_CONTINUE;
    }

    int ret = input_report(event->dev, INPUT_EV_REL, config->event_code, (int32_t)packet, true,
                           K_NO_WAIT);
    if (ret < 0) {
        LOG_ERR("Compact split encoder failed to emit packet: %d", ret);
        return ret;
    }
    return ZMK_INPUT_PROC_CONTINUE;
}

static const struct zmk_input_processor_driver_api zpt_compact_split_encoder_driver_api = {
    .handle_event = zpt_compact_split_encoder_handle_event,
};

#define ZPT_COMPACT_SPLIT_ENCODER_DEFINE(inst)                                                     \
    BUILD_ASSERT(DT_INST_PROP(inst, event_code) >= 0, "event-code cannot be negative");            \
    BUILD_ASSERT(DT_INST_PROP(inst, event_code) <= UINT16_MAX, "event-code must fit in 16 bits");  \
    BUILD_ASSERT(DT_INST_PROP(inst, event_code) != INPUT_REL_X &&                                  \
                     DT_INST_PROP(inst, event_code) != INPUT_REL_Y,                                \
                 "event-code must not replace an encoded axis");                                   \
    static struct zpt_compact_split_encoder_data zpt_compact_split_encoder_data_##inst;            \
    static const struct zpt_compact_split_encoder_config zpt_compact_split_encoder_config_##inst = \
        {                                                                                          \
            .event_code = DT_INST_PROP(inst, event_code),                                          \
    };                                                                                             \
    DEVICE_DT_INST_DEFINE(                                                                         \
        inst, zpt_compact_split_encoder_device_init, NULL, &zpt_compact_split_encoder_data_##inst, \
        &zpt_compact_split_encoder_config_##inst, POST_KERNEL,                                     \
        CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &zpt_compact_split_encoder_driver_api);

DT_INST_FOREACH_STATUS_OKAY(ZPT_COMPACT_SPLIT_ENCODER_DEFINE)
