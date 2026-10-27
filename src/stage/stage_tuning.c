/* SPDX-License-Identifier: MIT */

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include <zmk/pointing_tools/core/signal.h>
#include <zmk/pointing_tools/stage/axis_constraint.h>
#include <zmk/pointing_tools/stage/axis_intent.h>
#include <zmk/pointing_tools/stage/cursor_acceleration.h>
#include <zmk/pointing_tools/stage/cursor_coalescer.h>
#include <zmk/pointing_tools/stage/cursor_transfer.h>
#include <zmk/pointing_tools/stage/motion_gate/coherent_displacement.h>
#include <zmk/pointing_tools/stage/scroll_batcher.h>
#include <zmk/pointing_tools/stage/stage_tuning.h>
#include <zmk/pointing_tools/stage/text_nav.h>

#define ZPT_ARRAY_SIZE(array) (sizeof(array) / sizeof((array)[0]))

int32_t zpt_stage_tuning_fixed_millimeters_to_micrometers(int64_t value) {
    const int64_t scaled = value * 1000;
    const int64_t half = ZPT_FIXED_ONE / 2;
    const int64_t micrometers =
        scaled >= 0 ? (scaled + half) / ZPT_FIXED_ONE : (scaled - half) / ZPT_FIXED_ONE;
    if (micrometers > INT32_MAX) {
        return INT32_MAX;
    }
    return micrometers < INT32_MIN ? INT32_MIN : (int32_t)micrometers;
}

int32_t zpt_stage_tuning_fixed_per_millimeter_to_per_meter(int64_t value) {
    /* Same rounding: per metre is micro-per-millimetre scaled by 10^6/10^3. */
    return zpt_stage_tuning_fixed_millimeters_to_micrometers(value);
}

int zpt_stage_tuning_prepare(const struct zpt_stage_tuning_ops *ops, const void *current,
                             const struct zpt_tuning_value *values, size_t value_count,
                             void *candidate, uint8_t *failed_parameter_id) {
    if (ops == NULL || current == NULL || candidate == NULL ||
        (values == NULL && value_count > 0U)) {
        return -EINVAL;
    }
    memcpy(candidate, current, ops->config_size);
    for (size_t i = 0; i < value_count; i++) {
        int ret = ops->put(candidate, values[i].parameter_id, values[i].value);
        if (ret < 0) {
            if (failed_parameter_id != NULL) {
                *failed_parameter_id = values[i].parameter_id;
            }
            return ret;
        }
    }
    uint8_t failed = UINT8_MAX;
    int ret = ops->validate == NULL ? 0 : ops->validate(candidate, &failed);
    if (ret < 0 && failed_parameter_id != NULL) {
        *failed_parameter_id = failed;
    }
    return ret;
}

int zpt_stage_tuning_prepare_reset(const struct zpt_stage_tuning_ops *ops, const void *compiled,
                                   const void *current, void *candidate) {
    if (ops == NULL || compiled == NULL || current == NULL || candidate == NULL) {
        return -EINVAL;
    }
    memcpy(candidate, current, ops->config_size);
    for (size_t i = 0; i < ops->parameter_count; i++) {
        int32_t value;
        int ret = ops->get(compiled, ops->parameters[i].id, &value);
        if (ret == 0) {
            ret = ops->put(candidate, ops->parameters[i].id, value);
        }
        if (ret < 0) {
            return ret;
        }
    }
    return 0;
}

/* Candidate configs are staged on the stack, aligned for any config field. */
union zpt_stage_tuning_candidate {
    unsigned char bytes[ZPT_STAGE_TUNING_MAX_CONFIG_SIZE];
    int64_t align_integer;
    const void *align_pointer;
};

static int binding_get(void *context, uint8_t parameter_id, bool compiled, int32_t *value) {
    const struct zpt_stage_tuning_binding *binding = context;
    return binding->ops->get(compiled ? binding->compiled : binding->current, parameter_id, value);
}

