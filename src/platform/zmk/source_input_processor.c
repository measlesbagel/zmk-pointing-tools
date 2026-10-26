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
#include <zmk/pointing_tools/source/source_ingress.h>
#if IS_ENABLED(CONFIG_ZMK_POINTING_TOOLS_DEVICE_TABLE)
#include <zmk/pointing_tools/platform/zmk/device_table.h>
#endif

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

struct zpt_source_processor_config {
    struct zpt_source_ingress_config ingress;
    const struct device *router_device;
    /* Stable id of the pointing-device entry whose resolution labels this
     * source's frames, or NULL to keep the compiled resolution. */
    const char *device_stable_id;
    uint16_t compact_event_code;
    bool compact_transport;
};

struct zpt_source_processor_data {
    struct k_spinlock frame_lock;
    struct zpt_source_ingress ingress;
#if IS_ENABLED(CONFIG_ZMK_POINTING_TOOLS_DEVICE_TABLE)
    const struct zpt_pointing_device *device;
#endif
};

static uint16_t zpt_source_followed_cpi(const struct zpt_source_processor_data *data) {
#if IS_ENABLED(CONFIG_ZMK_POINTING_TOOLS_DEVICE_TABLE)
    return data->device != NULL ? zpt_device_current_cpi(data->device) : 0U;
#else
    ARG_UNUSED(data);
    return 0U;
#endif
}

/* Referenced by DEVICE_DT_INST_DEFINE only when the devicetree contains an
 * input-processor-source instance, so it reads as unused in configurations
 * without one. */
// NOLINTNEXTLINE(clang-diagnostic-unused-function)
static int zpt_source_processor_init(const struct device *dev) {
    const struct zpt_source_processor_config *config = dev->config;
    struct zpt_source_processor_data *data = dev->data;

    int ret = zpt_source_ingress_init(&data->ingress, &config->ingress);
    if (ret < 0) {
        LOG_ERR("Failed to initialize motion source %u: %d", config->ingress.source.source_id, ret);
        return ret;
    }
#if IS_ENABLED(CONFIG_ZMK_POINTING_TOOLS_DEVICE_TABLE)
    if (config->device_stable_id != NULL) {
        data->device = zpt_device_table_find(config->device_stable_id);
        if (data->device == NULL) {
            LOG_ERR("Motion source %u names unknown pointing device %s",
                    config->ingress.source.source_id, config->device_stable_id);
            return -ENODEV;
        }
    }
#endif
    return device_is_ready(config->router_device) ? 0 : -ENODEV;
}

static enum zpt_source_event_kind classify_event(const struct zpt_source_processor_config *config,
                                                 const struct input_event *event) {
    if (event->type != INPUT_EV_REL) {
        return ZPT_SOURCE_EVENT_OTHER;
    }
    if (config->compact_transport && event->code == config->compact_event_code) {
        return ZPT_SOURCE_EVENT_COMPACT;
    }
    if (event->code == INPUT_REL_X) {
        return ZPT_SOURCE_EVENT_AXIS_X;
    }
    return event->code == INPUT_REL_Y ? ZPT_SOURCE_EVENT_AXIS_Y : ZPT_SOURCE_EVENT_OTHER;
}

static int zpt_source_processor_handle_event(const struct device *dev, struct input_event *event,
                                             uint32_t param1, uint32_t param2,
                                             struct zmk_input_processor_state *state) {
    ARG_UNUSED(param1);
    ARG_UNUSED(param2);
    ARG_UNUSED(state);

    const struct zpt_source_processor_config *config = dev->config;
    struct zpt_source_processor_data *data = dev->data;
    const enum zpt_source_event_kind kind = classify_event(config, event);
    struct zpt_signal signal;
    int decode_error;

    k_spinlock_key_t key = k_spin_lock(&data->frame_lock);
    zpt_source_ingress_follow_resolution(&data->ingress, zpt_source_followed_cpi(data));
    const bool frame_ready =
        zpt_source_ingress_handle(&data->ingress, &config->ingress, kind, event->value, event->sync,
                                  k_uptime_get_32(), &signal, &decode_error);
    k_spin_unlock(&data->frame_lock, key);

    if (decode_error < 0) {
        LOG_ERR("Source %u rejected compact split packet: %d", config->ingress.source.source_id,
                decode_error);
    }
    if (frame_ready) {
        struct zpt_pipeline_result result;
        int ret = zpt_zmk_router_push(config->router_device, &signal, &result);
        if (ret < 0) {
            LOG_ERR("Motion router rejected source %u frame: %d", config->ingress.source.source_id,
                    ret);
        }
    }
    if (kind == ZPT_SOURCE_EVENT_OTHER) {
        return ZMK_INPUT_PROC_CONTINUE;
    }

    /* The motion now lives in the source frame; nothing downstream may
     * observe it a second time. Zeroing covers a layer override, which
     * swallows STOP; STOP keeps the base listener from sending an empty
     * mouse report on every sync, since the router reports the motion.
     * STOP is positive, so the listener ends the event without an error. */
    event->value = 0;
    return ZMK_INPUT_PROC_STOP;
}

