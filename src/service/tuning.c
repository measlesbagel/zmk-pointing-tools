/* SPDX-License-Identifier: MIT */

#include <errno.h>
#include <string.h>

#include <zephyr/sys/util.h>

#include <zmk/pointing_tools/service/tuning.h>

static const struct zpt_tuning_target
    *zpt_tuning_targets[CONFIG_ZMK_POINTING_TOOLS_TUNING_MAX_TARGETS];
static size_t zpt_tuning_targets_count;

static int validate_target_parameters(const struct zpt_tuning_target *target) {
    for (size_t i = 0; i < target->parameter_count; i++) {
        if (target->parameters[i].key == NULL ||
            target->parameters[i].devicetree_property == NULL ||
            target->parameters[i].label == NULL || target->parameters[i].unit == NULL ||
            target->parameters[i].description == NULL) {
            return -EINVAL;
        }
        for (size_t j = 0; j < i; j++) {
            if (target->parameters[j].id == target->parameters[i].id ||
                strcmp(target->parameters[j].key, target->parameters[i].key) == 0) {
                return -EEXIST;
            }
        }
    }
    return 0;
}

static int validate_target(const struct zpt_tuning_target *target) {
    if (target == NULL || target->stable_id == NULL || target->label == NULL ||
        target->devicetree_path == NULL || target->parameters == NULL ||
        target->parameter_count == 0 || target->get == NULL || target->set_many == NULL ||
        target->reset == NULL) {
        return -EINVAL;
    }
    return validate_target_parameters(target);
}

static int find_registered_target(const struct zpt_tuning_target *target) {
    for (size_t i = 0; i < zpt_tuning_targets_count; i++) {
        if (zpt_tuning_targets[i] == target) {
            return (int)i;
        }
        if (strcmp(zpt_tuning_targets[i]->stable_id, target->stable_id) == 0) {
            return -EEXIST;
        }
    }
    return -ENOENT;
}

int zpt_tuning_register(const struct zpt_tuning_target *target) {
    int ret = validate_target(target);
    if (ret < 0) {
        return ret;
    }

    ret = find_registered_target(target);
    if (ret != -ENOENT) {
        return ret;
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
    const struct zpt_tuning_value update = {
        .parameter_id = parameter_id,
        .value = value,
    };
    return zpt_tuning_set_many(target_id, &update, 1, NULL);
}

static int validate_value_update(const struct zpt_tuning_target *target,
                                 const struct zpt_tuning_value *values, size_t index) {
    const struct zpt_tuning_value *value = &values[index];
    const struct zpt_tuning_parameter *parameter =
        zpt_tuning_parameter_get(target, value->parameter_id);
    if (parameter == NULL) {
        return -ENOENT;
    }
    if (value->value < parameter->minimum || value->value > parameter->maximum ||
        (parameter->step > 1 && (value->value - parameter->minimum) % parameter->step != 0)) {
        return -ERANGE;
    }
    if (parameter->type == ZPT_TUNING_VALUE_BOOLEAN && value->value != 0 && value->value != 1) {
        return -ERANGE;
    }
    for (size_t j = 0; j < index; j++) {
        if (values[j].parameter_id == value->parameter_id) {
            return -EEXIST;
        }
    }
    return 0;
}

int zpt_tuning_set_many(uint8_t target_id, const struct zpt_tuning_value *values,
                        size_t value_count, uint8_t *failed_parameter_id) {
    const struct zpt_tuning_target *target = zpt_tuning_target_get(target_id);
    if (target == NULL) {
        return -ENODEV;
    }
    if (values == NULL || value_count == 0) {
        return -EINVAL;
    }

    for (size_t i = 0; i < value_count; i++) {
        if (failed_parameter_id != NULL) {
            *failed_parameter_id = values[i].parameter_id;
        }
        int ret = validate_value_update(target, values, i);
        if (ret < 0) {
            return ret;
        }
    }

    return target->set_many(target->context, values, value_count, failed_parameter_id);
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
