/* SPDX-License-Identifier: MIT */
#define DT_DRV_COMPAT measlesbagel_zpt_input_processor_trace

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/dt-bindings/input/input-event-codes.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include <drivers/input_processor.h>

#include <zmk/pointing_tools/trace.h>

struct zpt_trace_config {
    struct zpt_trace_descriptor descriptor;
};

struct zpt_trace_data {
    int32_t x;
    int32_t y;
    int32_t wheel;
    int32_t h_wheel;
};

__weak void zpt_telemetry_submit(const struct zpt_trace_sample *sample) { ARG_UNUSED(sample); }

static int zpt_trace_handle_event(const struct device *dev, struct input_event *event,
                                  uint32_t param1, uint32_t param2,
                                  struct zmk_input_processor_state *state) {
    ARG_UNUSED(param1);
    ARG_UNUSED(param2);
    ARG_UNUSED(state);

    if (event->type != INPUT_EV_REL) {
        return ZMK_INPUT_PROC_CONTINUE;
    }

    const struct zpt_trace_config *config = dev->config;
    struct zpt_trace_data *data = dev->data;

    switch (event->code) {
    case INPUT_REL_X:
        data->x += event->value;
        break;
    case INPUT_REL_Y:
        data->y += event->value;
        break;
    case INPUT_REL_WHEEL:
        data->wheel += event->value;
        break;
    case INPUT_REL_HWHEEL:
        data->h_wheel += event->value;
        break;
    default:
        return ZMK_INPUT_PROC_CONTINUE;
    }

    if (event->sync) {
        const struct zpt_trace_sample sample = {
            .timestamp_ms = k_uptime_get_32(),
            .pointing_device_id = config->descriptor.pointing_device_id,
            .stage = config->descriptor.stage,
            .x = data->x,
            .y = data->y,
            .wheel = data->wheel,
            .h_wheel = data->h_wheel,
        };
        zpt_telemetry_submit(&sample);
        data->x = data->y = data->wheel = data->h_wheel = 0;
    }

    return ZMK_INPUT_PROC_CONTINUE;
}

static const struct zmk_input_processor_driver_api zpt_trace_driver_api = {
    .handle_event = zpt_trace_handle_event,
};

#define ZPT_TRACE_DEFINE(inst)                                                                     \
    BUILD_ASSERT(DT_INST_PROP(inst, pointing_device_id) <= UINT8_MAX,                              \
                 "pointing-device-id must fit in one byte");                                       \
    BUILD_ASSERT(DT_INST_PROP(inst, stage) <= UINT8_MAX, "stage must fit in one byte");            \
    static struct zpt_trace_data zpt_trace_data_##inst;                                            \
    static const struct zpt_trace_config zpt_trace_config_##inst = {                               \
        .descriptor = {.pointing_device_id = DT_INST_PROP(inst, pointing_device_id),               \
                       .stage = DT_INST_PROP(inst, stage),                                         \
                       .label = DT_INST_PROP(inst, label)},                                        \
    };                                                                                             \
    DEVICE_DT_INST_DEFINE(inst, NULL, NULL, &zpt_trace_data_##inst, &zpt_trace_config_##inst,      \
                          POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,                        \
                          &zpt_trace_driver_api);

DT_INST_FOREACH_STATUS_OKAY(ZPT_TRACE_DEFINE)

#define ZPT_TRACE_DEVICE(inst) DEVICE_DT_INST_GET(inst),

static const struct device *const zpt_trace_devices[] = {
    DT_INST_FOREACH_STATUS_OKAY(ZPT_TRACE_DEVICE)};

size_t zpt_trace_stream_count(void) { return ARRAY_SIZE(zpt_trace_devices); }

const struct zpt_trace_descriptor *zpt_trace_stream_descriptor(size_t index) {
    if (index >= ARRAY_SIZE(zpt_trace_devices)) {
        return NULL;
    }

    const struct zpt_trace_config *config = zpt_trace_devices[index]->config;
    return &config->descriptor;
}
