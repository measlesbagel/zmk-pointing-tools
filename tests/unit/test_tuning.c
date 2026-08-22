/* SPDX-License-Identifier: MIT */

/* Unit tests for the runtime tuning service (src/service/tuning.c), the
 * Zephyr-side half of the runtime-tuning contract the host replay harness
 * cannot reach. The target registry is a process-wide singleton with no
 * unregister API: fakes are registered in suite setup and the capacity
 * fill runs last of all (z_capacity_fill in test_telemetry.c). All cases
 * of this binary share the single zpt_unit suite: ztest runs suites and
 * cases in name-sorted linker order, and the shared registry state makes
 * name order the only reliable execution order. */

#include <errno.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/ztest.h>

#include <zmk/pointing_tools/service/tuning.h>

#define ZPT_FAKE_MAX_TARGETS CONFIG_ZMK_POINTING_TOOLS_TUNING_MAX_TARGETS

struct zpt_fake_target {
    int32_t values[2];
    int32_t compiled[2];
    int reset_calls;
};

static struct zpt_fake_target fake_a;
static struct zpt_fake_target fake_b;

static int fake_get(void *context, uint8_t parameter_id, bool compiled, int32_t *value) {
    struct zpt_fake_target *fake = context;

    if (parameter_id >= 2) {
        return -ENOENT;
    }
    *value = compiled ? fake->compiled[parameter_id] : fake->values[parameter_id];
    return 0;
}

static int fake_set_many(void *context, const struct zpt_tuning_value *values, size_t value_count,
                         uint8_t *failed_parameter_id) {
    struct zpt_fake_target *fake = context;

    ARG_UNUSED(failed_parameter_id);
    for (size_t i = 0; i < value_count; i++) {
        fake->values[values[i].parameter_id] = values[i].value;
    }
    return 0;
}

static int fake_reset(void *context) {
    struct zpt_fake_target *fake = context;

    fake->reset_calls++;
    for (size_t i = 0; i < 2; i++) {
        fake->values[i] = fake->compiled[i];
    }
    return 0;
}

static int fake_reset_fail(void *context) {
    struct zpt_fake_target *fake = context;

    fake->reset_calls++;
    return -EIO;
}

static const struct zpt_tuning_parameter target_a_parameters[2] = {
    {
        .id = 0,
        .type = ZPT_TUNING_VALUE_INTEGER,
        .minimum = 0,
        .maximum = 100,
        .step = 5,
        .key = "gain",
        .devicetree_property = "measlesbagel,gain",
        .label = "Gain",
        .unit = "cpi",
        .description = "Sensitivity gain.",
    },
    {
        .id = 1,
        .type = ZPT_TUNING_VALUE_BOOLEAN,
        .minimum = 0,
        .maximum = 1,
        .step = 1,
        .key = "enabled",
        .devicetree_property = "measlesbagel,enabled",
        .label = "Enabled",
        .unit = "",
        .description = "Enable the target.",
    },
};

static const struct zpt_tuning_parameter target_b_parameters[1] = {
    {
        .id = 0,
        .type = ZPT_TUNING_VALUE_INTEGER,
        .minimum = 0,
        .maximum = 10,
        .step = 1,
        .key = "threshold",
        .devicetree_property = "measlesbagel,threshold",
        .label = "Threshold",
        .unit = "",
        .description = "Gate threshold.",
    },
};

static const struct zpt_tuning_target target_a = {
    .stable_id = "zpt-test-target-a",
    .label = "Target A",
    .devicetree_path = "/zpt/a",
    .kind = ZPT_TUNING_TARGET_PIPELINE_STAGE,
    .parameter_count = 2,
    .parameters = target_a_parameters,
    .context = &fake_a,
    .get = fake_get,
    .set_many = fake_set_many,
    .reset = fake_reset,
};

static const struct zpt_tuning_target target_b = {
    .stable_id = "zpt-test-target-b",
    .label = "Target B",
    .devicetree_path = "/zpt/b",
    .kind = ZPT_TUNING_TARGET_PIPELINE_STAGE,
    .parameter_count = 1,
    .parameters = target_b_parameters,
    .context = &fake_b,
    .get = fake_get,
    .set_many = fake_set_many,
    .reset = fake_reset_fail,
};

/* Suite setup for every case in this binary: the tuning registry is a
 * process-wide singleton with no unregister API, so the fakes are
 * registered once, up front. ztest runs suites and cases in
 * name-sorted order (the linker script sorts the __test_suite and
 * __unit_test sections by name), so the capacity-fill case — the only
 * one that permanently consumes target IDs — is named z_capacity_fill in
 * test_telemetry.c so that it sorts after every other case. */
extern const struct device *const uart_dev;

static void *zpt_setup(void) {
    zassert_true(device_is_ready(uart_dev));
    fake_a.compiled[0] = 10;
    fake_a.compiled[1] = 1;
    fake_b.compiled[0] = 5;
    zassert_equal(zpt_tuning_register(&target_a), 0);
    zassert_equal(zpt_tuning_register(&target_b), 1);
    zassert_equal(zpt_tuning_target_count(), 2);
    return NULL;
}

ZTEST_SUITE(zpt_unit, NULL, zpt_setup, NULL, NULL, NULL);

