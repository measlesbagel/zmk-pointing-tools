/* SPDX-License-Identifier: MIT */
#define DT_DRV_COMPAT measlesbagel_zpt_input_processor_scroll

#include <limits.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/dt-bindings/input/input-event-codes.h>
#include <zephyr/input/input.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include <drivers/input_processor.h>

#include <zmk/pointing_tools/axis_intent.h>

struct zpt_scroll_config {
    uint16_t scale_multiplier;
    uint16_t scale_divisor;
    uint16_t report_interval_ms;
    uint16_t idle_timeout_ms;
    struct zpt_axis_intent_config intent;
};

struct zpt_scroll_data {
    const struct device *dev;
    struct k_work_delayable flush_work;
    struct k_spinlock lock;
    struct zpt_axis_intent_state intent;
    enum zpt_axis_policy policy;
    int32_t frame_x;
    int32_t undecided_x;
    int32_t undecided_y;
    int32_t pending_x;
    int32_t pending_y;
    int32_t remainder_x;
    int32_t remainder_y;
    uint32_t last_frame_ms;
    bool have_last_frame;
};

static int32_t clamp_add(int32_t lhs, int32_t rhs) {
    int64_t result = (int64_t)lhs + rhs;
    return (int32_t)CLAMP(result, INT32_MIN, INT32_MAX);
}

static void accumulate_filtered(struct zpt_scroll_data *data, enum zpt_axis_intent intent,
                                int32_t x, int32_t y) {
    if (intent != ZPT_AXIS_INTENT_VERTICAL) {
        data->pending_x = clamp_add(data->pending_x, x);
    }
    if (intent != ZPT_AXIS_INTENT_HORIZONTAL) {
        data->pending_y = clamp_add(data->pending_y, y);
    }
}

static int16_t take_scaled(int32_t *pending, int32_t *remainder, uint16_t multiplier,
                           uint16_t divisor) {
    int64_t numerator = (int64_t)*pending * multiplier + *remainder;
    int64_t scaled = numerator / divisor;
    int16_t output = (int16_t)CLAMP(scaled, INT16_MIN, INT16_MAX);

    /* Keep both fractional and HID-range overflow for a later report. */
    int64_t remaining = numerator - ((int64_t)output * divisor);
    *remainder = (int32_t)CLAMP(remaining, INT32_MIN, INT32_MAX);
    *pending = 0;
    return output;
}

static void zpt_scroll_flush(struct k_work *work) {
    struct k_work_delayable *delayable = k_work_delayable_from_work(work);
    struct zpt_scroll_data *data = CONTAINER_OF(delayable, struct zpt_scroll_data, flush_work);
    const struct zpt_scroll_config *config = data->dev->config;
    int16_t horizontal;
    int16_t vertical;

    k_spinlock_key_t key = k_spin_lock(&data->lock);

    /* Preserve very short gestures that ended before adaptive intent engaged. */
    if (data->undecided_x != 0 || data->undecided_y != 0) {
        accumulate_filtered(data, ZPT_AXIS_INTENT_FREE, data->undecided_x, data->undecided_y);
        data->undecided_x = data->undecided_y = 0;
    }

    horizontal = take_scaled(&data->pending_x, &data->remainder_x, config->scale_multiplier,
                             config->scale_divisor);
    vertical = take_scaled(&data->pending_y, &data->remainder_y, config->scale_multiplier,
                           config->scale_divisor);
    k_spin_unlock(&data->lock, key);

    if (horizontal == 0 && vertical == 0) {
        return;
    }

    /* This processor device is also a virtual input source. A dedicated ZMK
     * input listener consumes these two events as one synchronized HID frame. */
    input_report(data->dev, INPUT_EV_REL, INPUT_REL_HWHEEL, horizontal, false, K_NO_WAIT);
    input_report(data->dev, INPUT_EV_REL, INPUT_REL_WHEEL, vertical, true, K_NO_WAIT);
}

static void schedule_flush(struct zpt_scroll_data *data, uint16_t interval_ms) {
    if (!k_work_delayable_is_pending(&data->flush_work)) {
        k_work_schedule(&data->flush_work, K_MSEC(interval_ms));
    }
}

