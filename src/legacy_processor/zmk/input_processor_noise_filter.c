/* SPDX-License-Identifier: MIT */
#define DT_DRV_COMPAT measlesbagel_zpt_input_processor_noise_filter

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

#include <zmk/pointing_tools/legacy_processor/noise_filter.h>
#if IS_ENABLED(CONFIG_ZMK_POINTING_TOOLS_STATE_TELEMETRY)
#include <zmk/pointing_tools/observer/state.h>
#endif
#include <zmk/pointing_tools/service/tuning.h>

struct zpt_noise_filter_config {
    struct zpt_noise_filter_settings compiled;
    const char *tuning_id;
    const char *tuning_label;
    const char *devicetree_path;
};

struct zpt_noise_filter_data {
    const struct device *dev;
    struct k_spinlock lock;
    struct zpt_noise_filter_state filter;
    struct zpt_noise_filter_settings settings;
    int32_t frame_x;
    int32_t frame_y;
#if IS_ENABLED(CONFIG_ZMK_POINTING_TOOLS_RUNTIME_TUNING)
    struct zpt_tuning_target tuning_target;
#endif
#if IS_ENABLED(CONFIG_ZMK_POINTING_TOOLS_STATE_TELEMETRY)
    uint8_t telemetry_target_id;
    bool state_suppressed;
#endif
};

static atomic_t zpt_noise_keypress_seen;
static atomic_t zpt_noise_last_keypress_ms;