static int binding_set_many(void *context, const struct zpt_tuning_value *values,
                            size_t value_count, uint8_t *failed_parameter_id) {
    struct zpt_stage_tuning_binding *binding = context;
    union zpt_stage_tuning_candidate candidate;
    int ret = zpt_stage_tuning_prepare(binding->ops, binding->current, values, value_count,
                                       candidate.bytes, failed_parameter_id);
    if (ret < 0) {
        return ret;
    }
    return binding->apply(binding->current, candidate.bytes, binding->ops->config_size);
}

static int binding_reset(void *context) {
    struct zpt_stage_tuning_binding *binding = context;
    union zpt_stage_tuning_candidate candidate;
    int ret = zpt_stage_tuning_prepare_reset(binding->ops, binding->compiled, binding->current,
                                             candidate.bytes);
    if (ret < 0) {
        return ret;
    }
    return binding->apply(binding->current, candidate.bytes, binding->ops->config_size);
}

int zpt_stage_tuning_target_init(struct zpt_tuning_target *target,
                                 struct zpt_stage_tuning_binding *binding, const char *stable_id,
                                 const char *devicetree_path) {
    if (target == NULL || binding == NULL || binding->ops == NULL || binding->compiled == NULL ||
        binding->current == NULL || binding->apply == NULL ||
        binding->ops->config_size > sizeof(union zpt_stage_tuning_candidate)) {
        return -EINVAL;
    }
    *target = (struct zpt_tuning_target){
        .kind = ZPT_TUNING_TARGET_PIPELINE_STAGE,
        .stable_id = stable_id,
        .label = stable_id,
        .devicetree_path = devicetree_path,
        .parameters = binding->ops->parameters,
        .parameter_count = binding->ops->parameter_count,
        .context = binding,
        .get = binding_get,
        .set_many = binding_set_many,
        .reset = binding_reset,
    };
    return 0;
}

/* --- Coherent-displacement motion gate -------------------------------- */

enum {
    GATE_ENABLED = 0,
    GATE_ACTIVATION_DISTANCE,
    GATE_COHERENCE,
    GATE_QUALIFICATION_TIMEOUT,
    GATE_IDLE_TIMEOUT,
};

static const struct zpt_tuning_parameter gate_parameters[] = {
    {GATE_ENABLED, ZPT_TUNING_VALUE_BOOLEAN, "enabled", "enabled", 0, 1, 1, "Enabled", "",
     "Hold back motion until it moves far enough in a consistent direction; off passes every "
     "frame through."},
    {GATE_ACTIVATION_DISTANCE, ZPT_TUNING_VALUE_INTEGER, "activation-distance",
     "activation-distance-micrometers", 1, 20000, 1, "Activation distance", "µm",
     "Ball travel needed before motion starts. Raise it if resting your hand nudges the cursor."},
    {GATE_COHERENCE, ZPT_TUNING_VALUE_INTEGER, "coherence", "coherence-percent", 0, 100, 1,
     "Coherence", "%",
     "How consistently held-back motion must point one way to count as intended."},
    {GATE_QUALIFICATION_TIMEOUT, ZPT_TUNING_VALUE_INTEGER, "qualification-timeout",
     "qualification-timeout-ms", 1, 5000, 1, "Qualification timeout", "ms",
     "How long held-back motion may take to qualify before it is discarded."},
    {GATE_IDLE_TIMEOUT, ZPT_TUNING_VALUE_INTEGER, "idle-timeout", "idle-timeout-ms", 1, 5000, 1,
     "Idle timeout", "ms", "Pause after which the gate closes and motion must qualify again."},
};

static int gate_get(const void *config, uint8_t parameter_id, int32_t *value) {
    const struct zpt_coherent_displacement_settings *settings =
        &((const struct zpt_coherent_displacement_stage_config *)config)->settings;
    switch (parameter_id) {
    case GATE_ENABLED:
        *value = settings->enabled ? 1 : 0;
        return 0;
    case GATE_ACTIVATION_DISTANCE:
        *value = zpt_stage_tuning_fixed_millimeters_to_micrometers(settings->activation_distance);
        return 0;
    case GATE_COHERENCE:
        *value = settings->coherence_percent;
        return 0;
    case GATE_QUALIFICATION_TIMEOUT:
        *value = settings->qualification_timeout_ms;
        return 0;
    case GATE_IDLE_TIMEOUT:
        *value = settings->idle_timeout_ms;
        return 0;
    default:
        return -ENOENT;
    }
}

