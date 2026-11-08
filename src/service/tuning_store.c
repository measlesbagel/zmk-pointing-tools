/* SPDX-License-Identifier: MIT */

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <zmk/pointing_tools/service/tuning.h>
#include <zmk/pointing_tools/service/tuning_store.h>

#ifdef CONFIG_ZMK_POINTING_TOOLS_SAVED_VALUES_MAX
#define ZPT_TUNING_STORE_CAPACITY CONFIG_ZMK_POINTING_TOOLS_SAVED_VALUES_MAX
#else
#define ZPT_TUNING_STORE_CAPACITY 128
#endif

/* The tuning registry caps a target at UINT8_MAX parameters, but no stage
 * has more than an acceleration curve's 18. */
#define ZPT_TUNING_STORE_TARGET_VALUES 32

struct zpt_saved_value {
    uint8_t target_id;
    uint8_t parameter_id;
    int32_t value;
};

static struct zpt_saved_value zpt_saved[ZPT_TUNING_STORE_CAPACITY];
static size_t zpt_saved_count;
static const struct zpt_tuning_store_backend *zpt_store_backend;

void zpt_tuning_store_init(const struct zpt_tuning_store_backend *backend) {
    zpt_store_backend = backend;
    zpt_saved_count = 0;
}

size_t zpt_tuning_store_count(void) { return zpt_saved_count; }

int zpt_tuning_store_name(char *buffer, size_t size, const char *stable_id, const char *key) {
    if (buffer == NULL || stable_id == NULL || key == NULL) {
        return -EINVAL;
    }
    const size_t length = strlen(stable_id) + 1 + strlen(key);
    if (length > ZPT_TUNING_STORE_NAME_MAX || length >= size) {
        return -ENAMETOOLONG;
    }
    (void)snprintf(buffer, size, "%s/%s", stable_id, key);
    return (int)length;
}

static struct zpt_saved_value *find_saved(uint8_t target_id, uint8_t parameter_id) {
    for (size_t i = 0; i < zpt_saved_count; i++) {
        if (zpt_saved[i].target_id == target_id && zpt_saved[i].parameter_id == parameter_id) {
            return &zpt_saved[i];
        }
    }
    return NULL;
}

static int put_saved(uint8_t target_id, uint8_t parameter_id, int32_t value) {
    struct zpt_saved_value *entry = find_saved(target_id, parameter_id);
    if (entry == NULL) {
        if (zpt_saved_count >= ZPT_TUNING_STORE_CAPACITY) {
            return -ENOMEM;
        }
        entry = &zpt_saved[zpt_saved_count++];
        entry->target_id = target_id;
        entry->parameter_id = parameter_id;
    }
    entry->value = value;
    return 0;
}

static void remove_saved(struct zpt_saved_value *entry) { *entry = zpt_saved[--zpt_saved_count]; }

static void drop_target(uint8_t target_id) {
    for (size_t i = zpt_saved_count; i-- > 0;) {
        if (zpt_saved[i].target_id == target_id) {
            remove_saved(&zpt_saved[i]);
        }
    }
}

static int find_target(const char *stable_id, size_t stable_id_length) {
    const size_t count = zpt_tuning_target_count();
    for (size_t i = 0; i < count && i < ZPT_TUNING_STORE_ALL_TARGETS; i++) {
        const struct zpt_tuning_target *target = zpt_tuning_target_get((uint8_t)i);
        if (strlen(target->stable_id) == stable_id_length &&
            strncmp(target->stable_id, stable_id, stable_id_length) == 0) {
            return (int)i;
        }
    }
    return -ENOENT;
}

static int find_parameter(const struct zpt_tuning_target *target, const char *key) {
    for (size_t i = 0; i < target->parameter_count; i++) {
        if (strcmp(target->parameters[i].key, key) == 0) {
            return target->parameters[i].id;
        }
    }
    return -ENOENT;
}

int zpt_tuning_store_load(const char *name, int32_t value) {
    /* Parameter keys never contain '/', so the last one splits the name. */
    const char *separator = name == NULL ? NULL : strrchr(name, '/');
    if (separator == NULL || separator == name || separator[1] == '\0') {
        return -EINVAL;
    }
    const int target_id = find_target(name, (size_t)(separator - name));
    if (target_id < 0) {
        return target_id;
    }
    const int parameter_id =
        find_parameter(zpt_tuning_target_get((uint8_t)target_id), separator + 1);
    if (parameter_id < 0) {
        return parameter_id;
    }
    return put_saved((uint8_t)target_id, (uint8_t)parameter_id, value);
}

/* Apply a target's saved values as one batch on top of its current ones. */
static int apply_saved(uint8_t target_id) {
    struct zpt_tuning_value values[ZPT_TUNING_STORE_TARGET_VALUES];
    size_t count = 0;
    for (size_t i = 0; i < zpt_saved_count; i++) {
        if (zpt_saved[i].target_id != target_id) {
            continue;
        }
        if (count >= ZPT_TUNING_STORE_TARGET_VALUES) {
            return -ENOMEM;
        }
        values[count++] = (struct zpt_tuning_value){
            .parameter_id = zpt_saved[i].parameter_id,
            .value = zpt_saved[i].value,
        };
    }
    return count == 0 ? 0 : zpt_tuning_set_many(target_id, values, count, NULL);
}

