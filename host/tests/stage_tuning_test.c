/* SPDX-License-Identifier: MIT */

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <zmk/pointing_tools/core/signal.h>
#include <zmk/pointing_tools/stage/axis_constraint.h>
#include <zmk/pointing_tools/stage/axis_intent.h>
#include <zmk/pointing_tools/stage/cursor_transfer.h>
#include <zmk/pointing_tools/stage/motion_gate/coherent_displacement.h>
#include <zmk/pointing_tools/stage/scroll_batcher.h>
#include <zmk/pointing_tools/stage/stage_tuning.h>
#include <zmk/pointing_tools/stage/text_nav.h>

static const struct zpt_stage_tuning_ops *const all_ops[] = {
    &zpt_coherent_displacement_tuning_ops, &zpt_cursor_transfer_tuning_ops,
    &zpt_scroll_batcher_tuning_ops,        &zpt_text_nav_tuning_ops,
    &zpt_axis_intent_tuning_ops,           &zpt_axis_constraint_tuning_ops,
};

static int32_t get(const struct zpt_stage_tuning_ops *ops, const void *config, uint8_t id) {
    int32_t value = INT32_MIN;
    assert(ops->get(config, id, &value) == 0);
    return value;
}

/* Every parameter is well formed, fits the staging buffer, and reads back
 * exactly what it stores across its whole range. */
static void test_parameter_tables_round_trip(void) {
    for (size_t i = 0; i < sizeof(all_ops) / sizeof(all_ops[0]); i++) {
        const struct zpt_stage_tuning_ops *ops = all_ops[i];
        assert(ops->config_size <= ZPT_STAGE_TUNING_MAX_CONFIG_SIZE);
        assert(ops->parameter_count > 0);
        for (size_t p = 0; p < ops->parameter_count; p++) {
            const struct zpt_tuning_parameter *parameter = &ops->parameters[p];
            assert(parameter->key && parameter->devicetree_property && parameter->label &&
                   parameter->unit && parameter->description);
            assert(parameter->minimum <= parameter->maximum && parameter->step >= 1);

            unsigned char config[ZPT_STAGE_TUNING_MAX_CONFIG_SIZE] = {0};
            const int32_t samples[] = {parameter->minimum, parameter->maximum,
                                       parameter->minimum +
                                           (parameter->maximum - parameter->minimum) / 3};
            for (size_t s = 0; s < 3; s++) {
                assert(ops->put(config, parameter->id, samples[s]) == 0);
                assert(get(ops, config, parameter->id) == samples[s]);
            }
        }
        unsigned char config[ZPT_STAGE_TUNING_MAX_CONFIG_SIZE] = {0};
        int32_t value;
        assert(ops->get(config, 200, &value) == -ENOENT);
        assert(ops->put(config, 200, 1) == -ENOENT);
    }
}

/* Devicetree micrometres and steps per metre convert to fixed point and back
 * without drifting, so compiled values display as authored. */
static void test_devicetree_units_read_back_exactly(void) {
    const int32_t micrometers[] = {1, 220, 581, 1270, 2721, 19999};
    for (size_t i = 0; i < sizeof(micrometers) / sizeof(micrometers[0]); i++) {
        assert(zpt_stage_tuning_fixed_millimeters_to_micrometers(
                   ZPT_MICROMETERS_TO_FIXED_MILLIMETERS(micrometers[i])) == micrometers[i]);
    }
    assert(zpt_stage_tuning_fixed_millimeters_to_micrometers(
               -ZPT_MICROMETERS_TO_FIXED_MILLIMETERS(581)) == -581);
    const int32_t per_meter[] = {1, 3445, 9999, 100000};
    for (size_t i = 0; i < sizeof(per_meter) / sizeof(per_meter[0]); i++) {
        assert(zpt_stage_tuning_fixed_per_millimeter_to_per_meter(
                   ZPT_PER_METER_TO_FIXED_PER_MILLIMETER(per_meter[i])) == per_meter[i]);
    }
    assert(zpt_stage_tuning_fixed_millimeters_to_micrometers(INT64_MAX / 2000) == INT32_MAX);
}