static int gate_put(void *config, uint8_t parameter_id, int32_t value) {
    struct zpt_coherent_displacement_settings *settings =
        &((struct zpt_coherent_displacement_stage_config *)config)->settings;
    switch (parameter_id) {
    case GATE_ENABLED:
        settings->enabled = value != 0;
        return 0;
    case GATE_ACTIVATION_DISTANCE:
        settings->activation_distance = ZPT_MICROMETERS_TO_FIXED_MILLIMETERS(value);
        return 0;
    case GATE_COHERENCE:
        settings->coherence_percent = (uint16_t)value;
        return 0;
    case GATE_QUALIFICATION_TIMEOUT:
        settings->qualification_timeout_ms = (uint16_t)value;
        return 0;
    case GATE_IDLE_TIMEOUT:
        settings->idle_timeout_ms = (uint16_t)value;
        return 0;
    default:
        return -ENOENT;
    }
}

const struct zpt_stage_tuning_ops zpt_coherent_displacement_tuning_ops = {
    .parameters = gate_parameters,
    .parameter_count = ZPT_ARRAY_SIZE(gate_parameters),
    .config_size = sizeof(struct zpt_coherent_displacement_stage_config),
    .get = gate_get,
    .put = gate_put,
};

/* --- Cursor transfer --------------------------------------------------- */

enum {
    TRANSFER_SCALE_MULTIPLIER = 0,
    TRANSFER_SCALE_DIVISOR,
};

static const struct zpt_tuning_parameter transfer_parameters[] = {
    {TRANSFER_SCALE_MULTIPLIER, ZPT_TUNING_VALUE_INTEGER, "scale-multiplier", "scale-multiplier", 0,
     1000, 1, "Gain multiplier", "×",
     "Cursor gain numerator: pointer distance is ball distance × multiplier ÷ divisor."},
    {TRANSFER_SCALE_DIVISOR, ZPT_TUNING_VALUE_INTEGER, "scale-divisor", "scale-divisor", 1, 1000, 1,
     "Gain divisor", "÷", "Cursor gain denominator."},
};

static int transfer_get(const void *config, uint8_t parameter_id, int32_t *value) {
    const struct zpt_cursor_transfer_config *transfer = config;
    switch (parameter_id) {
    case TRANSFER_SCALE_MULTIPLIER:
        *value = transfer->scale_multiplier;
        return 0;
    case TRANSFER_SCALE_DIVISOR:
        *value = transfer->scale_divisor;
        return 0;
    default:
        return -ENOENT;
    }
}

static int transfer_put(void *config, uint8_t parameter_id, int32_t value) {
    struct zpt_cursor_transfer_config *transfer = config;
    switch (parameter_id) {
    case TRANSFER_SCALE_MULTIPLIER:
        transfer->scale_multiplier = (uint16_t)value;
        return 0;
    case TRANSFER_SCALE_DIVISOR:
        transfer->scale_divisor = (uint16_t)value;
        return 0;
    default:
        return -ENOENT;
    }
}

const struct zpt_stage_tuning_ops zpt_cursor_transfer_tuning_ops = {
    .parameters = transfer_parameters,
    .parameter_count = ZPT_ARRAY_SIZE(transfer_parameters),
    .config_size = sizeof(struct zpt_cursor_transfer_config),
    .get = transfer_get,
    .put = transfer_put,
};

/* --- Scroll batcher ---------------------------------------------------- */

enum {
    SCROLL_STEPS_PER_METER = 0,
    SCROLL_REPORT_INTERVAL,
};

