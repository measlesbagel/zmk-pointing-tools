/* SPDX-License-Identifier: MIT */
#define DT_DRV_COMPAT measlesbagel_zpt_input_processor_text_nav

#include <limits.h>
#include <stdlib.h>

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

#include <zmk/pointing_tools/axis_intent.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define ZPT_TEXT_NAV_BINDING_COUNT 4

enum zpt_text_nav_direction {
    ZPT_TEXT_NAV_LEFT = 0,
    ZPT_TEXT_NAV_RIGHT,
    ZPT_TEXT_NAV_UP,
    ZPT_TEXT_NAV_DOWN,
};

struct zpt_text_nav_config {
    uint8_t index;
    uint16_t tap_ms;
    uint16_t idle_timeout_ms;
    uint16_t activation_distance;
    uint16_t engage_ratio_percent;
    const struct zmk_behavior_binding *bindings;
};

struct zpt_text_nav_data {
    int32_t frame_x;
    int32_t frame_y;
    int32_t accumulated_x;
    int32_t accumulated_y;
    uint32_t last_frame_ms;
    enum zpt_axis_intent intent;
    bool have_last_frame;
};

static int32_t clamp_add(int32_t lhs, int32_t rhs) {
    return (int32_t)CLAMP((int64_t)lhs + rhs, INT32_MIN, INT32_MAX);
}

static uint32_t magnitude(int32_t value) {
    return value >= 0 ? (uint32_t)value : (uint32_t)(-(value + 1)) + 1U;
}

static bool dominates(uint32_t major, uint32_t minor, uint16_t ratio_percent) {
    return (uint64_t)major * 100U >= (uint64_t)minor * ratio_percent;
}

