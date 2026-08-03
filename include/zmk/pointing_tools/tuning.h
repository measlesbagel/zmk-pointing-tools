/* SPDX-License-Identifier: MIT */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum zpt_tuning_target_kind {
    ZPT_TUNING_TARGET_SCROLL = 1,
    ZPT_TUNING_TARGET_TEXT_NAV = 2,
};

enum zpt_tuning_value_type {
    ZPT_TUNING_VALUE_INTEGER = 0,
    ZPT_TUNING_VALUE_BOOLEAN = 1,
};

struct zpt_tuning_parameter {
    uint8_t id;
    uint8_t type;
    int32_t minimum;
    int32_t maximum;
    int32_t step;
    const char *label;
    const char *unit;
    const char *description;
};

struct zpt_tuning_target {
    uint8_t kind;
    const char *label;
    const struct zpt_tuning_parameter *parameters;
    size_t parameter_count;
    void *context;
    int (*get)(void *context, uint8_t parameter_id, bool compiled, int32_t *value);
    int (*set)(void *context, uint8_t parameter_id, int32_t value);
    int (*reset)(void *context);
};

int zpt_tuning_register(const struct zpt_tuning_target *target);
size_t zpt_tuning_target_count(void);
const struct zpt_tuning_target *zpt_tuning_target_get(uint8_t target_id);
const struct zpt_tuning_parameter *zpt_tuning_parameter_get(const struct zpt_tuning_target *target,
                                                            uint8_t parameter_id);

int zpt_tuning_get(uint8_t target_id, uint8_t parameter_id, bool compiled, int32_t *value);
int zpt_tuning_set(uint8_t target_id, uint8_t parameter_id, int32_t value);
int zpt_tuning_reset(uint8_t target_id);
int zpt_tuning_reset_all(void);
