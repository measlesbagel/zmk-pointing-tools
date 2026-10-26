/* SPDX-License-Identifier: MIT */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include <zmk/pointing_tools/service/tuning.h>

/* Runtime tuning descriptions for pipeline stages, independent of ZMK.
 *
 * Each tunable stage type publishes its parameters in devicetree units
 * (micrometres, steps per metre, milliseconds, percent) and converts them to
 * and from its fixed-point config. Parameter keys and devicetree properties
 * match the stage binding, so an exported profile maps back onto devicetree
 * one to one. The platform layer registers one tuning target per stage
 * instance and applies prepared configs while no pipeline is running. */

struct zpt_stage_tuning_ops {
    const struct zpt_tuning_parameter *parameters;
    size_t parameter_count;
    /* Size of the stage's config struct, which tuning copies whole. */
    size_t config_size;
    int (*get)(const void *config, uint8_t parameter_id, int32_t *value);
    /* Store an already range-checked value into a config copy. */
    int (*put)(void *config, uint8_t parameter_id, int32_t value);
    /* Optional checks across parameters of a complete candidate config.
     * Reports the parameter to blame through *failed_parameter_id. */
    int (*validate)(const void *config, uint8_t *failed_parameter_id);
};

/* Largest stage config tuning can stage on the stack. */
#define ZPT_STAGE_TUNING_MAX_CONFIG_SIZE 96

extern const struct zpt_stage_tuning_ops zpt_coherent_displacement_tuning_ops;
extern const struct zpt_stage_tuning_ops zpt_cursor_transfer_tuning_ops;
extern const struct zpt_stage_tuning_ops zpt_scroll_batcher_tuning_ops;
extern const struct zpt_stage_tuning_ops zpt_text_nav_tuning_ops;
extern const struct zpt_stage_tuning_ops zpt_axis_intent_tuning_ops;
extern const struct zpt_stage_tuning_ops zpt_axis_constraint_tuning_ops;
extern const struct zpt_stage_tuning_ops zpt_cursor_acceleration_tuning_ops;
extern const struct zpt_stage_tuning_ops zpt_cursor_coalescer_tuning_ops;

/* Build the config that results from applying values to current, without
 * touching current. candidate must hold ops->config_size bytes. Values are
 * range-checked by the tuning service before they get here. Returns 0, or a
 * negative errno with the offending parameter in *failed_parameter_id. */
int zpt_stage_tuning_prepare(const struct zpt_stage_tuning_ops *ops, const void *current,
                             const struct zpt_tuning_value *values, size_t value_count,
                             void *candidate, uint8_t *failed_parameter_id);

/* Build the config that restores every tunable parameter to its compiled
 * value. Fields tuning doesn't own, such as resolved policy pointers, keep
 * their current values. */
int zpt_stage_tuning_prepare_reset(const struct zpt_stage_tuning_ops *ops, const void *compiled,
                                   const void *current, void *candidate);

/* Binds one stage instance's config to a tuning target. compiled is the
 * devicetree config and current the writable copy the stage reads. Only the
 * tuning service writes current, through apply, which the platform supplies
 * to copy a prepared candidate over current while no pipeline runs. */
struct zpt_stage_tuning_binding {
    const struct zpt_stage_tuning_ops *ops;
    const void *compiled;
    void *current;
    int (*apply)(void *current, const void *candidate, size_t size);
};

/* Fill target so the tuning service drives binding: get reads compiled or
 * current values, set_many and reset prepare a candidate and apply it whole.
 * The stage's stable id doubles as the target label. target and binding
 * must outlive the registration. Returns -EINVAL for a malformed binding or
 * a config larger than ZPT_STAGE_TUNING_MAX_CONFIG_SIZE. */
int zpt_stage_tuning_target_init(struct zpt_tuning_target *target,
                                 struct zpt_stage_tuning_binding *binding, const char *stable_id,
                                 const char *devicetree_path);

/* Unit conversions between devicetree integers and stage fixed point,
 * rounded to nearest so compiled values read back exactly. */
int32_t zpt_stage_tuning_fixed_millimeters_to_micrometers(int64_t value);
int32_t zpt_stage_tuning_fixed_per_millimeter_to_per_meter(int64_t value);
