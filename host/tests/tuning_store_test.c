/* SPDX-License-Identifier: MIT */

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <zmk/pointing_tools/service/tuning.h>
#include <zmk/pointing_tools/service/tuning_store.h>

/* A two-target stand-in for the tuning registry. Target "gain" has two
 * integer parameters and rejects a batch that makes low exceed high, like
 * a stage with a cross-parameter check. */

static const struct zpt_tuning_parameter gain_parameters[] = {
    {0, ZPT_TUNING_VALUE_INTEGER, "low", "low", 0, 100, 1, "Low", "", "Low."},
    {1, ZPT_TUNING_VALUE_INTEGER, "high", "high", 0, 100, 1, "High", "", "High."},
};
static const struct zpt_tuning_parameter flag_parameters[] = {
    {0, ZPT_TUNING_VALUE_BOOLEAN, "enabled", "enabled", 0, 1, 1, "Enabled", "", "On."},
};

struct fake_target {
    struct zpt_tuning_target target;
    int32_t compiled[2];
    int32_t current[2];
};

static struct fake_target fakes[2] = {
    {.target = {.stable_id = "gain", .parameters = gain_parameters, .parameter_count = 2},
     .compiled = {10, 50}},
    {.target = {.stable_id = "flag", .parameters = flag_parameters, .parameter_count = 1},
     .compiled = {1}},
};

size_t zpt_tuning_target_count(void) { return 2; }

const struct zpt_tuning_target *zpt_tuning_target_get(uint8_t target_id) {
    return target_id < 2 ? &fakes[target_id].target : NULL;
}

const struct zpt_tuning_parameter *zpt_tuning_parameter_get(const struct zpt_tuning_target *target,
                                                            uint8_t parameter_id) {
    return target != NULL && parameter_id < target->parameter_count
               ? &target->parameters[parameter_id]
               : NULL;
}

int zpt_tuning_get(uint8_t target_id, uint8_t parameter_id, bool compiled, int32_t *value) {
    if (target_id >= 2) {
        return -ENODEV;
    }
    if (parameter_id >= fakes[target_id].target.parameter_count) {
        return -ENOENT;
    }
    *value =
        compiled ? fakes[target_id].compiled[parameter_id] : fakes[target_id].current[parameter_id];
    return 0;
}

int zpt_tuning_set_many(uint8_t target_id, const struct zpt_tuning_value *values,
                        size_t value_count, uint8_t *failed_parameter_id) {
    (void)failed_parameter_id;
    if (target_id >= 2) {
        return -ENODEV;
    }
    struct fake_target *fake = &fakes[target_id];
    int32_t candidate[2] = {fake->current[0], fake->current[1]};
    for (size_t i = 0; i < value_count; i++) {
        const struct zpt_tuning_parameter *parameter =
            zpt_tuning_parameter_get(&fake->target, values[i].parameter_id);
        if (parameter == NULL) {
            return -ENOENT;
        }
        if (values[i].value < parameter->minimum || values[i].value > parameter->maximum) {
            return -ERANGE;
        }
        candidate[values[i].parameter_id] = values[i].value;
    }
    if (target_id == 0 && candidate[0] > candidate[1]) {
        return -EINVAL;
    }
    memcpy(fake->current, candidate, sizeof(candidate));
    return 0;
}

int zpt_tuning_reset(uint8_t target_id) {
    if (target_id >= 2) {
        return -ENODEV;
    }
    memcpy(fakes[target_id].current, fakes[target_id].compiled, sizeof(fakes[target_id].current));
    return 0;
}

/* Backend that mirrors storage as a small name/value table. */
#define FLASH_SLOTS 8
static struct {
    char name[ZPT_TUNING_STORE_NAME_MAX + 1];
    int32_t value;
    bool used;
} flash[FLASH_SLOTS];
static int flash_writes;

static int flash_write(void *context, const char *name, int32_t value) {
    (void)context;
    flash_writes++;
    for (size_t i = 0; i < FLASH_SLOTS; i++) {
        if (flash[i].used && strcmp(flash[i].name, name) == 0) {
            flash[i].value = value;
            return 0;
        }
    }
    for (size_t i = 0; i < FLASH_SLOTS; i++) {
        if (!flash[i].used) {
            flash[i].used = true;
            (void)snprintf(flash[i].name, sizeof(flash[i].name), "%s", name);
            flash[i].value = value;
            return 0;
        }
    }
    return -ENOSPC;
}

static int flash_erase(void *context, const char *name) {
    (void)context;
    for (size_t i = 0; i < FLASH_SLOTS; i++) {
        if (flash[i].used && strcmp(flash[i].name, name) == 0) {
            flash[i].used = false;
        }
    }
    return 0;
}

static const struct zpt_tuning_store_backend backend = {
    .write = flash_write,
    .erase = flash_erase,
};

static size_t flash_count(void) {
    size_t count = 0;
    for (size_t i = 0; i < FLASH_SLOTS; i++) {
        count += flash[i].used ? 1 : 0;
    }
    return count;
}

/* Simulate a reboot: RAM goes back to compiled and the store reloads what
 * storage holds. */
static size_t reboot(void) {
    (void)zpt_tuning_reset(0);
    (void)zpt_tuning_reset(1);
    zpt_tuning_store_init(&backend);
    for (size_t i = 0; i < FLASH_SLOTS; i++) {
        if (flash[i].used) {
            (void)zpt_tuning_store_load(flash[i].name, flash[i].value);
        }
    }
    return zpt_tuning_store_apply_loaded();
}

static int32_t current(uint8_t target_id, uint8_t parameter_id) {
    return fakes[target_id].current[parameter_id];
}