static const struct zpt_tuning_parameter scroll_parameters[] = {
    {SCROLL_STEPS_PER_METER, ZPT_TUNING_VALUE_INTEGER, "steps-per-meter", "steps-per-meter", 1,
     100000, 1, "Scroll speed", "steps/m",
     "Wheel steps per metre of ball travel along the engaged axis."},
    {SCROLL_REPORT_INTERVAL, ZPT_TUNING_VALUE_INTEGER, "report-interval", "report-interval-ms", 1,
     1000, 1, "Report interval", "ms",
     "How often accumulated scroll is sent. Longer intervals send fewer, larger steps."},
};

static int scroll_get(const void *config, uint8_t parameter_id, int32_t *value) {
    const struct zpt_scroll_batcher_config *scroll = config;
    switch (parameter_id) {
    case SCROLL_STEPS_PER_METER:
        *value = zpt_stage_tuning_fixed_per_millimeter_to_per_meter(scroll->steps_per_millimeter);
        return 0;
    case SCROLL_REPORT_INTERVAL:
        *value = scroll->report_interval_ms;
        return 0;
    default:
        return -ENOENT;
    }
}

static int scroll_put(void *config, uint8_t parameter_id, int32_t value) {
    struct zpt_scroll_batcher_config *scroll = config;
    switch (parameter_id) {
    case SCROLL_STEPS_PER_METER:
        scroll->steps_per_millimeter = ZPT_PER_METER_TO_FIXED_PER_MILLIMETER(value);
        return 0;
    case SCROLL_REPORT_INTERVAL:
        scroll->report_interval_ms = (uint16_t)value;
        return 0;
    default:
        return -ENOENT;
    }
}

const struct zpt_stage_tuning_ops zpt_scroll_batcher_tuning_ops = {
    .parameters = scroll_parameters,
    .parameter_count = ZPT_ARRAY_SIZE(scroll_parameters),
    .config_size = sizeof(struct zpt_scroll_batcher_config),
    .get = scroll_get,
    .put = scroll_put,
};

/* --- Text navigation --------------------------------------------------- */

enum {
    TEXT_HORIZONTAL_THRESHOLD = 0,
    TEXT_VERTICAL_THRESHOLD,
    TEXT_IDLE_TIMEOUT,
};

static const struct zpt_tuning_parameter text_parameters[] = {
    {TEXT_HORIZONTAL_THRESHOLD, ZPT_TUNING_VALUE_INTEGER, "horizontal-threshold",
     "horizontal-threshold-micrometers", 1, 50000, 1, "Horizontal step", "µm",
     "Ball travel per left or right key tap."},
    {TEXT_VERTICAL_THRESHOLD, ZPT_TUNING_VALUE_INTEGER, "vertical-threshold",
     "vertical-threshold-micrometers", 1, 50000, 1, "Vertical step", "µm",
     "Ball travel per up or down key tap."},
    {TEXT_IDLE_TIMEOUT, ZPT_TUNING_VALUE_INTEGER, "idle-timeout", "idle-timeout-ms", 1, 5000, 1,
     "Idle timeout", "ms", "Pause after which partial travel toward the next tap is dropped."},
};

static int text_get(const void *config, uint8_t parameter_id, int32_t *value) {
    const struct zpt_text_nav_config *text = config;
    switch (parameter_id) {
    case TEXT_HORIZONTAL_THRESHOLD:
        *value = zpt_stage_tuning_fixed_millimeters_to_micrometers(text->horizontal_threshold);
        return 0;
    case TEXT_VERTICAL_THRESHOLD:
        *value = zpt_stage_tuning_fixed_millimeters_to_micrometers(text->vertical_threshold);
        return 0;
    case TEXT_IDLE_TIMEOUT:
        *value = text->idle_timeout_ms;
        return 0;
    default:
        return -ENOENT;
    }
}

