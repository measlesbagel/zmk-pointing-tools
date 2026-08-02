/* SPDX-License-Identifier: MIT */
#define DT_DRV_COMPAT measlesbagel_zpt_input_processor_sign_extend_xy

#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/dt-bindings/input/input-event-codes.h>
#include <zephyr/sys/util.h>

#include <drivers/input_processor.h>

static int zpt_sign_extend_xy_handle_event(const struct device *dev, struct input_event *event,
                                           uint32_t param1, uint32_t param2,
                                           struct zmk_input_processor_state *state) {
    ARG_UNUSED(dev);
    ARG_UNUSED(param1);
    ARG_UNUSED(param2);
    ARG_UNUSED(state);

    if (event->type == INPUT_EV_REL && (event->code == INPUT_REL_X || event->code == INPUT_REL_Y)) {
        event->value = (int16_t)(uint16_t)event->value;
    }

    return ZMK_INPUT_PROC_CONTINUE;
}

static const struct zmk_input_processor_driver_api zpt_sign_extend_xy_driver_api = {
    .handle_event = zpt_sign_extend_xy_handle_event,
};

#define ZPT_SIGN_EXTEND_XY_DEFINE(inst)                                                            \
    DEVICE_DT_INST_DEFINE(inst, NULL, NULL, NULL, NULL, POST_KERNEL,                               \
                          CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &zpt_sign_extend_xy_driver_api);

DT_INST_FOREACH_STATUS_OKAY(ZPT_SIGN_EXTEND_XY_DEFINE)
