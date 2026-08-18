/* SPDX-License-Identifier: MIT */
#define DT_DRV_COMPAT measlesbagel_zpt_sink_action

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>

#include <zmk/behavior.h>
#include <zmk/behavior_queue.h>
#include <zmk/keymap.h>
#include <zmk/virtual_key_position.h>

#include <zmk/pointing_tools/platform/zmk/sink_provider.h>

/* Left, right, up, and down action bindings. */
#define ZPT_ACTION_BINDING_COUNT 4

struct zpt_action_sink_config {
    const struct zmk_behavior_binding *bindings;
    uint16_t tap_ms;
    uint16_t input_device_index;
    uint16_t behavior_index;
};

struct zpt_action_sink_provider_config {
    const char *stable_id;
    const struct zmk_behavior_binding *bindings;
    uint16_t tap_ms;
    uint16_t input_device_index;
    uint16_t behavior_index;
};

struct zpt_action_sink_provider_data {
    struct zpt_zmk_sink_provider_data provider;
    struct zpt_action_sink_config sink_config;
};

static int action_sink_emit(struct zpt_sink *sink, const struct zpt_signal *signal) {
    if (signal->kind != ZPT_SIGNAL_ACTION) {
        return -EPROTOTYPE;
    }
    const struct zpt_action_sink_config *config = sink->config;
    if (config == NULL || config->bindings == NULL) {
        return -EINVAL;
    }
    uint32_t direction = signal->data.action.id;
    if (direction >= ZPT_ACTION_BINDING_COUNT) {
        return -EINVAL;
    }

    struct zmk_behavior_binding_event behavior_event = {
        .position = ZMK_VIRTUAL_KEY_POSITION_BEHAVIOR_INPUT_PROCESSOR(config->input_device_index,
                                                                      config->behavior_index),
        .timestamp = k_uptime_get(),
#if IS_ENABLED(CONFIG_ZMK_SPLIT)
        .source = ZMK_POSITION_STATE_CHANGE_SOURCE_LOCAL,
#endif
    };

    int ret =
        zmk_behavior_queue_add(&behavior_event, config->bindings[direction], true, config->tap_ms);
    if (ret < 0) {
        return ret;
    }
    return zmk_behavior_queue_add(&behavior_event, config->bindings[direction], false, 0);
}

static const struct zpt_sink_api action_sink_api = {
    .type_id = "zmk-action",
    .accepted_kinds = ZPT_SIGNAL_KIND_MASK(ZPT_SIGNAL_ACTION),
    .emit = action_sink_emit,
};

static struct zpt_sink *action_sink_get(const struct device *dev) {
    struct zpt_action_sink_provider_data *data = dev->data;
    return &data->provider.sink;
}

static int action_sink_bind_output(const struct device *dev, const struct device *output_device) {
    (void)dev;
    (void)output_device;
    return 0;
}

static DEVICE_API(zpt_sink_provider, action_sink_provider_api) = {
    .get_sink = action_sink_get,
    .bind_output = action_sink_bind_output,
};

static int action_sink_provider_init(const struct device *dev) {
    const struct zpt_action_sink_provider_config *config = dev->config;
    struct zpt_action_sink_provider_data *data = dev->data;
    data->sink_config.bindings = config->bindings;
    data->sink_config.tap_ms = config->tap_ms;
    data->sink_config.input_device_index = config->input_device_index;
    data->sink_config.behavior_index = config->behavior_index;
    return zpt_zmk_sink_provider_init(dev, config->stable_id, &action_sink_api, &data->sink_config,
                                      NULL);
}

#define ZPT_ACTION_SINK_PROVIDER_DEFINE(inst)                                                      \
    BUILD_ASSERT(DT_INST_PROP_LEN(inst, bindings) == ZPT_ACTION_BINDING_COUNT,                     \
                 "bindings must be left, right, up, and down");                                    \
    static const struct zmk_behavior_binding zpt_action_sink_bindings_##inst[] = {LISTIFY(         \
        DT_INST_PROP_LEN(inst, bindings), ZMK_KEYMAP_EXTRACT_BINDING, (, ), DT_DRV_INST(inst))};   \
    static struct zpt_action_sink_provider_data zpt_action_sink_provider_data_##inst;              \
    static const struct zpt_action_sink_provider_config zpt_action_sink_provider_config_##inst = { \
        .stable_id = DT_INST_PROP(inst, stable_id),                                                \
        .bindings = zpt_action_sink_bindings_##inst,                                               \
        .tap_ms = DT_INST_PROP_OR(inst, tap_ms, 0),                                                \
        .input_device_index = DT_INST_PROP_OR(inst, input_device_index, 0),                        \
        .behavior_index = DT_INST_PROP_OR(inst, behavior_index, 0),                                \
    };                                                                                             \
    DEVICE_DT_INST_DEFINE(inst, action_sink_provider_init, NULL,                                   \
                          &zpt_action_sink_provider_data_##inst,                                   \
                          &zpt_action_sink_provider_config_##inst, POST_KERNEL,                    \
                          CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &action_sink_provider_api);

DT_INST_FOREACH_STATUS_OKAY(ZPT_ACTION_SINK_PROVIDER_DEFINE)
