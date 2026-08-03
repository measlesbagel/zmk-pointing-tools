/* SPDX-License-Identifier: MIT */
#define DT_DRV_COMPAT measlesbagel_zpt_input_processor_text_nav

#include <errno.h>
#include <limits.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/dt-bindings/input/input-event-codes.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <drivers/input_processor.h>
#include <zmk/behavior_queue.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/keymap.h>
#include <zmk/virtual_key_position.h>

#include <zmk/pointing_tools/text_nav.h>
#include <zmk/pointing_tools/tuning.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define ZPT_TEXT_NAV_BINDING_COUNT 4

struct zpt_text_nav_config {
    uint8_t index;
    uint16_t tap_ms;
    struct zpt_text_nav_settings compiled;
    const struct zmk_behavior_binding *bindings;
    const char *tuning_id;
    const char *tuning_label;
    const char *devicetree_path;
};

struct zpt_text_nav_data {
    struct k_spinlock lock;
    int32_t frame_x;
    int32_t frame_y;
    uint32_t last_frame_ms;
    bool have_last_frame;
    struct zpt_text_nav_state gesture;
    struct zpt_text_nav_settings settings;
#if IS_ENABLED(CONFIG_ZMK_POINTING_TOOLS_RUNTIME_TUNING)
    struct zpt_tuning_target tuning_target;
#endif
};

static int32_t clamp_add(int32_t lhs, int32_t rhs) {
    return (int32_t)CLAMP((int64_t)lhs + rhs, INT32_MIN, INT32_MAX);
}

static void reset_processing_locked(struct zpt_text_nav_data *data) {
    data->frame_x = data->frame_y = 0;
    data->have_last_frame = false;
    zpt_text_nav_reset(&data->gesture);
}