static void process_frame(const struct device *dev, int32_t x, int32_t y,
                          enum zpt_axis_policy policy) {
    const struct zpt_scroll_config *config = dev->config;
    struct zpt_scroll_data *data = dev->data;
    uint32_t now = k_uptime_get_32();

    k_spinlock_key_t key = k_spin_lock(&data->lock);
    uint32_t elapsed = data->have_last_frame ? now - data->last_frame_ms : 0U;

    if (!data->have_last_frame || elapsed >= config->idle_timeout_ms || policy != data->policy) {
        zpt_axis_intent_reset(&data->intent);
        data->undecided_x = data->undecided_y = 0;
    }

    data->policy = policy;
    data->last_frame_ms = now;
    data->have_last_frame = true;

    enum zpt_axis_intent previous = data->intent.intent;
    enum zpt_axis_intent intent =
        zpt_axis_intent_update(&data->intent, &config->intent, policy, x, y, elapsed);

    if (intent == ZPT_AXIS_INTENT_UNDECIDED) {
        data->undecided_x = clamp_add(data->undecided_x, x);
        data->undecided_y = clamp_add(data->undecided_y, y);
    } else {
        if (previous == ZPT_AXIS_INTENT_UNDECIDED) {
            data->undecided_x = clamp_add(data->undecided_x, x);
            data->undecided_y = clamp_add(data->undecided_y, y);
            accumulate_filtered(data, intent, data->undecided_x, data->undecided_y);
            data->undecided_x = data->undecided_y = 0;
        } else {
            accumulate_filtered(data, intent, x, y);
        }
    }

    schedule_flush(data, config->report_interval_ms);
    k_spin_unlock(&data->lock, key);
}

static int zpt_scroll_handle_event(const struct device *dev, struct input_event *event,
                                   uint32_t param1, uint32_t param2,
                                   struct zmk_input_processor_state *state) {
    ARG_UNUSED(param2);
    ARG_UNUSED(state);

    if (event->type != INPUT_EV_REL || (event->code != INPUT_REL_X && event->code != INPUT_REL_Y)) {
        return ZMK_INPUT_PROC_CONTINUE;
    }

    struct zpt_scroll_data *data = dev->data;
    if (event->code == INPUT_REL_X) {
        k_spinlock_key_t key = k_spin_lock(&data->lock);
        data->frame_x = clamp_add(data->frame_x, event->value);
        k_spin_unlock(&data->lock, key);
        return ZMK_INPUT_PROC_STOP;
    }

    k_spinlock_key_t key = k_spin_lock(&data->lock);
    int32_t x = data->frame_x;
    data->frame_x = 0;
    k_spin_unlock(&data->lock, key);

    process_frame(dev, x, event->value, (enum zpt_axis_policy)param1);
    return ZMK_INPUT_PROC_STOP;
}

static const struct zmk_input_processor_driver_api zpt_scroll_driver_api = {
    .handle_event = zpt_scroll_handle_event,
};

static int zpt_scroll_init(const struct device *dev) {
    struct zpt_scroll_data *data = dev->data;
    data->dev = dev;
    data->policy = ZPT_AXIS_POLICY_ADAPTIVE;
    zpt_axis_intent_reset(&data->intent);
    k_work_init_delayable(&data->flush_work, zpt_scroll_flush);
    return 0;
}

#define ZPT_SCROLL_DEFINE(inst)                                                                    \
    BUILD_ASSERT(DT_INST_PROP(inst, scale_multiplier) > 0, "scale-multiplier must be positive");   \
    BUILD_ASSERT(DT_INST_PROP(inst, scale_divisor) > 0, "scale-divisor must be positive");         \
    BUILD_ASSERT(DT_INST_PROP(inst, report_interval_ms) > 0,                                       \
                 "report-interval-ms must be positive");                                           \
    BUILD_ASSERT(DT_INST_PROP(inst, engage_ratio_percent) >                                        \
                     DT_INST_PROP(inst, release_ratio_percent),                                    \
                 "engage ratio must exceed release ratio");                                        \
    static struct zpt_scroll_data zpt_scroll_data_##inst;                                          \
    static const struct zpt_scroll_config zpt_scroll_config_##inst = {                             \
        .scale_multiplier = DT_INST_PROP(inst, scale_multiplier),                                  \
        .scale_divisor = DT_INST_PROP(inst, scale_divisor),                                        \
        .report_interval_ms = DT_INST_PROP(inst, report_interval_ms),                              \
        .idle_timeout_ms = DT_INST_PROP(inst, idle_timeout_ms),                                    \
        .intent = {.engage_ratio_percent = DT_INST_PROP(inst, engage_ratio_percent),               \
                   .release_ratio_percent = DT_INST_PROP(inst, release_ratio_percent),             \
                   .activation_distance = DT_INST_PROP(inst, activation_distance),                 \
                   .window_ms = DT_INST_PROP(inst, intent_window_ms)},                             \
    };                                                                                             \
    DEVICE_DT_INST_DEFINE(inst, zpt_scroll_init, NULL, &zpt_scroll_data_##inst,                    \
                          &zpt_scroll_config_##inst, POST_KERNEL,                                  \
                          CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &zpt_scroll_driver_api);

DT_INST_FOREACH_STATUS_OKAY(ZPT_SCROLL_DEFINE)
