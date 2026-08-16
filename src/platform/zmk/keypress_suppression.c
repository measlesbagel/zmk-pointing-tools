/* SPDX-License-Identifier: MIT */
#define DT_DRV_COMPAT measlesbagel_zpt_keypress_suppression

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>

#include <zmk/event_manager.h>
#include <zmk/events/position_state_changed.h>

#include <zmk/pointing_tools/platform/zmk/keypress_suppression.h>

struct zpt_keypress_suppression_config {
    uint32_t suppress_after_ms;
};

struct zpt_keypress_suppression_data {
    uint32_t suppress_after_ms;
    /* Written by the ZMK event listener thread, read by the input thread. */
    atomic_t last_keypress_ms;
    atomic_t have_keypress;
    struct zpt_suppression_policy policy;
};

static bool keypress_is_suppressed(void *context, const struct zpt_signal *signal,
                                   uint32_t now_ms) {
    (void)signal;
    struct zpt_keypress_suppression_data *data = context;
    /* have_keypress is published after last_keypress_ms, so observing it
     * nonzero means the timestamp is already the newest press. */
    return atomic_get(&data->have_keypress) != 0 &&
           now_ms - (uint32_t)atomic_get(&data->last_keypress_ms) < data->suppress_after_ms;
}

static const struct zpt_suppression_policy *
keypress_suppression_get_policy(const struct device *dev) {
    struct zpt_keypress_suppression_data *data = dev->data;
    return &data->policy;
}

static DEVICE_API(zpt_keypress_suppression, keypress_suppression_api) = {
    .get_policy = keypress_suppression_get_policy,
};

int zpt_zmk_keypress_suppression_get(const struct device *dev,
                                     const struct zpt_suppression_policy **policy) {
    if (dev == NULL || policy == NULL) {
        return -EINVAL;
    }
    if (!device_is_ready(dev)) {
        return -ENODEV;
    }
    if (!DEVICE_API_IS(zpt_keypress_suppression, dev)) {
        return -EPROTOTYPE;
    }
    const struct zpt_keypress_suppression_driver_api *api =
        DEVICE_API_GET(zpt_keypress_suppression, dev);
    if (api->get_policy == NULL) {
        return -ENOSYS;
    }
    const struct zpt_suppression_policy *candidate = api->get_policy(dev);
    if (candidate == NULL) {
        return -ENODEV;
    }
    *policy = candidate;
    return 0;
}

static int keypress_suppression_init(const struct device *dev) {
    const struct zpt_keypress_suppression_config *config = dev->config;
    struct zpt_keypress_suppression_data *data = dev->data;
    data->suppress_after_ms = config->suppress_after_ms;
    data->policy = (struct zpt_suppression_policy){
        .is_suppressed = keypress_is_suppressed,
        .context = data,
    };
    return 0;
}

static int position_state_changed_listener(const zmk_event_t *event) {
    const struct zmk_position_state_changed *position = as_zmk_position_state_changed(event);
    if (position == NULL || !position->state) {
        return ZMK_EV_EVENT_BUBBLE;
    }

#define ZPT_KEYPRESS_SUPPRESSION_RECORD(inst)                                                      \
    do {                                                                                           \
        const struct device *dev = DEVICE_DT_INST_GET(inst);                                       \
        if (device_is_ready(dev)) {                                                                \
            struct zpt_keypress_suppression_data *data = dev->data;                                \
            /* Publish the timestamp first; both accesses are seq-cst, so                          \
             * observing the flag implies the timestamp is visible.            */                  \
            atomic_set(&data->last_keypress_ms, (atomic_val_t)position->timestamp);                \
            atomic_set(&data->have_keypress, 1);                                                   \
        }                                                                                          \
    } while (false);

    DT_INST_FOREACH_STATUS_OKAY(ZPT_KEYPRESS_SUPPRESSION_RECORD)
#undef ZPT_KEYPRESS_SUPPRESSION_RECORD
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(zpt_keypress_suppression, position_state_changed_listener);
ZMK_SUBSCRIPTION(zpt_keypress_suppression, zmk_position_state_changed);

#define ZPT_KEYPRESS_SUPPRESSION_DEFINE(inst)                                                      \
    static struct zpt_keypress_suppression_data zpt_keypress_suppression_data_##inst;              \
    static const struct zpt_keypress_suppression_config zpt_keypress_suppression_config_##inst = { \
        .suppress_after_ms = DT_INST_PROP(inst, suppress_after_keypress_ms),                       \
    };                                                                                             \
    DEVICE_DT_INST_DEFINE(inst, keypress_suppression_init, NULL,                                   \
                          &zpt_keypress_suppression_data_##inst,                                   \
                          &zpt_keypress_suppression_config_##inst, POST_KERNEL,                    \
                          CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &keypress_suppression_api);

DT_INST_FOREACH_STATUS_OKAY(ZPT_KEYPRESS_SUPPRESSION_DEFINE)