ZTEST(zpt_unit, register_invalid) {
    struct zpt_tuning_target bad = target_a;

    zassert_equal(zpt_tuning_register(NULL), -EINVAL);
    bad.stable_id = NULL;
    zassert_equal(zpt_tuning_register(&bad), -EINVAL);
    bad = target_a;
    bad.get = NULL;
    zassert_equal(zpt_tuning_register(&bad), -EINVAL);
    bad = target_a;
    bad.parameter_count = 0;
    zassert_equal(zpt_tuning_register(&bad), -EINVAL);

    struct zpt_tuning_parameter bad_param = target_a_parameters[0];
    bad_param.key = NULL;
    bad = target_a;
    bad.parameters = &bad_param;
    bad.parameter_count = 1;
    zassert_equal(zpt_tuning_register(&bad), -EINVAL);

    struct zpt_tuning_parameter duplicate[2] = {target_a_parameters[0], target_a_parameters[0]};
    bad = target_a;
    bad.parameters = duplicate;
    bad.parameter_count = 2;
    zassert_equal(zpt_tuning_register(&bad), -EEXIST);

    duplicate[1] = target_a_parameters[1];
    duplicate[1].key = "gain";
    zassert_equal(zpt_tuning_register(&bad), -EEXIST);
    zassert_equal(zpt_tuning_target_count(), 2);
}

ZTEST(zpt_unit, register_duplicate) {
    struct zpt_tuning_target clone = target_b;
    struct zpt_tuning_parameter clone_parameters = target_b_parameters[0];
    struct zpt_tuning_target distinct = {
        .kind = target_a.kind,
        .stable_id = "zpt-test-target-a",
        .label = "Clone A",
        .devicetree_path = "/zpt/clone-a",
        .parameter_count = 1,
        .parameters = &clone_parameters,
        .context = &fake_b,
        .get = fake_get,
        .set_many = fake_set_many,
        .reset = fake_reset,
    };
    ARG_UNUSED(clone);

    /* Same pointer: idempotent, returns the existing index. */
    zassert_equal(zpt_tuning_register(&target_a), 0);
    /* Same stable id, different object: rejected. */
    zassert_equal(zpt_tuning_register(&distinct), -EEXIST);
    zassert_equal(zpt_tuning_target_count(), 2);
    zassert_is_null(zpt_tuning_target_get(99));
    zassert_equal(zpt_tuning_target_get(0), &target_a);
}

ZTEST(zpt_unit, get_set) {
    int32_t value = -1;

    zassert_equal(zpt_tuning_get(99, 0, false, &value), -ENODEV);
    zassert_equal(zpt_tuning_get(0, 99, false, &value), -ENOENT);

    zassert_equal(zpt_tuning_get(0, 0, true, &value), 0);
    zassert_equal(value, 10);
    zassert_equal(zpt_tuning_get(0, 0, false, &value), 0);
    zassert_equal(value, 0);

    zassert_equal(zpt_tuning_set(0, 0, 50), 0);
    zassert_equal(zpt_tuning_get(0, 0, false, &value), 0);
    zassert_equal(value, 50);

    zassert_equal(zpt_tuning_set(0, 0, 105), -ERANGE);
    zassert_equal(zpt_tuning_set(0, 0, 52), -ERANGE);
    zassert_equal(zpt_tuning_set(0, 1, 2), -ERANGE);
    zassert_equal(zpt_tuning_set(99, 0, 1), -ENODEV);
    zassert_equal(zpt_tuning_get(0, 0, false, &value), 0);
    zassert_equal(value, 50);

    struct zpt_tuning_value updates[2] = {{.parameter_id = 0, .value = 10},
                                          {.parameter_id = 1, .value = 1}};
    uint8_t failed = UINT8_MAX;
    zassert_equal(zpt_tuning_set_many(0, updates, 2, &failed), 0);
    zassert_equal(zpt_tuning_get(0, 0, false, &value), 0);
    zassert_equal(value, 10);
    zassert_equal(zpt_tuning_get(0, 1, false, &value), 0);
    zassert_equal(value, 1);

    struct zpt_tuning_value duplicate[2] = {{.parameter_id = 0, .value = 10},
                                            {.parameter_id = 0, .value = 20}};
    failed = UINT8_MAX;
    zassert_equal(zpt_tuning_set_many(0, duplicate, 2, &failed), -EEXIST);
    zassert_equal(failed, 0);
    zassert_equal(zpt_tuning_get(0, 0, false, &value), 0);
    zassert_equal(value, 10);

    struct zpt_tuning_value out_of_range[1] = {{.parameter_id = 0, .value = 105}};
    zassert_equal(zpt_tuning_set_many(0, out_of_range, 1, &failed), -ERANGE);
    zassert_equal(zpt_tuning_set_many(99, updates, 1, &failed), -ENODEV);
    zassert_equal(zpt_tuning_set_many(0, NULL, 0, &failed), -EINVAL);
}

/* Named target_reset: ZTEST generates a function named after the case,
 * which would otherwise collide with the zpt_tuning_reset() API. */
ZTEST(zpt_unit, target_reset) {
    int32_t value = -1;

    zassert_equal(zpt_tuning_set(0, 0, 50), 0);
    zassert_equal(zpt_tuning_reset(0), 0);
    zassert_equal(fake_a.reset_calls, 1);
    zassert_equal(zpt_tuning_get(0, 0, false, &value), 0);
    zassert_equal(value, 10);
    zassert_equal(zpt_tuning_reset(99), -ENODEV);

    /* reset_all keeps going past a failing target and reports the first
     * error: A resets cleanly, B's fake fails with -EIO. */
    zassert_equal(zpt_tuning_set(0, 0, 50), 0);
    zassert_equal(zpt_tuning_set(1, 0, 7), 0);
    const int before = fake_b.reset_calls;
    zassert_equal(zpt_tuning_reset_all(), -EIO);
    zassert_equal(fake_a.reset_calls, 2);
    zassert_equal(fake_b.reset_calls, before + 1);
    zassert_equal(zpt_tuning_get(0, 0, false, &value), 0);
    zassert_equal(value, 10);
}