static void reset_world(void) {
    memset(flash, 0, sizeof(flash));
    flash_writes = 0;
    (void)reboot();
}

/* Previewing never writes; save writes only values that differ from
 * compiled, and they come back after a reboot. */
static void test_save_survives_reboot(void) {
    reset_world();
    const struct zpt_tuning_value preview = {.parameter_id = 1, .value = 70};
    assert(zpt_tuning_set_many(0, &preview, 1, NULL) == 0);
    assert(flash_writes == 0);
    assert(zpt_tuning_store_saved(0, 1, NULL) == -ENOENT);

    assert(zpt_tuning_store_save(0) == 0);
    assert(flash_count() == 1);
    int32_t saved = 0;
    assert(zpt_tuning_store_saved(0, 1, &saved) == 0 && saved == 70);
    assert(zpt_tuning_store_saved(0, 0, NULL) == -ENOENT);

    /* Saving again with nothing changed writes nothing. */
    assert(zpt_tuning_store_save(0) == 0);
    assert(flash_writes == 1);

    assert(reboot() == 0);
    assert(current(0, 0) == 10 && current(0, 1) == 70);
    assert(zpt_tuning_store_saved(0, 1, &saved) == 0 && saved == 70);
}

/* Restore discards previews back to saved, not compiled. */
static void test_restore_returns_to_saved(void) {
    reset_world();
    const struct zpt_tuning_value tuned = {.parameter_id = 1, .value = 80};
    assert(zpt_tuning_set_many(0, &tuned, 1, NULL) == 0);
    assert(zpt_tuning_store_save(0) == 0);

    const struct zpt_tuning_value preview = {.parameter_id = 1, .value = 90};
    assert(zpt_tuning_set_many(0, &preview, 1, NULL) == 0);
    assert(zpt_tuning_store_restore(0) == 0);
    assert(current(0, 1) == 80);
}

/* Saving a value back at compiled erases it, and clear erases everything
 * for one target or all of them. */
static void test_clear_and_save_at_compiled_erase(void) {
    reset_world();
    const struct zpt_tuning_value gain[] = {{.parameter_id = 0, .value = 20},
                                            {.parameter_id = 1, .value = 60}};
    const struct zpt_tuning_value off = {.parameter_id = 0, .value = 0};
    assert(zpt_tuning_set_many(0, gain, 2, NULL) == 0);
    assert(zpt_tuning_set_many(1, &off, 1, NULL) == 0);
    assert(zpt_tuning_store_save(ZPT_TUNING_STORE_ALL_TARGETS) == 0);
    assert(flash_count() == 3 && zpt_tuning_store_count() == 3);

    const struct zpt_tuning_value back = {.parameter_id = 0, .value = 10};
    assert(zpt_tuning_set_many(0, &back, 1, NULL) == 0);
    assert(zpt_tuning_store_save(0) == 0);
    assert(flash_count() == 2);
    assert(zpt_tuning_store_saved(0, 0, NULL) == -ENOENT);

    assert(zpt_tuning_store_clear(1) == 0);
    assert(current(1, 0) == 1);
    assert(flash_count() == 1);

    assert(zpt_tuning_store_clear(ZPT_TUNING_STORE_ALL_TARGETS) == 0);
    assert(flash_count() == 0 && zpt_tuning_store_count() == 0);
    assert(current(0, 0) == 10 && current(0, 1) == 50);
}

/* After a firmware update, unknown names are dropped, and a saved set the
 * target now rejects falls back to compiled as a whole. */
static void test_stale_sets_fall_back(void) {
    reset_world();
    assert(zpt_tuning_store_load("gone/low", 5) == -ENOENT);
    assert(zpt_tuning_store_load("gain/renamed", 5) == -ENOENT);
    assert(zpt_tuning_store_load("no-separator", 5) == -EINVAL);
    assert(zpt_tuning_store_load("gain/", 5) == -EINVAL);

    /* low 90 > compiled high 50: the batch is inconsistent. */
    assert(zpt_tuning_store_load("gain/low", 90) == 0);
    assert(zpt_tuning_store_load("flag/enabled", 0) == 0);
    assert(zpt_tuning_store_apply_loaded() == 1);
    assert(current(0, 0) == 10 && current(0, 1) == 50);
    assert(zpt_tuning_store_saved(0, 0, NULL) == -ENOENT);
    /* The other target still loads. */
    assert(current(1, 0) == 0);
    assert(zpt_tuning_store_saved(1, 0, NULL) == 0);

    /* Out of range too. */
    reset_world();
    assert(zpt_tuning_store_load("gain/high", 500) == 0);
    assert(zpt_tuning_store_apply_loaded() == 1);
    assert(current(0, 1) == 50);
}

static void test_names_are_bounded(void) {
    char name[ZPT_TUNING_STORE_NAME_MAX + 1];
    assert(zpt_tuning_store_name(name, sizeof(name), "gain", "low") == 8);
    assert(strcmp(name, "gain/low") == 0);
    char long_id[ZPT_TUNING_STORE_NAME_MAX + 1];
    memset(long_id, 'a', sizeof(long_id) - 1);
    long_id[sizeof(long_id) - 1] = '\0';
    assert(zpt_tuning_store_name(name, sizeof(name), long_id, "x") == -ENAMETOOLONG);
    assert(zpt_tuning_store_save(5) == -ENODEV);
}

int main(void) {
    test_save_survives_reboot();
    test_restore_returns_to_saved();
    test_clear_and_save_at_compiled_erase();
    test_stale_sets_fall_back();
    test_names_are_bounded();
    puts("tuning store tests passed");
    return 0;
}
