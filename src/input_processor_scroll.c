/* SPDX-License-Identifier: MIT */
#define DT_DRV_COMPAT measlesbagel_zpt_input_processor_scroll

#include <errno.h>
#include <limits.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/dt-bindings/input/input-event-codes.h>
#include <zephyr/input/input.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>

#include <drivers/input_processor.h>
#include <zmk/event_manager.h>
#include <zmk/events/position_state_changed.h>

#include <zmk/pointing_tools/scroll.h>
#if IS_ENABLED(CONFIG_ZMK_POINTING_TOOLS_STATE_TELEMETRY)
#include <zmk/pointing_tools/state.h>
#endif
#include <zmk/pointing_tools/tuning.h>

struct zpt_scroll_config {
    struct zpt_scroll_settings compiled;
    const char *tuning_id;
    const char *tuning_label;
    const char *devicetree_path;
};

struct zpt_scroll_data {
    const struct device *dev;
    struct k_work_delayable flush_work;
    struct k_spinlock lock;
    struct zpt_scroll_state scroll;
    int32_t frame_x;
    int32_t frame_y;
    bool flush_armed;
    struct zpt_scroll_settings settings;
#if IS_ENABLED(CONFIG_ZMK_POINTING_TOOLS_RUNTIME_TUNING)
    struct zpt_tuning_target tuning_target;
#endif
#if IS_ENABLED(CONFIG_ZMK_POINTING_TOOLS_STATE_TELEMETRY)
    uint8_t telemetry_target_id;
    bool state_suppressed;
#endif
};

static atomic_t zpt_scroll_keypress_seen;
static atomic_t zpt_scroll_last_keypress_ms;

