/* SPDX-License-Identifier: MIT */

#include <string.h>

#include <zephyr/logging/log.h>

#include <zmk/pointing_tools/platform/zmk/router.h>
#include <zmk/pointing_tools/platform/zmk/stage_tuning.h>
#if IS_ENABLED(CONFIG_ZMK_POINTING_TOOLS_INPUT_CAPTURE)
#include <zmk/pointing_tools/service/capture.h>
#endif

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

/* Stages read their config on the input thread and from deadline work, and
 * a config holds 64-bit fields, so a change lands only while no router is
 * mid-frame. */
static int apply_between_frames(void *current, const void *candidate, size_t size) {
    int ret = zpt_zmk_router_lock_all();
    if (ret < 0) {
        return ret;
    }
    memcpy(current, candidate, size);
#if IS_ENABLED(CONFIG_ZMK_POINTING_TOOLS_INPUT_CAPTURE)
    /* Marks where the recorded settings stop applying. */
    zpt_capture_settings_changed();
#endif
    return zpt_zmk_router_unlock_all();
}

void zpt_zmk_stage_tuning_register(struct zpt_zmk_stage_tuning *tuning,
                                   const struct zpt_stage_tuning_ops *ops, const char *stable_id,
                                   const char *devicetree_path, const void *compiled,
                                   void *current) {
    tuning->binding = (struct zpt_stage_tuning_binding){
        .ops = ops,
        .compiled = compiled,
        .current = current,
        .apply = apply_between_frames,
    };
    int ret =
        zpt_stage_tuning_target_init(&tuning->target, &tuning->binding, stable_id, devicetree_path);
    if (ret == 0) {
        ret = zpt_tuning_register(&tuning->target);
    }
    if (ret < 0) {
        LOG_WRN("Stage %s is not runtime tunable: %d", stable_id, ret);
    }
}