static int text_put(void *config, uint8_t parameter_id, int32_t value) {
    struct zpt_text_nav_config *text = config;
    switch (parameter_id) {
    case TEXT_HORIZONTAL_THRESHOLD:
        text->horizontal_threshold = ZPT_MICROMETERS_TO_FIXED_MILLIMETERS(value);
        return 0;
    case TEXT_VERTICAL_THRESHOLD:
        text->vertical_threshold = ZPT_MICROMETERS_TO_FIXED_MILLIMETERS(value);
        return 0;
    case TEXT_IDLE_TIMEOUT:
        text->idle_timeout_ms = (uint16_t)value;
        return 0;
    default:
        return -ENOENT;
    }
}

const struct zpt_stage_tuning_ops zpt_text_nav_tuning_ops = {
    .parameters = text_parameters,
    .parameter_count = ZPT_ARRAY_SIZE(text_parameters),
    .config_size = sizeof(struct zpt_text_nav_config),
    .get = text_get,
    .put = text_put,
};

/* --- Axis intent --------------------------------------------------------- */

enum {
    INTENT_ENGAGE_RATIO = 0,
    INTENT_RELEASE_RATIO,
    INTENT_ACTIVATION_DISTANCE,
    INTENT_WINDOW,
    INTENT_IDLE_TIMEOUT,
};

static const struct zpt_tuning_parameter intent_parameters[] = {
    {INTENT_ENGAGE_RATIO, ZPT_TUNING_VALUE_INTEGER, "engage-ratio", "engage-ratio-percent", 100,
     1000, 1, "Engage ratio", "%",
     "How much the major axis must dominate before motion locks to it."},
    {INTENT_RELEASE_RATIO, ZPT_TUNING_VALUE_INTEGER, "release-ratio", "release-ratio-percent", 100,
     1000, 1, "Release ratio", "%",
     "Dominance below which a locked axis is released. Keep it at or below the engage ratio."},
    {INTENT_ACTIVATION_DISTANCE, ZPT_TUNING_VALUE_INTEGER, "activation-distance",
     "activation-distance-micrometers", 1, 20000, 1, "Activation distance", "µm",
     "Ball travel needed before an axis is chosen."},
    {INTENT_WINDOW, ZPT_TUNING_VALUE_INTEGER, "window", "window-ms", 1, 2000, 1, "Window", "ms",
     "How far back motion counts toward choosing an axis."},
    {INTENT_IDLE_TIMEOUT, ZPT_TUNING_VALUE_INTEGER, "idle-timeout", "idle-timeout-ms", 0, 5000, 1,
     "Idle timeout", "ms", "Pause after which the axis choice resets; 0 never resets."},
};

static int intent_get(const void *config, uint8_t parameter_id, int32_t *value) {
    const struct zpt_axis_intent_stage_config *intent = config;
    switch (parameter_id) {
    case INTENT_ENGAGE_RATIO:
        *value = intent->settings.engage_ratio_percent;
        return 0;
    case INTENT_RELEASE_RATIO:
        *value = intent->settings.release_ratio_percent;
        return 0;
    case INTENT_ACTIVATION_DISTANCE:
        *value =
            zpt_stage_tuning_fixed_millimeters_to_micrometers(intent->settings.activation_distance);
        return 0;
    case INTENT_WINDOW:
        *value = intent->settings.window_ms;
        return 0;
    case INTENT_IDLE_TIMEOUT:
        *value = intent->idle_timeout_ms;
        return 0;
    default:
        return -ENOENT;
    }
}

static int intent_put(void *config, uint8_t parameter_id, int32_t value) {
    struct zpt_axis_intent_stage_config *intent = config;
    switch (parameter_id) {
    case INTENT_ENGAGE_RATIO:
        intent->settings.engage_ratio_percent = (uint16_t)value;
        return 0;
    case INTENT_RELEASE_RATIO:
        intent->settings.release_ratio_percent = (uint16_t)value;
        return 0;
    case INTENT_ACTIVATION_DISTANCE:
        intent->settings.activation_distance = ZPT_MICROMETERS_TO_FIXED_MILLIMETERS(value);
        return 0;
    case INTENT_WINDOW:
        intent->settings.window_ms = (uint16_t)value;
        return 0;
    case INTENT_IDLE_TIMEOUT:
        intent->idle_timeout_ms = (uint16_t)value;
        return 0;
    default:
        return -ENOENT;
    }
}