size_t zpt_tuning_store_apply_loaded(void) {
    size_t fallbacks = 0;
    const size_t count = zpt_tuning_target_count();
    for (size_t i = 0; i < count && i < ZPT_TUNING_STORE_ALL_TARGETS; i++) {
        if (apply_saved((uint8_t)i) < 0) {
            /* Out of range or inconsistent after a firmware update: run on
             * compiled values and stop showing the set as saved. */
            (void)zpt_tuning_reset((uint8_t)i);
            drop_target((uint8_t)i);
            fallbacks++;
        }
    }
    return fallbacks;
}

int zpt_tuning_store_saved(uint8_t target_id, uint8_t parameter_id, int32_t *value) {
    const struct zpt_saved_value *entry = find_saved(target_id, parameter_id);
    if (entry == NULL) {
        return -ENOENT;
    }
    if (value != NULL) {
        *value = entry->value;
    }
    return 0;
}

static int backend_write(const char *name, int32_t value) {
    if (zpt_store_backend == NULL || zpt_store_backend->write == NULL) {
        return 0;
    }
    return zpt_store_backend->write(zpt_store_backend->context, name, value);
}

static int backend_erase(const char *name) {
    if (zpt_store_backend == NULL || zpt_store_backend->erase == NULL) {
        return 0;
    }
    return zpt_store_backend->erase(zpt_store_backend->context, name);
}

static int save_parameter(uint8_t target_id, const struct zpt_tuning_target *target,
                          const struct zpt_tuning_parameter *parameter) {
    int32_t compiled = 0;
    int32_t current = 0;
    int ret = zpt_tuning_get(target_id, parameter->id, true, &compiled);
    if (ret == 0) {
        ret = zpt_tuning_get(target_id, parameter->id, false, &current);
    }
    if (ret < 0) {
        return ret;
    }

    char name[ZPT_TUNING_STORE_NAME_MAX + 1];
    ret = zpt_tuning_store_name(name, sizeof(name), target->stable_id, parameter->key);
    if (ret < 0) {
        return ret;
    }

    struct zpt_saved_value *entry = find_saved(target_id, parameter->id);
    if (current == compiled) {
        /* Matching compiled needs no saved value. */
        if (entry == NULL) {
            return 0;
        }
        ret = backend_erase(name);
        if (ret == 0) {
            remove_saved(entry);
        }
        return ret;
    }
    if (entry != NULL && entry->value == current) {
        return 0;
    }
    if (entry == NULL && zpt_saved_count >= ZPT_TUNING_STORE_CAPACITY) {
        return -ENOMEM;
    }
    ret = backend_write(name, current);
    return ret < 0 ? ret : put_saved(target_id, parameter->id, current);
}

static int save_target(uint8_t target_id) {
    const struct zpt_tuning_target *target = zpt_tuning_target_get(target_id);
    if (target == NULL) {
        return -ENODEV;
    }
    for (size_t i = 0; i < target->parameter_count; i++) {
        const int ret = save_parameter(target_id, target, &target->parameters[i]);
        if (ret < 0) {
            return ret;
        }
    }
    return 0;
}

static int clear_target(uint8_t target_id) {
    const struct zpt_tuning_target *target = zpt_tuning_target_get(target_id);
    if (target == NULL) {
        return -ENODEV;
    }
    for (size_t i = zpt_saved_count; i-- > 0;) {
        if (zpt_saved[i].target_id != target_id) {
            continue;
        }
        const struct zpt_tuning_parameter *parameter =
            zpt_tuning_parameter_get(target, zpt_saved[i].parameter_id);
        char name[ZPT_TUNING_STORE_NAME_MAX + 1];
        int ret = parameter == NULL ? -ENOENT
                                    : zpt_tuning_store_name(name, sizeof(name), target->stable_id,
                                                            parameter->key);
        if (ret >= 0) {
            ret = backend_erase(name);
        }
        if (ret < 0) {
            return ret;
        }
        remove_saved(&zpt_saved[i]);
    }
    return zpt_tuning_reset(target_id);
}

static int restore_target(uint8_t target_id) {
    int ret = zpt_tuning_reset(target_id);
    return ret < 0 ? ret : apply_saved(target_id);
}

static int for_targets(uint8_t target_id, int (*action)(uint8_t target_id)) {
    if (target_id != ZPT_TUNING_STORE_ALL_TARGETS) {
        return action(target_id);
    }
    int result = 0;
    const size_t count = zpt_tuning_target_count();
    for (size_t i = 0; i < count && i < ZPT_TUNING_STORE_ALL_TARGETS; i++) {
        const int ret = action((uint8_t)i);
        if (ret < 0 && result == 0) {
            result = ret;
        }
    }
    return result;
}

int zpt_tuning_store_save(uint8_t target_id) { return for_targets(target_id, save_target); }

int zpt_tuning_store_clear(uint8_t target_id) { return for_targets(target_id, clear_target); }

int zpt_tuning_store_restore(uint8_t target_id) { return for_targets(target_id, restore_target); }
