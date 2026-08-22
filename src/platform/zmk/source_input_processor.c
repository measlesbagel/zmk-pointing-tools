/* SPDX-License-Identifier: MIT */
#define DT_DRV_COMPAT measlesbagel_zpt_input_processor_source

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/dt-bindings/input/input-event-codes.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <drivers/input_processor.h>

#include <zmk/pointing_tools/platform/zmk/router.h>
#include <zmk/pointing_tools/source/motion_source.h>
#include <zmk/pointing_tools/source/transport/compact_split_codec.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

struct zpt_source_processor_config {
    struct zpt_motion_source_config source;
    const struct device *router_device;
    uint16_t compact_event_code;
    bool compact_transport;
    bool compact_transport_coalesced;
};

struct zpt_source_processor_data {
    struct k_spinlock frame_lock;
    struct zpt_motion_source_state source;
    struct zpt_compact_split_decoder compact_decoder;
};

/* Referenced by DEVICE_DT_INST_DEFINE only when the devicetree contains an
 * input-processor-source instance, so it reads as unused in configurations
 * without one. */
// NOLINTNEXTLINE(clang-diagnostic-unused-function)
static int zpt_source_processor_init(const struct device *dev) {
    const struct zpt_source_processor_config *config = dev->config;
    struct zpt_source_processor_data *data = dev->data;

    int ret = zpt_motion_source_init(&data->source, &config->source);
    if (ret < 0) {
        LOG_ERR("Failed to initialize motion source %u: %d", config->source.source_id, ret);
        return ret;
    }
    zpt_compact_split_decoder_init(&data->compact_decoder);
    return device_is_ready(config->router_device) ? 0 : -ENODEV;
}

static int zpt_source_processor_handle_event(const struct device *dev, struct input_event *event,
                                             uint32_t param1, uint32_t param2,
                                             struct zmk_input_processor_state *state) {
    ARG_UNUSED(param1);
    ARG_UNUSED(param2);
    ARG_UNUSED(state);

    const struct zpt_source_processor_config *config = dev->config;
    struct zpt_source_processor_data *data = dev->data;
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
            LOG_ERR("Source %u rejected compact split packet: %d", config->source.source_id, ret);
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
    int ret = zpt_zmk_router_push(config->router_device, &signal, &result);
    if (ret < 0) {
        LOG_ERR("Motion router rejected source %u frame: %d", config->source.source_id, ret);
    }

    /* Keep the processor chain intact: ZMK's input listener treats any
     * non-CONTINUE return as termination and drops the whole event on a
     * negative value, which would also discard the frame boundary. */
    return ZMK_INPUT_PROC_CONTINUE;
}

/* See zpt_source_processor_init above: used only when a devicetree instance
 * instantiates this driver. */
// NOLINTNEXTLINE(clang-diagnostic-unused-const-variable)
static const struct zmk_input_processor_driver_api zpt_source_processor_driver_api = {
    .handle_event = zpt_source_processor_handle_event,
};

#define ZPT_SOURCE_PROCESSOR_DEFINE(inst)                                                          \
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
    static struct zpt_source_processor_data zpt_source_processor_data_##inst;                      \
    static const struct zpt_source_processor_config zpt_source_processor_config_##inst = {         \
        .source =                                                                                  \
            {                                                                                      \
                .flags = COND_CODE_1(DT_INST_PROP(inst, transported),                              \
                                     (ZPT_SIGNAL_FLAG_TRANSPORTED), (ZPT_SIGNAL_FLAG_LOCAL)),      \
                .source_id = DT_INST_PROP(inst, source_id),                                        \
                .resolution_cpi = DT_INST_PROP(inst, resolution_cpi),                              \
            },                                                                                     \
        .router_device = DEVICE_DT_GET(DT_INST_PHANDLE(inst, router)),                             \
        .compact_event_code = DT_INST_PROP_OR(inst, compact_event_code, 0),                        \
        .compact_transport = DT_INST_NODE_HAS_PROP(inst, compact_event_code),                      \
        .compact_transport_coalesced = DT_INST_PROP(inst, compact_transport_coalesced),            \
    };                                                                                             \
    DEVICE_DT_INST_DEFINE(inst, zpt_source_processor_init, NULL,                                   \
                          &zpt_source_processor_data_##inst, &zpt_source_processor_config_##inst,  \
                          POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,                        \
                          &zpt_source_processor_driver_api);

DT_INST_FOREACH_STATUS_OKAY(ZPT_SOURCE_PROCESSOR_DEFINE)