/* A batch lands in a copy; the live config is untouched until applied. */
static void test_prepare_builds_a_candidate(void) {
    const struct zpt_scroll_batcher_config current = {
        .steps_per_millimeter = ZPT_PER_METER_TO_FIXED_PER_MILLIMETER(3445),
        .report_interval_ms = 16,
        .suppression = (const struct zpt_suppression_policy *)&current,
    };
    struct zpt_scroll_batcher_config candidate;
    const struct zpt_tuning_value values[] = {{0, 5000}, {1, 8}};
    uint8_t failed = UINT8_MAX;

    assert(zpt_stage_tuning_prepare(&zpt_scroll_batcher_tuning_ops, &current, values, 2, &candidate,
                                    &failed) == 0);
    assert(get(&zpt_scroll_batcher_tuning_ops, &candidate, 0) == 5000);
    assert(candidate.report_interval_ms == 8);
    /* Fields tuning doesn't own are carried over. */
    assert(candidate.suppression == current.suppression);
    assert(current.report_interval_ms == 16);

    const struct zpt_tuning_value unknown[] = {{0, 4000}, {9, 1}};
    assert(zpt_stage_tuning_prepare(&zpt_scroll_batcher_tuning_ops, &current, unknown, 2,
                                    &candidate, &failed) == -ENOENT);
    assert(failed == 9);
    assert(zpt_stage_tuning_prepare(NULL, &current, values, 2, &candidate, &failed) == -EINVAL);
}

static void test_axis_intent_rejects_release_above_engage(void) {
    const struct zpt_axis_intent_stage_config current = {
        .settings =
            {
                .engage_ratio_percent = 300,
                .release_ratio_percent = 180,
                .activation_distance = ZPT_MICROMETERS_TO_FIXED_MILLIMETERS(581),
                .window_ms = 64,
            },
        .policy = ZPT_AXIS_POLICY_ADAPTIVE,
        .idle_timeout_ms = 120,
    };
    struct zpt_axis_intent_stage_config candidate;
    uint8_t failed = UINT8_MAX;

    const struct zpt_tuning_value too_high[] = {{1, 320}};
    assert(zpt_stage_tuning_prepare(&zpt_axis_intent_tuning_ops, &current, too_high, 1, &candidate,
                                    &failed) == -EINVAL);
    assert(failed == 1);

    /* Raising both together in one batch is fine. */
    const struct zpt_tuning_value both[] = {{1, 320}, {0, 400}};
    assert(zpt_stage_tuning_prepare(&zpt_axis_intent_tuning_ops, &current, both, 2, &candidate,
                                    &failed) == 0);
    assert(candidate.settings.release_ratio_percent == 320);
    assert(candidate.policy == ZPT_AXIS_POLICY_ADAPTIVE);
}

static void test_reset_restores_compiled_tunables_only(void) {
    const struct zpt_text_nav_config compiled = {
        .horizontal_threshold = ZPT_MICROMETERS_TO_FIXED_MILLIMETERS(2721),
        .vertical_threshold = ZPT_MICROMETERS_TO_FIXED_MILLIMETERS(2721),
        .idle_timeout_ms = 40,
    };
    const struct zpt_text_nav_config current = {
        .horizontal_threshold = ZPT_MICROMETERS_TO_FIXED_MILLIMETERS(1000),
        .vertical_threshold = ZPT_MICROMETERS_TO_FIXED_MILLIMETERS(3000),
        .idle_timeout_ms = 90,
    };
    struct zpt_text_nav_config candidate;
    assert(zpt_stage_tuning_prepare_reset(&zpt_text_nav_tuning_ops, &compiled, &current,
                                          &candidate) == 0);
    assert(candidate.horizontal_threshold == compiled.horizontal_threshold);
    assert(candidate.vertical_threshold == compiled.vertical_threshold);
    assert(candidate.idle_timeout_ms == compiled.idle_timeout_ms);

    const struct zpt_coherent_displacement_stage_config gate_compiled = {
        .settings = {.enabled = true,
                     .activation_distance = ZPT_MICROMETERS_TO_FIXED_MILLIMETERS(220),
                     .coherence_percent = 60,
                     .qualification_timeout_ms = 160,
                     .idle_timeout_ms = 120},
    };
    struct zpt_coherent_displacement_stage_config gate_current = gate_compiled;
    gate_current.settings.enabled = false;
    gate_current.settings.coherence_percent = 10;
    struct zpt_coherent_displacement_stage_config gate_candidate;
    assert(zpt_stage_tuning_prepare_reset(&zpt_coherent_displacement_tuning_ops, &gate_compiled,
                                          &gate_current, &gate_candidate) == 0);
    assert(gate_candidate.settings.enabled);
    assert(gate_candidate.settings.coherence_percent == 60);
    assert(gate_candidate.settings.activation_distance ==
           gate_compiled.settings.activation_distance);
}

