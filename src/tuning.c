/* SPDX-License-Identifier: MIT */

#include <errno.h>

#include <zephyr/sys/util.h>

#include <zmk/pointing_tools/tuning.h>

static const struct zpt_tuning_target
    *zpt_tuning_targets[CONFIG_ZMK_POINTING_TOOLS_TUNING_MAX_TARGETS];
static size_t zpt_tuning_targets_count;

int zpt_tuning_register(const struct zpt_tuning_target *target) {
    if (target == NULL || target->label == NULL || target->parameters == NULL ||
        target->parameter_count == 0 || target->get == NULL || target->set == NULL ||
        target->reset == NULL) {
        return -EINVAL;
    }

    for (size_t i = 0; i < zpt_tuning_targets_count; i++) {
        if (zpt_tuning_targets[i] == target) {
            return (int)i;
        }
    }

    if (zpt_tuning_targets_count >= ARRAY_SIZE(zpt_tuning_targets)) {
        return -ENOMEM;
    }

    zpt_tuning_targets[zpt_tuning_targets_count] = target;
    return (int)zpt_tuning_targets_count++;
}

size_t zpt_tuning_target_count(void) { return zpt_tuning_targets_count; }

const struct zpt_tuning_target *zpt_tuning_target_get(uint8_t target_id) {
    if (target_id >= zpt_tuning_targets_count) {
        return NULL;
    }
    return zpt_tuning_targets[target_id];
}

const struct zpt_tuning_parameter *zpt_tuning_parameter_get(const struct zpt_tuning_target *target,
                                                            uint8_t parameter_id) {
    if (target == NULL) {
        return NULL;
    }

    for (size_t i = 0; i < target->parameter_count; i++) {
        if (target->parameters[i].id == parameter_id) {
            return &target->parameters[i];
        }
    }
    return NULL;
}

int zpt_tuning_get(uint8_t target_id, uint8_t parameter_id, bool compiled, int32_t *value) {
    const struct zpt_tuning_target *target = zpt_tuning_target_get(target_id);
    if (target == NULL) {
        return -ENODEV;
    }
    if (zpt_tuning_parameter_get(target, parameter_id) == NULL) {
        return -ENOENT;
    }
    return target->get(target->context, parameter_id, compiled, value);
}

int zpt_tuning_set(uint8_t target_id, uint8_t parameter_id, int32_t value) {
    const struct zpt_tuning_target *target = zpt_tuning_target_get(target_id);
    if (target == NULL) {
        return -ENODEV;
    }

    const struct zpt_tuning_parameter *parameter = zpt_tuning_parameter_get(target, parameter_id);
    if (parameter == NULL) {
        return -ENOENT;
    }
    if (value < parameter->minimum || value > parameter->maximum ||
        (parameter->step > 1 && (value - parameter->minimum) % parameter->step != 0)) {
        return -ERANGE;
    }
    if (parameter->type == ZPT_TUNING_VALUE_BOOLEAN && value != 0 && value != 1) {
        return -ERANGE;
    }
    return target->set(target->context, parameter_id, value);
}

int zpt_tuning_reset(uint8_t target_id) {
    const struct zpt_tuning_target *target = zpt_tuning_target_get(target_id);
    return target == NULL ? -ENODEV : target->reset(target->context);
}

int zpt_tuning_reset_all(void) {
    int result = 0;
    for (size_t i = 0; i < zpt_tuning_targets_count; i++) {
        int ret = zpt_tuning_targets[i]->reset(zpt_tuning_targets[i]->context);
        if (ret < 0 && result == 0) {
            result = ret;
        }
    }
    return result;
}