static int queue_tap(const struct device *dev, struct zmk_input_processor_state *state,
                     enum zpt_text_nav_direction direction) {
    const struct zpt_text_nav_config *config = dev->config;
    struct zmk_behavior_binding_event behavior_event = {
        .position = ZMK_VIRTUAL_KEY_POSITION_BEHAVIOR_INPUT_PROCESSOR(state->input_device_index,
                                                                      config->index),
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

static int zpt_text_nav_handle_event(const struct device *dev, struct input_event *event,
                                     uint32_t param1, uint32_t param2,
                                     struct zmk_input_processor_state *state) {
    ARG_UNUSED(param1);
    ARG_UNUSED(param2);

    if (event->type != INPUT_EV_REL || (event->code != INPUT_REL_X && event->code != INPUT_REL_Y)) {
        return ZMK_INPUT_PROC_CONTINUE;
    }

    struct zpt_text_nav_data *data = dev->data;
    enum zpt_text_nav_direction direction = ZPT_TEXT_NAV_NONE;

    k_spinlock_key_t key = k_spin_lock(&data->lock);
    if (event->code == INPUT_REL_X) {
        data->frame_x = clamp_add(data->frame_x, event->value);
    } else {
        data->frame_y = clamp_add(data->frame_y, event->value);
    }

    if (event->sync) {
        uint32_t now = k_uptime_get_32();
        uint32_t elapsed = data->have_last_frame ? now - data->last_frame_ms : 0U;
        direction = zpt_text_nav_update(&data->gesture, &data->settings, data->frame_x,
                                        data->frame_y, elapsed, data->have_last_frame);
        data->frame_x = data->frame_y = 0;
        data->last_frame_ms = now;
        data->have_last_frame = true;
    }
    k_spin_unlock(&data->lock, key);

    if (direction != ZPT_TEXT_NAV_NONE) {
        int ret = queue_tap(dev, state, direction);
        if (ret < 0) {
            LOG_ERR("Failed to queue text navigation behavior: %d", ret);
        }
    }

    /* Layer overrides consume STOP before ZMK's HID listener sees it. Zeroing
     * preserves report synchronization without also moving the cursor. */
    event->value = 0;
    return ZMK_INPUT_PROC_CONTINUE;
}

#if IS_ENABLED(CONFIG_ZMK_POINTING_TOOLS_RUNTIME_TUNING)

enum zpt_text_nav_parameter_id {
    ZPT_TEXT_NAV_HORIZONTAL_THRESHOLD = 1,
    ZPT_TEXT_NAV_VERTICAL_THRESHOLD,
    ZPT_TEXT_NAV_ACTIVATION_DISTANCE,
    ZPT_TEXT_NAV_ENGAGE_RATIO,
    ZPT_TEXT_NAV_IDLE_TIMEOUT,
};

static const struct zpt_tuning_parameter zpt_text_nav_parameters[] = {
    {.id = ZPT_TEXT_NAV_HORIZONTAL_THRESHOLD,
     .type = ZPT_TUNING_VALUE_INTEGER,
     .key = "horizontal-threshold",
     .devicetree_property = "horizontal-threshold",
     .minimum = 1,
     .maximum = 10000,
     .step = 1,
     .label = "Horizontal step distance",
     .unit = "counts",
     .description = "Horizontal movement required for one left or right key tap. Lower values "
                    "move through text faster."},
    {.id = ZPT_TEXT_NAV_VERTICAL_THRESHOLD,
     .type = ZPT_TUNING_VALUE_INTEGER,
     .key = "vertical-threshold",
     .devicetree_property = "vertical-threshold",
     .minimum = 1,
     .maximum = 10000,
     .step = 1,
     .label = "Vertical step distance",
     .unit = "counts",
     .description = "Vertical movement required for one up or down key tap. Lower values move "
                    "through lines faster."},
    {.id = ZPT_TEXT_NAV_ACTIVATION_DISTANCE,
     .type = ZPT_TUNING_VALUE_INTEGER,
     .key = "activation-distance",
     .devicetree_property = "activation-distance",
     .minimum = 1,
     .maximum = 10000,
     .step = 1,
     .label = "Activation distance",
     .unit = "counts",
     .description = "Accumulated movement required before choosing the gesture's locked axis. "
                    "This prevents immediate direction choices from tiny initial motion."},
    {.id = ZPT_TEXT_NAV_ENGAGE_RATIO,
     .type = ZPT_TUNING_VALUE_INTEGER,
     .key = "engage-ratio-percent",
     .devicetree_property = "engage-ratio-percent",
     .minimum = 101,
     .maximum = 1000,
     .step = 1,
     .label = "Axis engage ratio",
     .unit = "%",
     .description = "Dominant-to-minor movement ratio required to choose an axis. Higher values "
                    "require a straighter initial gesture; 150% means 1.5:1."},
    {.id = ZPT_TEXT_NAV_IDLE_TIMEOUT,
     .type = ZPT_TUNING_VALUE_INTEGER,
     .key = "idle-timeout-ms",
     .devicetree_property = "idle-timeout-ms",
     .minimum = 10,
     .maximum = 2000,
     .step = 1,
     .label = "Gesture idle timeout",
     .unit = "ms",
     .description = "The motion-free gap that ends the current gesture and allows a new axis to "
                    "be selected."},
};

static int setting_get(const struct zpt_text_nav_settings *settings, uint8_t parameter_id,
                       int32_t *value) {
    switch (parameter_id) {
    case ZPT_TEXT_NAV_HORIZONTAL_THRESHOLD:
        *value = settings->horizontal_threshold;
        break;
    case ZPT_TEXT_NAV_VERTICAL_THRESHOLD:
        *value = settings->vertical_threshold;
        break;
    case ZPT_TEXT_NAV_ACTIVATION_DISTANCE:
        *value = settings->activation_distance;
        break;
    case ZPT_TEXT_NAV_ENGAGE_RATIO:
        *value = settings->engage_ratio_percent;
        break;
    case ZPT_TEXT_NAV_IDLE_TIMEOUT:
        *value = settings->idle_timeout_ms;
        break;
    default:
        return -ENOENT;
    }
    return 0;
}

static int tuning_get(void *context, uint8_t parameter_id, bool compiled, int32_t *value) {
    const struct device *dev = context;
    const struct zpt_text_nav_config *config = dev->config;
    struct zpt_text_nav_data *data = dev->data;

    if (compiled) {
        return setting_get(&config->compiled, parameter_id, value);
    }

    k_spinlock_key_t key = k_spin_lock(&data->lock);
    int ret = setting_get(&data->settings, parameter_id, value);
    k_spin_unlock(&data->lock, key);
    return ret;
}

static int tuning_set_many(void *context, const struct zpt_tuning_value *values, size_t value_count,
                           uint8_t *failed_parameter_id) {
    const struct device *dev = context;
    struct zpt_text_nav_data *data = dev->data;
    int ret = 0;

    k_spinlock_key_t key = k_spin_lock(&data->lock);
    struct zpt_text_nav_settings candidate = data->settings;
    for (size_t i = 0; i < value_count; i++) {
        if (failed_parameter_id != NULL) {
            *failed_parameter_id = values[i].parameter_id;
        }
        switch (values[i].parameter_id) {
        case ZPT_TEXT_NAV_HORIZONTAL_THRESHOLD:
            candidate.horizontal_threshold = values[i].value;
            break;
        case ZPT_TEXT_NAV_VERTICAL_THRESHOLD:
            candidate.vertical_threshold = values[i].value;
            break;
        case ZPT_TEXT_NAV_ACTIVATION_DISTANCE:
            candidate.activation_distance = values[i].value;
            break;
        case ZPT_TEXT_NAV_ENGAGE_RATIO:
            candidate.engage_ratio_percent = values[i].value;
            break;
        case ZPT_TEXT_NAV_IDLE_TIMEOUT:
            candidate.idle_timeout_ms = values[i].value;
            break;
        default:
            ret = -ENOENT;
            break;
        }
        if (ret < 0) {
            break;
        }
    }

    if (ret == 0) {
        data->settings = candidate;
        reset_processing_locked(data);
    }
    k_spin_unlock(&data->lock, key);
    return ret;
}

static int tuning_reset(void *context) {
    const struct device *dev = context;
    const struct zpt_text_nav_config *config = dev->config;
    struct zpt_text_nav_data *data = dev->data;

    k_spinlock_key_t key = k_spin_lock(&data->lock);
    data->settings = config->compiled;
    reset_processing_locked(data);
    k_spin_unlock(&data->lock, key);
    return 0;
}

#endif /* CONFIG_ZMK_POINTING_TOOLS_RUNTIME_TUNING */

static const struct zmk_input_processor_driver_api zpt_text_nav_driver_api = {
    .handle_event = zpt_text_nav_handle_event,
};

static int zpt_text_nav_init(const struct device *dev) {
    const struct zpt_text_nav_config *config = dev->config;
    struct zpt_text_nav_data *data = dev->data;
    data->settings = config->compiled;
    zpt_text_nav_reset(&data->gesture);
#if IS_ENABLED(CONFIG_ZMK_POINTING_TOOLS_RUNTIME_TUNING)
    data->tuning_target = (struct zpt_tuning_target){
        .kind = ZPT_TUNING_TARGET_TEXT_NAV,
        .stable_id = config->tuning_id,
        .label = config->tuning_label,
        .devicetree_path = config->devicetree_path,
        .parameters = zpt_text_nav_parameters,
        .parameter_count = ARRAY_SIZE(zpt_text_nav_parameters),
        .context = (void *)dev,
        .get = tuning_get,
        .set_many = tuning_set_many,
        .reset = tuning_reset,
    };
    int ret = zpt_tuning_register(&data->tuning_target);
    if (ret < 0) {
        return ret;
    }
#endif
    return 0;
}

#define ZPT_TEXT_NAV_DEFINE(inst)                                                                  \
    BUILD_ASSERT(DT_INST_PROP_LEN(inst, bindings) == ZPT_TEXT_NAV_BINDING_COUNT,                   \
                 "bindings must be left, right, up, and down");                                    \
    BUILD_ASSERT(DT_INST_PROP(inst, horizontal_threshold) > 0,                                     \
                 "horizontal-threshold must be positive");                                         \
    BUILD_ASSERT(DT_INST_PROP(inst, vertical_threshold) > 0,                                       \
                 "vertical-threshold must be positive");                                           \
    BUILD_ASSERT(DT_INST_PROP(inst, engage_ratio_percent) > 100,                                   \
                 "engage-ratio-percent must exceed 100");                                          \
    static const struct zmk_behavior_binding zpt_text_nav_bindings_##inst[] = {LISTIFY(            \
        DT_INST_PROP_LEN(inst, bindings), ZMK_KEYMAP_EXTRACT_BINDING, (, ), DT_DRV_INST(inst))};   \
    static const struct zpt_text_nav_config zpt_text_nav_config_##inst = {                         \
        .index = inst,                                                                             \
        .tap_ms = DT_INST_PROP(inst, tap_ms),                                                      \
        .compiled =                                                                                \
            {                                                                                      \
                .horizontal_threshold = DT_INST_PROP(inst, horizontal_threshold),                  \
                .vertical_threshold = DT_INST_PROP(inst, vertical_threshold),                      \
                .idle_timeout_ms = DT_INST_PROP(inst, idle_timeout_ms),                            \
                .activation_distance = DT_INST_PROP(inst, activation_distance),                    \
                .engage_ratio_percent = DT_INST_PROP(inst, engage_ratio_percent),                  \
            },                                                                                     \
        .bindings = zpt_text_nav_bindings_##inst,                                                  \
        .tuning_id = DT_INST_PROP(inst, tuning_id),                                                \
        .tuning_label = DT_INST_PROP(inst, tuning_label),                                          \
        .devicetree_path = DT_NODE_PATH(DT_DRV_INST(inst)),                                        \
    };                                                                                             \
    static struct zpt_text_nav_data zpt_text_nav_data_##inst;                                      \
    DEVICE_DT_INST_DEFINE(inst, zpt_text_nav_init, NULL, &zpt_text_nav_data_##inst,                \
                          &zpt_text_nav_config_##inst, POST_KERNEL,                                \
                          CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &zpt_text_nav_driver_api);

DT_INST_FOREACH_STATUS_OKAY(ZPT_TEXT_NAV_DEFINE)