static int apply_calls;
static int apply_result;

static int fake_apply(void *current, const void *candidate, size_t size) {
    apply_calls++;
    if (apply_result == 0) {
        memcpy(current, candidate, size);
    }
    return apply_result;
}

/* The tuning target reads compiled or live values and only ever replaces the
 * live config whole, through apply, once a candidate is complete. */
static void test_binding_drives_a_tuning_target(void) {
    const struct zpt_scroll_batcher_config compiled = {
        .steps_per_millimeter = ZPT_PER_METER_TO_FIXED_PER_MILLIMETER(3445),
        .report_interval_ms = 16,
    };
    struct zpt_scroll_batcher_config current = compiled;
    current.suppression = (const struct zpt_suppression_policy *)&compiled;
    struct zpt_stage_tuning_binding binding = {
        .ops = &zpt_scroll_batcher_tuning_ops,
        .compiled = &compiled,
        .current = &current,
        .apply = fake_apply,
    };
    struct zpt_tuning_target target;
    assert(zpt_stage_tuning_target_init(&target, &binding, "scroll-batcher", "/zpt/scroll") == 0);
    assert(target.kind == ZPT_TUNING_TARGET_PIPELINE_STAGE);
    assert(strcmp(target.label, "scroll-batcher") == 0);
    assert(target.parameter_count == zpt_scroll_batcher_tuning_ops.parameter_count);

    apply_calls = 0;
    apply_result = 0;
    const struct zpt_tuning_value faster[] = {{0, 5000}, {1, 8}};
    uint8_t failed = UINT8_MAX;
    assert(target.set_many(target.context, faster, 2, &failed) == 0);
    assert(apply_calls == 1);
    int32_t value;
    assert(target.get(target.context, 0, false, &value) == 0 && value == 5000);
    assert(target.get(target.context, 0, true, &value) == 0 && value == 3445);
    assert(current.report_interval_ms == 8);

    /* A rejected batch never reaches apply. */
    const struct zpt_tuning_value unknown[] = {{1, 4}, {7, 1}};
    assert(target.set_many(target.context, unknown, 2, &failed) == -ENOENT);
    assert(failed == 7 && apply_calls == 1 && current.report_interval_ms == 8);

    /* Apply failures surface and leave the live config alone. */
    apply_result = -EBUSY;
    assert(target.set_many(target.context, faster, 1, &failed) == -EBUSY);
    assert(target.reset(target.context) == -EBUSY);
    assert(current.report_interval_ms == 8);

    /* Reset restores compiled tunables and keeps the resolved policy. */
    apply_result = 0;
    assert(target.reset(target.context) == 0);
    assert(current.report_interval_ms == 16);
    assert(current.steps_per_millimeter == compiled.steps_per_millimeter);
    assert(current.suppression == (const struct zpt_suppression_policy *)&compiled);
}

static void test_binding_rejects_malformed_targets(void) {
    struct zpt_cursor_transfer_config config = {.scale_multiplier = 1, .scale_divisor = 1};
    struct zpt_stage_tuning_binding binding = {
        .ops = &zpt_cursor_transfer_tuning_ops,
        .compiled = &config,
        .current = &config,
        .apply = fake_apply,
    };
    struct zpt_tuning_target target;
    assert(zpt_stage_tuning_target_init(NULL, &binding, "transfer", "/t") == -EINVAL);
    assert(zpt_stage_tuning_target_init(&target, NULL, "transfer", "/t") == -EINVAL);

    struct zpt_stage_tuning_binding no_apply = binding;
    no_apply.apply = NULL;
    assert(zpt_stage_tuning_target_init(&target, &no_apply, "transfer", "/t") == -EINVAL);

    struct zpt_stage_tuning_ops oversized = zpt_cursor_transfer_tuning_ops;
    oversized.config_size = ZPT_STAGE_TUNING_MAX_CONFIG_SIZE + 1;
    struct zpt_stage_tuning_binding too_big = binding;
    too_big.ops = &oversized;
    assert(zpt_stage_tuning_target_init(&target, &too_big, "transfer", "/t") == -EINVAL);
}

int main(void) {
    test_parameter_tables_round_trip();
    test_devicetree_units_read_back_exactly();
    test_prepare_builds_a_candidate();
    test_axis_intent_rejects_release_above_engage();
    test_reset_restores_compiled_tunables_only();
    test_binding_drives_a_tuning_target();
    test_binding_rejects_malformed_targets();
    puts("stage tuning tests passed");
    return 0;
}
