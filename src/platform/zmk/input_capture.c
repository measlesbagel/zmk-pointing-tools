/* SPDX-License-Identifier: MIT */

/* Connects ZMK to the input capture service: every devicetree pipeline is
 * recordable, and recordings start between frames. */

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/init.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <zmk/pointing_tools/platform/zmk/pipeline_provider.h>
#include <zmk/pointing_tools/platform/zmk/router.h>
#include <zmk/pointing_tools/service/capture.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define ZPT_CAPTURE_PIPELINE_DEVICE(node_id) DEVICE_DT_GET(node_id),

/* NULL-terminated so a build without pipelines still has a valid array. */
static const struct device *const zpt_capture_pipeline_devices[] = {
    DT_FOREACH_STATUS_OKAY(measlesbagel_zpt_pipeline, ZPT_CAPTURE_PIPELINE_DEVICE) NULL};

int zpt_capture_between_frames(void (*fn)(void *arg), void *arg) {
    int ret = zpt_zmk_router_lock_all();
    if (ret < 0) {
        return ret;
    }
    fn(arg);
    return zpt_zmk_router_unlock_all();
}

static void zpt_input_capture_register_all(void *arg) {
    ARG_UNUSED(arg);
    for (size_t i = 0; zpt_capture_pipeline_devices[i] != NULL; i++) {
        struct zpt_pipeline *pipeline;
        int ret = zpt_zmk_pipeline_provider_get(zpt_capture_pipeline_devices[i], &pipeline);
        if (ret == 0) {
            ret = zpt_capture_register_pipeline(pipeline);
        }
        if (ret < 0) {
            LOG_WRN("Pipeline %u can't be recorded: %d", (unsigned int)i, ret);
        }
    }
}

static int zpt_input_capture_init(void) {
    /* Sensors may already report, so observers go in between frames. */
    return zpt_capture_between_frames(zpt_input_capture_register_all, NULL);
}

/* After every pipeline provider and router (POST_KERNEL), before any input
 * reaches a pipeline. */
SYS_INIT(zpt_input_capture_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