static int intent_validate(const void *config, uint8_t *failed_parameter_id) {
    const struct zpt_axis_intent_stage_config *intent = config;
    /* A release ratio above the engage ratio would drop an axis the moment
     * it engaged. */
    if (intent->settings.release_ratio_percent > intent->settings.engage_ratio_percent) {
        *failed_parameter_id = INTENT_RELEASE_RATIO;
        return -EINVAL;
    }
    return 0;
}

const struct zpt_stage_tuning_ops zpt_axis_intent_tuning_ops = {
    .parameters = intent_parameters,
    .parameter_count = ZPT_ARRAY_SIZE(intent_parameters),
    .config_size = sizeof(struct zpt_axis_intent_stage_config),
    .get = intent_get,
    .put = intent_put,
    .validate = intent_validate,
};

/* --- Axis constraint ----------------------------------------------------- */

enum {
    CONSTRAINT_DISCARD_UNCLASSIFIED = 0,
    CONSTRAINT_IDLE_TIMEOUT,
    CONSTRAINT_FOLD_INTERVAL,
};

static const struct zpt_tuning_parameter constraint_parameters[] = {
    {CONSTRAINT_DISCARD_UNCLASSIFIED, ZPT_TUNING_VALUE_BOOLEAN, "discard-unclassified",
     "discard-unclassified", 0, 1, 1, "Discard undecided motion", "",
     "Drop motion held while no axis was chosen instead of releasing it in both axes."},
    {CONSTRAINT_IDLE_TIMEOUT, ZPT_TUNING_VALUE_INTEGER, "idle-timeout", "idle-timeout-ms", 0, 5000,
     1, "Idle timeout", "ms",
     "Pause after which held undecided motion is released or dropped; 0 never times out."},
    {CONSTRAINT_FOLD_INTERVAL, ZPT_TUNING_VALUE_INTEGER, "fold-interval", "fold-interval-ms", 0,
     1000, 1, "Fold interval", "ms",
     "How often held undecided motion is released or dropped; 0 uses the idle timeout."},
};

static int constraint_get(const void *config, uint8_t parameter_id, int32_t *value) {
    const struct zpt_axis_constraint_config *constraint = config;
    switch (parameter_id) {
    case CONSTRAINT_DISCARD_UNCLASSIFIED:
        *value = constraint->discard_unclassified ? 1 : 0;
        return 0;
    case CONSTRAINT_IDLE_TIMEOUT:
        *value = constraint->idle_timeout_ms;
        return 0;
    case CONSTRAINT_FOLD_INTERVAL:
        *value = constraint->fold_interval_ms;
        return 0;
    default:
        return -ENOENT;
    }
}

static int constraint_put(void *config, uint8_t parameter_id, int32_t value) {
    struct zpt_axis_constraint_config *constraint = config;
    switch (parameter_id) {
    case CONSTRAINT_DISCARD_UNCLASSIFIED:
        constraint->discard_unclassified = value != 0;
        return 0;
    case CONSTRAINT_IDLE_TIMEOUT:
        constraint->idle_timeout_ms = (uint16_t)value;
        return 0;
    case CONSTRAINT_FOLD_INTERVAL:
        constraint->fold_interval_ms = (uint16_t)value;
        return 0;
    default:
        return -ENOENT;
    }
}

const struct zpt_stage_tuning_ops zpt_axis_constraint_tuning_ops = {
    .parameters = constraint_parameters,
    .parameter_count = ZPT_ARRAY_SIZE(constraint_parameters),
    .config_size = sizeof(struct zpt_axis_constraint_config),
    .get = constraint_get,
    .put = constraint_put,
};

/* --- Cursor acceleration ------------------------------------------------- */