static int zpt_noise_key_activity_listener(const zmk_event_t *event) {
    const struct zmk_position_state_changed *position = as_zmk_position_state_changed(event);
    if (position != NULL && position->state) {
        atomic_set(&zpt_noise_last_keypress_ms, (atomic_val_t)k_uptime_get_32());
        atomic_set(&zpt_noise_keypress_seen, 1);
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(zpt_noise_key_activity, zpt_noise_key_activity_listener);
ZMK_SUBSCRIPTION(zpt_noise_key_activity, zmk_position_state_changed);

static int32_t clamp_add(int32_t left, int32_t right) {
    int64_t result = (int64_t)left + right;
    return (int32_t)CLAMP(result, INT32_MIN, INT32_MAX);
}

static void process_frame(const struct device *dev, int32_t x, int32_t y) {
    struct zpt_noise_filter_data *data = dev->data;
    uint32_t now = k_uptime_get_32();
    int32_t output_x;
    int32_t output_y;
#if IS_ENABLED(CONFIG_ZMK_POINTING_TOOLS_STATE_TELEMETRY)
    enum zpt_state_level state_level = zpt_state_telemetry_level(data->telemetry_target_id);
    struct zpt_state_sample sample = {
        .timestamp_ms = now,
        .target_id = data->telemetry_target_id,
        .target_kind = ZPT_TUNING_TARGET_NOISE_FILTER,
        .event = ZPT_STATE_EVENT_FRAME,
        .values = {x, y},
    };
#endif

    k_spinlock_key_t key = k_spin_lock(&data->lock);
    bool suppress = data->settings.suppress_after_keypress_ms > 0U &&
                    atomic_get(&zpt_noise_keypress_seen) != 0 &&
                    now - (uint32_t)atomic_get(&zpt_noise_last_keypress_ms) <
                        data->settings.suppress_after_keypress_ms;
    struct zpt_noise_filter_result result =
        zpt_noise_filter_update(&data->filter, &data->settings, x, y, now, suppress);
    output_x = result.x;
    output_y = result.y;

#if IS_ENABLED(CONFIG_ZMK_POINTING_TOOLS_STATE_TELEMETRY)
    sample.intent = result.phase;
    sample.flags = (result.reset_for_idle ? ZPT_STATE_FLAG_IDLE_RESET : 0) |
                   (result.suppressed ? ZPT_STATE_FLAG_SUPPRESSED : 0) |
                   (result.qualified ? ZPT_STATE_FLAG_QUALIFIED : 0) |
                   (result.discarded ? ZPT_STATE_FLAG_PENDING_DISCARDED : 0);
    if (result.suppressed != data->state_suppressed) {
        sample.flags |= ZPT_STATE_FLAG_SUPPRESSION_CHANGED;
        data->state_suppressed = result.suppressed;
    }
    if (result.reset_for_timeout) {
        sample.flags |= ZPT_STATE_FLAG_PENDING_DISCARDED;
    }
    sample.values[2] = output_x;
    sample.values[3] = output_y;
    sample.values[4] = data->filter.pending_x;
    sample.values[5] = data->filter.pending_y;
    sample.values[6] = MIN(data->filter.sample_count, INT32_MAX);
    sample.values[7] = MIN(data->filter.squared_energy, INT32_MAX);
    sample.values[8] = result.phase;
    sample.values[9] = data->settings.enabled;
#endif
    k_spin_unlock(&data->lock, key);

#if IS_ENABLED(CONFIG_ZMK_POINTING_TOOLS_STATE_TELEMETRY)
    if (state_level == ZPT_STATE_LEVEL_VERBOSE || sample.flags != 0) {
        zpt_state_telemetry_submit(&sample);
    }
#endif

    if (output_x == 0 && output_y == 0) {
        return;
    }
    input_report(data->dev, INPUT_EV_REL, INPUT_REL_X, output_x, false, K_NO_WAIT);
    input_report(data->dev, INPUT_EV_REL, INPUT_REL_Y, output_y, true, K_NO_WAIT);
}

static int zpt_noise_filter_handle_event(const struct device *dev, struct input_event *event,
                                         uint32_t param1, uint32_t param2,
                                         struct zmk_input_processor_state *state) {
    ARG_UNUSED(param1);
    ARG_UNUSED(param2);
    ARG_UNUSED(state);

    if (event->type != INPUT_EV_REL || (event->code != INPUT_REL_X && event->code != INPUT_REL_Y)) {
        return ZMK_INPUT_PROC_CONTINUE;
    }

    struct zpt_noise_filter_data *data = dev->data;
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

    event->value = 0;
    if (complete) {
        process_frame(dev, x, y);
    }
    return ZMK_INPUT_PROC_CONTINUE;
}

#if IS_ENABLED(CONFIG_ZMK_POINTING_TOOLS_RUNTIME_TUNING)

enum zpt_noise_filter_parameter_id {
    ZPT_NOISE_FILTER_ENABLED = 1,
    ZPT_NOISE_FILTER_ACTIVATION_DISTANCE,
    ZPT_NOISE_FILTER_COHERENCE,
    ZPT_NOISE_FILTER_QUALIFICATION_TIMEOUT,
    ZPT_NOISE_FILTER_IDLE_TIMEOUT,
    ZPT_NOISE_FILTER_KEYPRESS_GUARD,
};

static const struct zpt_tuning_parameter zpt_noise_filter_parameters[] = {
    {.id = ZPT_NOISE_FILTER_ENABLED,
     .type = ZPT_TUNING_VALUE_BOOLEAN,
     .key = "enabled",
     .devicetree_property = "enabled",
     .minimum = 0,
     .maximum = 1,
     .step = 1,
     .label = "Enabled",
     .unit = "",
     .description = "Enable movement qualification. Disabled passes every complete vector "
                    "unchanged."},
    {.id = ZPT_NOISE_FILTER_ACTIVATION_DISTANCE,
     .type = ZPT_TUNING_VALUE_INTEGER,
     .key = "activation-distance",
     .devicetree_property = "activation-distance",
     .minimum = 1,
     .maximum = 1000,
     .step = 1,
     .label = "Activation distance",
     .unit = "counts",
     .description = "Net radial movement required to classify pending input as intentional. "
                    "Buffered distance is emitted rather than lost after qualification."},
    {.id = ZPT_NOISE_FILTER_COHERENCE,
     .type = ZPT_TUNING_VALUE_INTEGER,
     .key = "coherence-percent",
     .devicetree_property = "coherence-percent",
     .minimum = 0,
     .maximum = 100,
     .step = 1,
     .label = "Directional coherence",
     .unit = "%",
     .description = "How consistently pending vectors must point in one direction. Higher "
                    "values reject alternating jitter; zero disables this check."},
    {.id = ZPT_NOISE_FILTER_QUALIFICATION_TIMEOUT,
     .type = ZPT_TUNING_VALUE_INTEGER,
     .key = "qualification-timeout-ms",
     .devicetree_property = "qualification-timeout-ms",
     .minimum = 10,
     .maximum = 2000,
     .step = 1,
     .label = "Qualification window",
     .unit = "ms",
     .description = "Maximum time pending movement may accumulate before it is discarded and "
                    "a fresh qualification window begins."},
    {.id = ZPT_NOISE_FILTER_IDLE_TIMEOUT,
     .type = ZPT_TUNING_VALUE_INTEGER,
     .key = "idle-timeout-ms",
     .devicetree_property = "idle-timeout-ms",
     .minimum = 10,
     .maximum = 2000,
     .step = 1,
     .label = "Active idle timeout",
     .unit = "ms",
     .description = "Motion-free gap that ends an active gesture and requires movement to "
                    "qualify again."},
    {.id = ZPT_NOISE_FILTER_KEYPRESS_GUARD,
     .type = ZPT_TUNING_VALUE_INTEGER,
     .key = "suppress-after-keypress-ms",
     .devicetree_property = "suppress-after-keypress-ms",
     .minimum = 0,
     .maximum = 500,
     .step = 1,
     .label = "Physical keypress guard",
     .unit = "ms",
     .description = "Discard movement briefly after a physical key press. Zero relies only on "
                    "radial and coherence qualification."},
};

static int setting_get(const struct zpt_noise_filter_settings *settings, uint8_t parameter_id,
                       int32_t *value) {
    switch (parameter_id) {
    case ZPT_NOISE_FILTER_ENABLED:
        *value = settings->enabled;
        break;
    case ZPT_NOISE_FILTER_ACTIVATION_DISTANCE:
        *value = settings->activation_distance;
        break;
    case ZPT_NOISE_FILTER_COHERENCE:
        *value = settings->coherence_percent;
        break;
    case ZPT_NOISE_FILTER_QUALIFICATION_TIMEOUT:
        *value = settings->qualification_timeout_ms;
        break;
    case ZPT_NOISE_FILTER_IDLE_TIMEOUT:
        *value = settings->idle_timeout_ms;
        break;
    case ZPT_NOISE_FILTER_KEYPRESS_GUARD:
        *value = settings->suppress_after_keypress_ms;
        break;
    default:
        return -ENOENT;
    }
    return 0;
}

static int tuning_get(void *context, uint8_t parameter_id, bool compiled, int32_t *value) {
    const struct device *dev = context;
    const struct zpt_noise_filter_config *config = dev->config;
    struct zpt_noise_filter_data *data = dev->data;
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
    struct zpt_noise_filter_data *data = dev->data;
    int ret = 0;
    k_spinlock_key_t key = k_spin_lock(&data->lock);
    struct zpt_noise_filter_settings candidate = data->settings;
    for (size_t index = 0; index < value_count; index++) {
        if (failed_parameter_id != NULL) {
            *failed_parameter_id = values[index].parameter_id;
        }
        switch (values[index].parameter_id) {
        case ZPT_NOISE_FILTER_ENABLED:
            candidate.enabled = values[index].value != 0;
            break;
        case ZPT_NOISE_FILTER_ACTIVATION_DISTANCE:
            candidate.activation_distance = values[index].value;
            break;
        case ZPT_NOISE_FILTER_COHERENCE:
            candidate.coherence_percent = values[index].value;
            break;
        case ZPT_NOISE_FILTER_QUALIFICATION_TIMEOUT:
            candidate.qualification_timeout_ms = values[index].value;
            break;
        case ZPT_NOISE_FILTER_IDLE_TIMEOUT:
            candidate.idle_timeout_ms = values[index].value;
            break;
        case ZPT_NOISE_FILTER_KEYPRESS_GUARD:
            candidate.suppress_after_keypress_ms = values[index].value;
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
        data->frame_x = data->frame_y = 0;
        zpt_noise_filter_reset(&data->filter);
#if IS_ENABLED(CONFIG_ZMK_POINTING_TOOLS_STATE_TELEMETRY)
        data->state_suppressed = false;
#endif
    }
    k_spin_unlock(&data->lock, key);
    return ret;
}

static int tuning_reset(void *context) {
    const struct device *dev = context;
    const struct zpt_noise_filter_config *config = dev->config;
    struct zpt_noise_filter_data *data = dev->data;
    k_spinlock_key_t key = k_spin_lock(&data->lock);
    data->settings = config->compiled;
    data->frame_x = data->frame_y = 0;
    zpt_noise_filter_reset(&data->filter);
#if IS_ENABLED(CONFIG_ZMK_POINTING_TOOLS_STATE_TELEMETRY)
    data->state_suppressed = false;
#endif
    k_spin_unlock(&data->lock, key);
    return 0;
}

#endif

static const struct zmk_input_processor_driver_api zpt_noise_filter_driver_api = {
    .handle_event = zpt_noise_filter_handle_event,
};

static int zpt_noise_filter_init(const struct device *dev) {
    const struct zpt_noise_filter_config *config = dev->config;
    struct zpt_noise_filter_data *data = dev->data;
    data->dev = dev;
    data->settings = config->compiled;
    zpt_noise_filter_reset(&data->filter);
#if IS_ENABLED(CONFIG_ZMK_POINTING_TOOLS_RUNTIME_TUNING)
    data->tuning_target = (struct zpt_tuning_target){
        .kind = ZPT_TUNING_TARGET_NOISE_FILTER,
        .stable_id = config->tuning_id,
        .label = config->tuning_label,
        .devicetree_path = config->devicetree_path,
        .parameters = zpt_noise_filter_parameters,
        .parameter_count = ARRAY_SIZE(zpt_noise_filter_parameters),
        .context = (void *)dev,
        .get = tuning_get,
        .set_many = tuning_set_many,
        .reset = tuning_reset,
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

#define ZPT_NOISE_FILTER_DEFINE(inst)                                                              \
    BUILD_ASSERT(DT_INST_PROP(inst, activation_distance) > 0,                                      \
                 "activation-distance must be positive");                                          \
    BUILD_ASSERT(DT_INST_PROP(inst, coherence_percent) <= 100,                                     \
                 "coherence-percent must not exceed 100");                                         \
    BUILD_ASSERT(DT_INST_PROP(inst, qualification_timeout_ms) > 0,                                 \
                 "qualification-timeout-ms must be positive");                                     \
    BUILD_ASSERT(DT_INST_PROP(inst, idle_timeout_ms) > 0, "idle-timeout-ms must be positive");     \
    static struct zpt_noise_filter_data zpt_noise_filter_data_##inst;                              \
    static const struct zpt_noise_filter_config zpt_noise_filter_config_##inst = {                 \
        .compiled =                                                                                \
            {                                                                                      \
                .enabled = DT_INST_PROP(inst, enabled),                                            \
                .activation_distance = DT_INST_PROP(inst, activation_distance),                    \
                .coherence_percent = DT_INST_PROP(inst, coherence_percent),                        \
                .qualification_timeout_ms = DT_INST_PROP(inst, qualification_timeout_ms),          \
                .idle_timeout_ms = DT_INST_PROP(inst, idle_timeout_ms),                            \
                .suppress_after_keypress_ms =                                                      \
                    DT_INST_PROP_OR(inst, suppress_after_keypress_ms, 0),                          \
            },                                                                                     \
        .tuning_id = DT_INST_PROP(inst, tuning_id),                                                \
        .tuning_label = DT_INST_PROP(inst, tuning_label),                                          \
        .devicetree_path = DT_NODE_PATH(DT_DRV_INST(inst)),                                        \
    };                                                                                             \
    DEVICE_DT_INST_DEFINE(inst, zpt_noise_filter_init, NULL, &zpt_noise_filter_data_##inst,        \
                          &zpt_noise_filter_config_##inst, POST_KERNEL,                            \
                          CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &zpt_noise_filter_driver_api);

DT_INST_FOREACH_STATUS_OKAY(ZPT_NOISE_FILTER_DEFINE)
