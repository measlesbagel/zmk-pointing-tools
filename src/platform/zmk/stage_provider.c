/* SPDX-License-Identifier: MIT */

#include <errno.h>

#include <zephyr/device.h>

#include <zmk/pointing_tools/platform/zmk/stage_provider.h>

static struct zpt_stage *get_stage(const struct device *dev) {
    struct zpt_zmk_stage_provider_data *data = dev->data;
    return &data->stage;
}

DEVICE_API(zpt_stage_provider, zpt_stage_provider_api) = {
    .get_stage = get_stage,
};

int zpt_zmk_stage_provider_init(const struct device *dev, const char *stable_id,
                                const struct zpt_stage_api *api, const void *config, void *state) {
    if (dev == NULL || dev->data == NULL || stable_id == NULL || stable_id[0] == '\0' ||
        api == NULL) {
        return -EINVAL;
    }

    struct zpt_zmk_stage_provider_data *data = dev->data;
    data->stage = (struct zpt_stage){
        .stable_id = stable_id,
        .api = api,
        .config = config,
        .state = state,
    };
    return 0;
}

int zpt_zmk_stage_provider_get(const struct device *dev, struct zpt_stage **stage) {
    if (dev == NULL || stage == NULL) {
        return -EINVAL;
    }
    if (!device_is_ready(dev)) {
        return -ENODEV;
    }
    if (!DEVICE_API_IS(zpt_stage_provider, dev)) {
        return -EPROTOTYPE;
    }

    const struct zpt_stage_provider_driver_api *api = DEVICE_API_GET(zpt_stage_provider, dev);
    if (api->get_stage == NULL) {
        return -ENOSYS;
    }
    struct zpt_stage *candidate = api->get_stage(dev);
    if (candidate == NULL) {
        return -ENODEV;
    }
    *stage = candidate;
    return 0;
}