/* Control point speeds take the first ids and their sensitivities the next,
 * in point order, so each pair of devicetree arrays maps onto a contiguous
 * id range. */
enum {
    ACCELERATION_SPEED_FIRST = 0,
    ACCELERATION_PERCENT_FIRST = ACCELERATION_SPEED_FIRST + ZPT_CURSOR_ACCELERATION_POINTS,
    ACCELERATION_SPEED_RISE = ACCELERATION_PERCENT_FIRST + ZPT_CURSOR_ACCELERATION_POINTS,
    ACCELERATION_SPEED_FALL,
};

#define ACCELERATION_SPEED(point)                                                                  \
    {                                                                                              \
        ACCELERATION_SPEED_FIRST + (point) - 1, ZPT_TUNING_VALUE_INTEGER, "curve-speed-" #point,   \
            "curve-speeds-mm-per-s", 0, 10000, 1, "Point " #point " speed", "mm/s",                \
            "Ball speed of curve point " #point ". Point speeds must increase from point 1 to 8."  \
    }

#define ACCELERATION_PERCENT(point)                                                                \
    {                                                                                              \
        ACCELERATION_PERCENT_FIRST + (point) - 1, ZPT_TUNING_VALUE_INTEGER,                        \
            "curve-sensitivity-" #point, "curve-sensitivity-percent", 1,                           \
            ZPT_CURSOR_ACCELERATION_MAX_PERCENT, 1, "Point " #point " sensitivity", "%",           \
            "Cursor sensitivity at curve point " #point                                            \
            ", on top of the base gain. The curve runs "                                           \
            "smoothly between points and stays flat beyond the first and last."                    \
    }

static const struct zpt_tuning_parameter acceleration_parameters[] = {
    ACCELERATION_SPEED(1),
    ACCELERATION_SPEED(2),
    ACCELERATION_SPEED(3),
    ACCELERATION_SPEED(4),
    ACCELERATION_SPEED(5),
    ACCELERATION_SPEED(6),
    ACCELERATION_SPEED(7),
    ACCELERATION_SPEED(8),
    ACCELERATION_PERCENT(1),
    ACCELERATION_PERCENT(2),
    ACCELERATION_PERCENT(3),
    ACCELERATION_PERCENT(4),
    ACCELERATION_PERCENT(5),
    ACCELERATION_PERCENT(6),
    ACCELERATION_PERCENT(7),
    ACCELERATION_PERCENT(8),
    {ACCELERATION_SPEED_RISE, ZPT_TUNING_VALUE_INTEGER, "speed-rise", "speed-rise-ms", 1, 200, 1,
     "Speed rise time", "ms",
     "How quickly the speed estimate follows the ball speeding up. Shorter makes flicks "
     "accelerate sooner; longer smooths out jitter."},
    {ACCELERATION_SPEED_FALL, ZPT_TUNING_VALUE_INTEGER, "speed-fall", "speed-fall-ms", 1, 200, 1,
     "Speed fall time", "ms",
     "How quickly the speed estimate follows the ball slowing down. Shorter stops the cursor "
     "overshooting when you stop to aim."},
};

_Static_assert(ZPT_ARRAY_SIZE(acceleration_parameters) == ACCELERATION_SPEED_FALL + 1,
               "every curve point needs a speed and a sensitivity parameter");

/* Every acceleration parameter is a uint16 field. */
static const uint16_t *acceleration_field(const struct zpt_cursor_acceleration_config *config,
                                          uint8_t parameter_id) {
    if (parameter_id < ACCELERATION_PERCENT_FIRST) {
        return &config->curve_speed_mm_per_s[parameter_id - ACCELERATION_SPEED_FIRST];
    }
    if (parameter_id < ACCELERATION_SPEED_RISE) {
        return &config->curve_percent[parameter_id - ACCELERATION_PERCENT_FIRST];
    }
    if (parameter_id == ACCELERATION_SPEED_RISE) {
        return &config->speed_rise_ms;
    }
    return parameter_id == ACCELERATION_SPEED_FALL ? &config->speed_fall_ms : NULL;
}