static void reset_gesture(struct zpt_text_nav_data *data) {
    data->accumulated_x = data->accumulated_y = 0;
    data->intent = ZPT_AXIS_INTENT_UNDECIDED;
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

static void process_frame(const struct device *dev, struct zmk_input_processor_state *state,
                          int32_t x, int32_t y, uint32_t horizontal_threshold,
                          uint32_t vertical_threshold) {
    const struct zpt_text_nav_config *config = dev->config;
    struct zpt_text_nav_data *data = dev->data;
    uint32_t now = k_uptime_get_32();
    uint32_t elapsed = data->have_last_frame ? now - data->last_frame_ms : 0U;

    if (!data->have_last_frame || elapsed >= config->idle_timeout_ms) {
        reset_gesture(data);
    }
    data->last_frame_ms = now;
    data->have_last_frame = true;

    if (data->intent == ZPT_AXIS_INTENT_UNDECIDED) {
        data->accumulated_x = clamp_add(data->accumulated_x, x);
        data->accumulated_y = clamp_add(data->accumulated_y, y);

        uint32_t horizontal = magnitude(data->accumulated_x);
        uint32_t vertical = magnitude(data->accumulated_y);
        if (horizontal + vertical < config->activation_distance) {
            return;
        }

        if (dominates(horizontal, vertical, config->engage_ratio_percent)) {
            data->intent = ZPT_AXIS_INTENT_HORIZONTAL;
            data->accumulated_y = 0;
        } else if (dominates(vertical, horizontal, config->engage_ratio_percent)) {
            data->intent = ZPT_AXIS_INTENT_VERTICAL;
            data->accumulated_x = 0;
        } else {
            return;
        }
    } else if (data->intent == ZPT_AXIS_INTENT_HORIZONTAL) {
        data->accumulated_x = clamp_add(data->accumulated_x, x);
    } else {
        data->accumulated_y = clamp_add(data->accumulated_y, y);
    }

    int32_t *movement;
    uint32_t threshold;
    enum zpt_text_nav_direction negative_direction;
    enum zpt_text_nav_direction positive_direction;

    if (data->intent == ZPT_AXIS_INTENT_HORIZONTAL) {
        movement = &data->accumulated_x;
        threshold = MAX(horizontal_threshold, 1U);
        negative_direction = ZPT_TEXT_NAV_LEFT;
        positive_direction = ZPT_TEXT_NAV_RIGHT;
    } else {
        movement = &data->accumulated_y;
        threshold = MAX(vertical_threshold, 1U);
        negative_direction = ZPT_TEXT_NAV_UP;
        positive_direction = ZPT_TEXT_NAV_DOWN;
    }

    if (magnitude(*movement) < threshold) {
        return;
    }

    enum zpt_text_nav_direction direction = *movement < 0 ? negative_direction : positive_direction;
    *movement += *movement < 0 ? (int32_t)threshold : -(int32_t)threshold;

    int ret = queue_tap(dev, state, direction);
    if (ret < 0) {
        LOG_ERR("Failed to queue text navigation behavior: %d", ret);
    }
}

static int zpt_text_nav_handle_event(const struct device *dev, struct input_event *event,
                                     uint32_t param1, uint32_t param2,
                                     struct zmk_input_processor_state *state) {
    if (event->type != INPUT_EV_REL || (event->code != INPUT_REL_X && event->code != INPUT_REL_Y)) {
        return ZMK_INPUT_PROC_CONTINUE;
    }

    struct zpt_text_nav_data *data = dev->data;
    if (event->code == INPUT_REL_X) {
        data->frame_x = clamp_add(data->frame_x, event->value);
    } else {
        data->frame_y = clamp_add(data->frame_y, event->value);
    }

    if (event->sync) {
        int32_t x = data->frame_x;
        int32_t y = data->frame_y;
        data->frame_x = data->frame_y = 0;
        process_frame(dev, state, x, y, param1, param2);
    }

    /* Layer overrides consume STOP before ZMK's HID listener sees it. Zeroing
     * preserves report synchronization without also moving the cursor. */
    event->value = 0;
    return ZMK_INPUT_PROC_CONTINUE;
}

static const struct zmk_input_processor_driver_api zpt_text_nav_driver_api = {
    .handle_event = zpt_text_nav_handle_event,
};

static int zpt_text_nav_init(const struct device *dev) {
    struct zpt_text_nav_data *data = dev->data;
    reset_gesture(data);
    return 0;
}

#define ZPT_TEXT_NAV_DEFINE(inst)                                                                  \
    BUILD_ASSERT(DT_INST_PROP_LEN(inst, bindings) == ZPT_TEXT_NAV_BINDING_COUNT,                   \
                 "bindings must be left, right, up, and down");                                    \
    BUILD_ASSERT(DT_INST_PROP(inst, engage_ratio_percent) >= 100,                                  \
                 "engage-ratio-percent must be at least 100");                                     \
    static const struct zmk_behavior_binding zpt_text_nav_bindings_##inst[] = {LISTIFY(            \
        DT_INST_PROP_LEN(inst, bindings), ZMK_KEYMAP_EXTRACT_BINDING, (, ), DT_DRV_INST(inst))};   \
    static const struct zpt_text_nav_config zpt_text_nav_config_##inst = {                         \
        .index = inst,                                                                             \
        .tap_ms = DT_INST_PROP(inst, tap_ms),                                                      \
        .idle_timeout_ms = DT_INST_PROP(inst, idle_timeout_ms),                                    \
        .activation_distance = DT_INST_PROP(inst, activation_distance),                            \
        .engage_ratio_percent = DT_INST_PROP(inst, engage_ratio_percent),                          \
        .bindings = zpt_text_nav_bindings_##inst,                                                  \
    };                                                                                             \
    static struct zpt_text_nav_data zpt_text_nav_data_##inst;                                      \
    DEVICE_DT_INST_DEFINE(inst, zpt_text_nav_init, NULL, &zpt_text_nav_data_##inst,                \
                          &zpt_text_nav_config_##inst, POST_KERNEL,                                \
                          CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &zpt_text_nav_driver_api);

DT_INST_FOREACH_STATUS_OKAY(ZPT_TEXT_NAV_DEFINE)