/* See zpt_source_processor_init above: used only when a devicetree instance
 * instantiates this driver. */
// NOLINTNEXTLINE(clang-diagnostic-unused-const-variable)
static const struct zmk_input_processor_driver_api zpt_source_processor_driver_api = {
    .handle_event = zpt_source_processor_handle_event,
};

/* The compiled resolution: the source's own, else its pointing device's. */
#define ZPT_SOURCE_RESOLUTION_CPI(inst)                                                            \
    COND_CODE_1(DT_INST_NODE_HAS_PROP(inst, resolution_cpi), (DT_INST_PROP(inst, resolution_cpi)), \
                (DT_PROP_OR(DT_INST_PHANDLE(inst, device), resolution_cpi, 0)))

#define ZPT_SOURCE_DEVICE_STABLE_ID(inst)                                                          \
    COND_CODE_1(DT_INST_NODE_HAS_PROP(inst, device),                                               \
                (DT_PROP(DT_INST_PHANDLE(inst, device), stable_id)), (NULL))

#define ZPT_SOURCE_PROCESSOR_DEFINE(inst)                                                          \
    BUILD_ASSERT(DT_INST_PROP(inst, source_id) >= 0, "source-id cannot be negative");              \
    BUILD_ASSERT(DT_INST_PROP(inst, source_id) <= UINT16_MAX, "source-id must fit in 16 bits");    \
    BUILD_ASSERT(DT_INST_NODE_HAS_PROP(inst, resolution_cpi) ||                                    \
                     DT_INST_NODE_HAS_PROP(inst, device),                                          \
                 "set resolution-cpi, or a device to take it from");                               \
    BUILD_ASSERT(!DT_INST_NODE_HAS_PROP(inst, device) ||                                           \
                     IS_ENABLED(CONFIG_ZMK_POINTING_TOOLS_DEVICE_TABLE),                           \
                 "a source device needs CONFIG_ZMK_POINTING_TOOLS_DEVICE_TABLE");                  \
    BUILD_ASSERT(ZPT_SOURCE_RESOLUTION_CPI(inst) > 0, "resolution-cpi must be positive");          \
    BUILD_ASSERT(ZPT_SOURCE_RESOLUTION_CPI(inst) <= UINT16_MAX,                                    \
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
        .ingress =                                                                                 \
            {                                                                                      \
                .source =                                                                          \
                    {                                                                              \
                        .flags =                                                                   \
                            COND_CODE_1(DT_INST_PROP(inst, transported),                           \
                                        (ZPT_SIGNAL_FLAG_TRANSPORTED), (ZPT_SIGNAL_FLAG_LOCAL)),   \
                        .source_id = DT_INST_PROP(inst, source_id),                                \
                        .resolution_cpi = ZPT_SOURCE_RESOLUTION_CPI(inst),                         \
                    },                                                                             \
                .compact_transport_coalesced = DT_INST_PROP(inst, compact_transport_coalesced),    \
            },                                                                                     \
        .router_device = DEVICE_DT_GET(DT_INST_PHANDLE(inst, router)),                             \
        .device_stable_id = ZPT_SOURCE_DEVICE_STABLE_ID(inst),                                     \
        .compact_event_code = DT_INST_PROP_OR(inst, compact_event_code, 0),                        \
        .compact_transport = DT_INST_NODE_HAS_PROP(inst, compact_event_code),                      \
    };                                                                                             \
    DEVICE_DT_INST_DEFINE(inst, zpt_source_processor_init, NULL,                                   \
                          &zpt_source_processor_data_##inst, &zpt_source_processor_config_##inst,  \
                          POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,                        \
                          &zpt_source_processor_driver_api);

DT_INST_FOREACH_STATUS_OKAY(ZPT_SOURCE_PROCESSOR_DEFINE)