static int acceleration_get(const void *config, uint8_t parameter_id, int32_t *value) {
    const uint16_t *field = acceleration_field(config, parameter_id);
    if (field == NULL) {
        return -ENOENT;
    }
    *value = *field;
    return 0;
}

static int acceleration_put(void *config, uint8_t parameter_id, int32_t value) {
    /* The field belongs to the writable config passed in. */
    uint16_t *field = (uint16_t *)acceleration_field(config, parameter_id);
    if (field == NULL) {
        return -ENOENT;
    }
    *field = (uint16_t)value;
    return 0;
}

static int acceleration_validate(const void *config, uint8_t *failed_parameter_id) {
    const struct zpt_cursor_acceleration_config *acceleration = config;
    /* Ranges are checked already; the curve also needs increasing speeds
     * to interpolate between. Blame the point that fails to move on. */
    for (uint8_t point = 1; point < ZPT_CURSOR_ACCELERATION_POINTS; point++) {
        if (acceleration->curve_speed_mm_per_s[point] <=
            acceleration->curve_speed_mm_per_s[point - 1U]) {
            *failed_parameter_id = ACCELERATION_SPEED_FIRST + point;
            return -EINVAL;
        }
    }
    return 0;
}

const struct zpt_stage_tuning_ops zpt_cursor_acceleration_tuning_ops = {
    .parameters = acceleration_parameters,
    .parameter_count = ZPT_ARRAY_SIZE(acceleration_parameters),
    .config_size = sizeof(struct zpt_cursor_acceleration_config),
    .get = acceleration_get,
    .put = acceleration_put,
    .validate = acceleration_validate,
};

/* --- Cursor coalescer ---------------------------------------------------- */

enum {
    COALESCER_USB_REPORT_INTERVAL = 0,
    COALESCER_BLE_REPORT_INTERVAL,
};

static const struct zpt_tuning_parameter coalescer_parameters[] = {
    {COALESCER_USB_REPORT_INTERVAL, ZPT_TUNING_VALUE_INTEGER, "usb-report-interval",
     "usb-report-interval-ms", 0, 1000, 1, "USB report interval", "ms",
     "Shortest time between cursor reports over USB; motion in between is summed. 0 sends every "
     "frame."},
    {COALESCER_BLE_REPORT_INTERVAL, ZPT_TUNING_VALUE_INTEGER, "ble-report-interval",
     "ble-report-interval-ms", 0, 1000, 1, "Bluetooth report interval", "ms",
     "Shortest time between cursor reports over Bluetooth. Match the connection interval, "
     "usually 7.5 to 15 ms, so reports don't queue up."},
};

static const uint16_t *coalescer_field(const struct zpt_cursor_coalescer_config *config,
                                       uint8_t parameter_id) {
    if (parameter_id == COALESCER_USB_REPORT_INTERVAL) {
        return &config->usb_report_interval_ms;
    }
    return parameter_id == COALESCER_BLE_REPORT_INTERVAL ? &config->ble_report_interval_ms : NULL;
}

static int coalescer_get(const void *config, uint8_t parameter_id, int32_t *value) {
    const uint16_t *field = coalescer_field(config, parameter_id);
    if (field == NULL) {
        return -ENOENT;
    }
    *value = *field;
    return 0;
}

static int coalescer_put(void *config, uint8_t parameter_id, int32_t value) {
    /* The field belongs to the writable config passed in. */
    uint16_t *field = (uint16_t *)coalescer_field(config, parameter_id);
    if (field == NULL) {
        return -ENOENT;
    }
    *field = (uint16_t)value;
    return 0;
}

const struct zpt_stage_tuning_ops zpt_cursor_coalescer_tuning_ops = {
    .parameters = coalescer_parameters,
    .parameter_count = ZPT_ARRAY_SIZE(coalescer_parameters),
    .config_size = sizeof(struct zpt_cursor_coalescer_config),
    .get = coalescer_get,
    .put = coalescer_put,
};
