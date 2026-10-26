/* SPDX-License-Identifier: MIT */
#pragma once

#include <zmk/pointing_tools/service/tuning.h>
#include <zmk/pointing_tools/stage/stage_tuning.h>

/* Runtime tuning registration for a devicetree stage provider. Lives in the
 * provider's device data next to the writable config copy its stage reads. */
struct zpt_zmk_stage_tuning {
    struct zpt_tuning_target target;
    struct zpt_stage_tuning_binding binding;
};

/* Register current as a tuning target. Changes are applied while every
 * router is held between frames. A stage that can't register (for example
 * because the target table is full) logs a warning and keeps running on its
 * compiled config, so this never fails provider init. */
void zpt_zmk_stage_tuning_register(struct zpt_zmk_stage_tuning *tuning,
                                   const struct zpt_stage_tuning_ops *ops, const char *stable_id,
                                   const char *devicetree_path, const void *compiled,
                                   void *current);