static int zpt_scroll_key_activity_listener(const zmk_event_t *event) {
    const struct zmk_position_state_changed *position = as_zmk_position_state_changed(event);
    if (position != NULL && position->state) {
        atomic_set(&zpt_scroll_last_keypress_ms, (atomic_val_t)k_uptime_get_32());
        atomic_set(&zpt_scroll_keypress_seen, 1);
    }

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(zpt_scroll_key_activity, zpt_scroll_key_activity_listener);
ZMK_SUBSCRIPTION(zpt_scroll_key_activity, zmk_position_state_changed);

static int32_t clamp_add(int32_t lhs, int32_t rhs) {
    int64_t result = (int64_t)lhs + rhs;
    return (int32_t)CLAMP(result, INT32_MIN, INT32_MAX);
}

static void zpt_scroll_flush_work(struct k_work *work) {
    struct k_work_delayable *delayable = k_work_delayable_from_work(work);
    struct zpt_scroll_data *data = CONTAINER_OF(delayable, struct zpt_scroll_data, flush_work);
    int16_t horizontal;
    int16_t vertical;
#if IS_ENABLED(CONFIG_ZMK_POINTING_TOOLS_STATE_TELEMETRY)
    struct zpt_scroll_flush_decision decision;
    struct zpt_state_sample sample = {0};
    enum zpt_state_level state_level = zpt_state_telemetry_level(data->telemetry_target_id);
#endif

    k_spinlock_key_t key = k_spin_lock(&data->lock);
    data->flush_armed = false;

    zpt_scroll_flush(&data->scroll, &data->settings, &horizontal, &vertical,
#if IS_ENABLED(CONFIG_ZMK_POINTING_TOOLS_STATE_TELEMETRY)
                     &decision
#else
                     NULL
#endif
    );
#if IS_ENABLED(CONFIG_ZMK_POINTING_TOOLS_STATE_TELEMETRY)
    sample = (struct zpt_state_sample){
        .timestamp_ms = k_uptime_get_32(),
        .target_id = data->telemetry_target_id,
        .target_kind = ZPT_TUNING_TARGET_SCROLL,
        .event = ZPT_STATE_EVENT_FLUSH,
        .intent = data->scroll.intent.intent,
        .flags = (horizontal != 0 || vertical != 0 ? ZPT_STATE_FLAG_OUTPUT : 0) |
                 (decision.discarded ? ZPT_STATE_FLAG_DISCARDED : 0) |
                 (decision.clipped_horizontal ? ZPT_STATE_FLAG_CLIPPED_HORIZONTAL : 0) |
                 (decision.clipped_vertical ? ZPT_STATE_FLAG_CLIPPED_VERTICAL : 0),
        .values = {horizontal, vertical, data->scroll.intent.horizontal_energy,
                   data->scroll.intent.vertical_energy, decision.undecided_x,
                   decision.undecided_y, data->scroll.pending_x, data->scroll.pending_y,
                   data->scroll.remainder_x, data->scroll.remainder_y},
    };
#endif
    k_spin_unlock(&data->lock, key);

#if IS_ENABLED(CONFIG_ZMK_POINTING_TOOLS_STATE_TELEMETRY)
    if (state_level == ZPT_STATE_LEVEL_VERBOSE || sample.flags != 0) {
        zpt_state_telemetry_submit(&sample);
    }
#endif

    if (horizontal == 0 && vertical == 0) {
        return;
    }

    /* This processor device is also a virtual input source. A dedicated ZMK
     * input listener consumes these two events as one synchronized HID frame. */
    input_report(data->dev, INPUT_EV_REL, INPUT_REL_HWHEEL, horizontal, false, K_NO_WAIT);
    input_report(data->dev, INPUT_EV_REL, INPUT_REL_WHEEL, vertical, true, K_NO_WAIT);
}

static void schedule_flush(struct zpt_scroll_data *data, uint16_t interval_ms) {
    if (!data->flush_armed) {
        data->flush_armed = true;
        k_work_schedule(&data->flush_work, K_MSEC(interval_ms));
    }
}

static void process_frame(const struct device *dev, int32_t x, int32_t y,
                          enum zpt_axis_policy policy) {
    struct zpt_scroll_data *data = dev->data;
    uint32_t now = k_uptime_get_32();
#if IS_ENABLED(CONFIG_ZMK_POINTING_TOOLS_STATE_TELEMETRY)
    enum zpt_state_level state_level = zpt_state_telemetry_level(data->telemetry_target_id);
    struct zpt_state_sample sample = {
        .timestamp_ms = now,
        .target_id = data->telemetry_target_id,
        .target_kind = ZPT_TUNING_TARGET_SCROLL,
        .event = ZPT_STATE_EVENT_FRAME,
        .values = {x, y},
    };
#endif

    k_spinlock_key_t key = k_spin_lock(&data->lock);

    bool suppress = data->settings.suppress_after_keypress_ms > 0U &&
                    atomic_get(&zpt_scroll_keypress_seen) != 0 &&
                    now - (uint32_t)atomic_get(&zpt_scroll_last_keypress_ms) <
                        data->settings.suppress_after_keypress_ms;
    struct zpt_scroll_decision decision =
        zpt_scroll_process(&data->scroll, &data->settings, x, y, policy, now, suppress);
    if (decision.suppressed) {
#if IS_ENABLED(CONFIG_ZMK_POINTING_TOOLS_STATE_TELEMETRY)
        sample.intent = decision.intent;
        sample.flags = ZPT_STATE_FLAG_SUPPRESSED |
                       (!data->state_suppressed ? ZPT_STATE_FLAG_SUPPRESSION_CHANGED : 0);
        data->state_suppressed = true;
#endif
        k_spin_unlock(&data->lock, key);
#if IS_ENABLED(CONFIG_ZMK_POINTING_TOOLS_STATE_TELEMETRY)
        if (state_level == ZPT_STATE_LEVEL_VERBOSE ||
            (sample.flags & ZPT_STATE_FLAG_SUPPRESSION_CHANGED) != 0) {
            zpt_state_telemetry_submit(&sample);
        }
#endif
        return;
    }

#if IS_ENABLED(CONFIG_ZMK_POINTING_TOOLS_STATE_TELEMETRY)
    if (decision.reset_for_idle) {
        sample.flags |= ZPT_STATE_FLAG_IDLE_RESET;
    }
    if (decision.intent != decision.previous_intent) {
        sample.flags |= ZPT_STATE_FLAG_INTENT_CHANGED;
    }
    if (data->state_suppressed) {
        sample.flags |= ZPT_STATE_FLAG_SUPPRESSION_CHANGED;
        data->state_suppressed = false;
    }
#endif
    schedule_flush(data, data->settings.report_interval_ms);
#if IS_ENABLED(CONFIG_ZMK_POINTING_TOOLS_STATE_TELEMETRY)
    sample.intent = decision.intent;
    sample.values[2] = data->scroll.intent.horizontal_energy;
    sample.values[3] = data->scroll.intent.vertical_energy;
    sample.values[4] = data->scroll.undecided_x;
    sample.values[5] = data->scroll.undecided_y;
    sample.values[6] = data->scroll.pending_x;
    sample.values[7] = data->scroll.pending_y;
    sample.values[8] = data->scroll.remainder_x;
    sample.values[9] = data->scroll.remainder_y;
#endif
    k_spin_unlock(&data->lock, key);
#if IS_ENABLED(CONFIG_ZMK_POINTING_TOOLS_STATE_TELEMETRY)
    if (state_level == ZPT_STATE_LEVEL_VERBOSE || sample.flags != 0) {
        zpt_state_telemetry_submit(&sample);
    }
#endif
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
    int32_t x = 0;
    int32_t y = 0;
    bool complete = event->sync;

    k_spinlock_key_t key = k_spin_lock(&data->lock);
    if (event->code == INPUT_REL_X) {
        data->frame_x = clamp_add(data->frame_x, event->value);
    } else {
        data->frame_y = clamp_add(data->frame_y, event->value);
    }
    if (complete) {
        x = data->frame_x;
        y = data->frame_y;
        data->frame_x = data->frame_y = 0;
    }
    k_spin_unlock(&data->lock, key);

    if (complete) {
        process_frame(dev, x, y, (enum zpt_axis_policy)param1);
    }

    /* ZMK intentionally consumes STOP inside a layer override before its HID
     * listener sees it. Zero the source event instead so semantic output does
     * not also leak through as cursor movement. */
    event->value = 0;
    return ZMK_INPUT_PROC_CONTINUE;
}

#if IS_ENABLED(CONFIG_ZMK_POINTING_TOOLS_RUNTIME_TUNING)

enum zpt_scroll_parameter_id {
    ZPT_SCROLL_SCALE_MULTIPLIER = 1,
    ZPT_SCROLL_SCALE_DIVISOR,
    ZPT_SCROLL_REPORT_INTERVAL,
    ZPT_SCROLL_IDLE_TIMEOUT,
    ZPT_SCROLL_INTENT_WINDOW,
    ZPT_SCROLL_ACTIVATION_DISTANCE,
    ZPT_SCROLL_ENGAGE_RATIO,
    ZPT_SCROLL_RELEASE_RATIO,
    ZPT_SCROLL_KEYPRESS_GUARD,
    ZPT_SCROLL_DISCARD_UNCLASSIFIED,
};

static const struct zpt_tuning_parameter zpt_scroll_parameters[] = {
    {.id = ZPT_SCROLL_SCALE_MULTIPLIER,
     .type = ZPT_TUNING_VALUE_INTEGER,
     .key = "scale-multiplier",
     .devicetree_property = "scale-multiplier",
     .minimum = 1,
     .maximum = 1024,
     .step = 1,
     .label = "Scale multiplier",
     .unit = "",
     .description = "The numerator of the scroll gain. Increasing it makes scrolling faster unless "
                    "the divisor also changes."},
    {.id = ZPT_SCROLL_SCALE_DIVISOR,
     .type = ZPT_TUNING_VALUE_INTEGER,
     .key = "scale-divisor",
     .devicetree_property = "scale-divisor",
     .minimum = 1,
     .maximum = 1024,
     .step = 1,
     .label = "Scale divisor",
     .unit = "",
     .description = "The denominator of the scroll gain. Increasing it makes scrolling slower "
                    "unless the multiplier also changes."},
    {.id = ZPT_SCROLL_REPORT_INTERVAL,
     .type = ZPT_TUNING_VALUE_INTEGER,
     .key = "report-interval-ms",
     .devicetree_property = "report-interval-ms",
     .minimum = 1,
     .maximum = 100,
     .step = 1,
     .label = "Report interval",
     .unit = "ms",
     .description = "How long movement is coalesced before wheel output. Lower values respond "
                    "sooner; higher values can feel smoother."},
    {.id = ZPT_SCROLL_IDLE_TIMEOUT,
     .type = ZPT_TUNING_VALUE_INTEGER,
     .key = "idle-timeout-ms",
     .devicetree_property = "idle-timeout-ms",
     .minimum = 10,
     .maximum = 2000,
     .step = 1,
     .label = "Gesture idle timeout",
     .unit = "ms",
     .description =
         "The motion-free gap that ends a gesture and releases its current axis classification."},
    {.id = ZPT_SCROLL_INTENT_WINDOW,
     .type = ZPT_TUNING_VALUE_INTEGER,
     .key = "intent-window-ms",
     .devicetree_property = "intent-window-ms",
     .minimum = 1,
     .maximum = 1000,
     .step = 1,
     .label = "Axis intent window",
     .unit = "ms",
     .description = "How long recent movement influences dominant-axis detection. Longer windows "
                    "are steadier but slower to recognize turns."},
    {.id = ZPT_SCROLL_ACTIVATION_DISTANCE,
     .type = ZPT_TUNING_VALUE_INTEGER,
     .key = "activation-distance",
     .devicetree_property = "activation-distance",
     .minimum = 1,
     .maximum = 10000,
     .step = 1,
     .label = "Activation distance",
     .unit = "counts",
     .description = "Accumulated motion required before adaptive axis classification. It is only a "
                    "dead zone when unclassified motion is discarded."},
    {.id = ZPT_SCROLL_ENGAGE_RATIO,
     .type = ZPT_TUNING_VALUE_INTEGER,
     .key = "engage-ratio-percent",
     .devicetree_property = "engage-ratio-percent",
     .minimum = 101,
     .maximum = 1000,
     .step = 1,
     .label = "Axis-lock engage ratio",
     .unit = "%",
     .description = "Dominant-to-minor movement ratio required to lock an axis. Higher values "
                    "require a straighter gesture; 300% means 3:1."},
    {.id = ZPT_SCROLL_RELEASE_RATIO,
     .type = ZPT_TUNING_VALUE_INTEGER,
     .key = "release-ratio-percent",
     .devicetree_property = "release-ratio-percent",
     .minimum = 100,
     .maximum = 999,
     .step = 1,
     .label = "Axis-lock release ratio",
     .unit = "%",
     .description = "Dominant-to-minor ratio used to retain an existing lock. Lower values hold "
                    "the lock more strongly. Keep this below engage."},
    {.id = ZPT_SCROLL_KEYPRESS_GUARD,
     .type = ZPT_TUNING_VALUE_INTEGER,
     .key = "suppress-after-keypress-ms",
     .devicetree_property = "suppress-after-keypress-ms",
     .minimum = 0,
     .maximum = 500,
     .step = 1,
     .label = "Physical keypress guard",
     .unit = "ms",
     .description = "Ignores trackball movement briefly after a physical key press to reject "
                    "typing vibration without an idle-motion dead zone."},
    {.id = ZPT_SCROLL_DISCARD_UNCLASSIFIED,
     .type = ZPT_TUNING_VALUE_BOOLEAN,
     .key = "discard-unclassified",
     .devicetree_property = "discard-unclassified",
     .minimum = 0,
     .maximum = 1,
     .step = 1,
     .label = "Discard unclassified motion",
     .unit = "",
     .description = "Drops motion that ends before adaptive classification. Useful for vibration "
                    "filtering, but can hide intentional fine movement."},
};

static int zpt_scroll_setting_get(const struct zpt_scroll_settings *settings, uint8_t parameter_id,
                                  int32_t *value) {
    switch (parameter_id) {
    case ZPT_SCROLL_SCALE_MULTIPLIER:
        *value = settings->scale_multiplier;
        break;
    case ZPT_SCROLL_SCALE_DIVISOR:
        *value = settings->scale_divisor;
        break;
    case ZPT_SCROLL_REPORT_INTERVAL:
        *value = settings->report_interval_ms;
        break;
    case ZPT_SCROLL_IDLE_TIMEOUT:
        *value = settings->idle_timeout_ms;
        break;
    case ZPT_SCROLL_INTENT_WINDOW:
        *value = settings->intent.window_ms;
        break;
    case ZPT_SCROLL_ACTIVATION_DISTANCE:
        *value = settings->intent.activation_distance;
        break;
    case ZPT_SCROLL_ENGAGE_RATIO:
        *value = settings->intent.engage_ratio_percent;
        break;
    case ZPT_SCROLL_RELEASE_RATIO:
        *value = settings->intent.release_ratio_percent;
        break;
    case ZPT_SCROLL_KEYPRESS_GUARD:
        *value = settings->suppress_after_keypress_ms;
        break;
    case ZPT_SCROLL_DISCARD_UNCLASSIFIED:
        *value = settings->discard_unclassified;
        break;
    default:
        return -ENOENT;
    }
    return 0;
}

static void zpt_scroll_reset_processing_locked(struct zpt_scroll_data *data) {
    data->frame_x = data->frame_y = 0;
    zpt_scroll_reset(&data->scroll, true);
#if IS_ENABLED(CONFIG_ZMK_POINTING_TOOLS_STATE_TELEMETRY)
    data->state_suppressed = false;
#endif
}

static int zpt_scroll_tuning_get(void *context, uint8_t parameter_id, bool compiled,
                                 int32_t *value) {
    const struct device *dev = context;
    const struct zpt_scroll_config *config = dev->config;
    struct zpt_scroll_data *data = dev->data;

    if (compiled) {
        return zpt_scroll_setting_get(&config->compiled, parameter_id, value);
    }

    k_spinlock_key_t key = k_spin_lock(&data->lock);
    int ret = zpt_scroll_setting_get(&data->settings, parameter_id, value);
    k_spin_unlock(&data->lock, key);
    return ret;
}

static int zpt_scroll_tuning_set_many(void *context, const struct zpt_tuning_value *values,
                                      size_t value_count, uint8_t *failed_parameter_id) {
    const struct device *dev = context;
    struct zpt_scroll_data *data = dev->data;
    int ret = 0;

    k_spinlock_key_t key = k_spin_lock(&data->lock);
    struct zpt_scroll_settings candidate = data->settings;
    for (size_t i = 0; i < value_count; i++) {
        if (failed_parameter_id != NULL) {
            *failed_parameter_id = values[i].parameter_id;
        }
        switch (values[i].parameter_id) {
        case ZPT_SCROLL_SCALE_MULTIPLIER:
            candidate.scale_multiplier = values[i].value;
            break;
        case ZPT_SCROLL_SCALE_DIVISOR:
            candidate.scale_divisor = values[i].value;
            break;
        case ZPT_SCROLL_REPORT_INTERVAL:
            candidate.report_interval_ms = values[i].value;
            break;
        case ZPT_SCROLL_IDLE_TIMEOUT:
            candidate.idle_timeout_ms = values[i].value;
            break;
        case ZPT_SCROLL_INTENT_WINDOW:
            candidate.intent.window_ms = values[i].value;
            break;
        case ZPT_SCROLL_ACTIVATION_DISTANCE:
            candidate.intent.activation_distance = values[i].value;
            break;
        case ZPT_SCROLL_ENGAGE_RATIO:
            candidate.intent.engage_ratio_percent = values[i].value;
            break;
        case ZPT_SCROLL_RELEASE_RATIO:
            candidate.intent.release_ratio_percent = values[i].value;
            break;
        case ZPT_SCROLL_KEYPRESS_GUARD:
            candidate.suppress_after_keypress_ms = values[i].value;
            break;
        case ZPT_SCROLL_DISCARD_UNCLASSIFIED:
            candidate.discard_unclassified = values[i].value != 0;
            break;
        default:
            ret = -ENOENT;
            break;
        }
        if (ret < 0) {
            break;
        }
    }

    if (ret == 0 &&
        candidate.intent.engage_ratio_percent <= candidate.intent.release_ratio_percent) {
        ret = -EINVAL;
    }
    if (ret == 0) {
        data->settings = candidate;
        zpt_scroll_reset_processing_locked(data);
    }
    k_spin_unlock(&data->lock, key);
    return ret;
}

static int zpt_scroll_tuning_reset(void *context) {
    const struct device *dev = context;
    const struct zpt_scroll_config *config = dev->config;
    struct zpt_scroll_data *data = dev->data;

    k_spinlock_key_t key = k_spin_lock(&data->lock);
    data->settings = config->compiled;
    zpt_scroll_reset_processing_locked(data);
    k_spin_unlock(&data->lock, key);
    return 0;
}

#endif /* CONFIG_ZMK_POINTING_TOOLS_RUNTIME_TUNING */

static const struct zmk_input_processor_driver_api zpt_scroll_driver_api = {
    .handle_event = zpt_scroll_handle_event,
};

static int zpt_scroll_init(const struct device *dev) {
    const struct zpt_scroll_config *config = dev->config;
    struct zpt_scroll_data *data = dev->data;
    data->dev = dev;
    data->settings = config->compiled;
    data->scroll.policy = ZPT_AXIS_POLICY_ADAPTIVE;
    zpt_scroll_reset(&data->scroll, true);
    k_work_init_delayable(&data->flush_work, zpt_scroll_flush_work);
#if IS_ENABLED(CONFIG_ZMK_POINTING_TOOLS_RUNTIME_TUNING)
    data->tuning_target = (struct zpt_tuning_target){
        .kind = ZPT_TUNING_TARGET_SCROLL,
        .stable_id = config->tuning_id,
        .label = config->tuning_label,
        .devicetree_path = config->devicetree_path,
        .parameters = zpt_scroll_parameters,
        .parameter_count = ARRAY_SIZE(zpt_scroll_parameters),
        .context = (void *)dev,
        .get = zpt_scroll_tuning_get,
        .set_many = zpt_scroll_tuning_set_many,
        .reset = zpt_scroll_tuning_reset,
    };
    int ret = zpt_tuning_register(&data->tuning_target);
    if (ret < 0) {
        return ret;
    }
#if IS_ENABLED(CONFIG_ZMK_POINTING_TOOLS_STATE_TELEMETRY)
    data->telemetry_target_id = ret;
#endif
#endif
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
        .compiled =                                                                                \
            {                                                                                      \
                .scale_multiplier = DT_INST_PROP(inst, scale_multiplier),                          \
                .scale_divisor = DT_INST_PROP(inst, scale_divisor),                                \
                .report_interval_ms = DT_INST_PROP(inst, report_interval_ms),                      \
                .idle_timeout_ms = DT_INST_PROP(inst, idle_timeout_ms),                            \
                .suppress_after_keypress_ms =                                                      \
                    DT_INST_PROP_OR(inst, suppress_after_keypress_ms, 0),                          \
                .discard_unclassified = DT_INST_PROP(inst, discard_unclassified),                  \
                .intent =                                                                          \
                    {                                                                              \
                        .engage_ratio_percent = DT_INST_PROP(inst, engage_ratio_percent),          \
                        .release_ratio_percent = DT_INST_PROP(inst, release_ratio_percent),        \
                        .activation_distance = DT_INST_PROP(inst, activation_distance),            \
                        .window_ms = DT_INST_PROP(inst, intent_window_ms),                         \
                    },                                                                             \
            },                                                                                     \
        .tuning_id = DT_INST_PROP(inst, tuning_id),                                                \
        .tuning_label = DT_INST_PROP(inst, tuning_label),                                          \
        .devicetree_path = DT_NODE_PATH(DT_DRV_INST(inst)),                                        \
    };                                                                                             \
    DEVICE_DT_INST_DEFINE(inst, zpt_scroll_init, NULL, &zpt_scroll_data_##inst,                    \
                          &zpt_scroll_config_##inst, POST_KERNEL,                                  \
                          CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &zpt_scroll_driver_api);

DT_INST_FOREACH_STATUS_OKAY(ZPT_SCROLL_DEFINE)
