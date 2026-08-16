/* SPDX-License-Identifier: MIT */

#include <errno.h>

#include <zephyr/device.h>

#include <zmk/pointing_tools/platform/zmk/sink_provider.h>

int zpt_zmk_sink_provider_init(const struct device *dev, const char *stable_id,
                               const struct zpt_sink_api *api, const void *config, void *state) {
    if (dev == NULL || dev->data == NULL || stable_id == NULL || stable_id[0] == '\0' ||
        api == NULL) {
        return -EINVAL;
    }

    struct zpt_zmk_sink_provider_data *data = dev->data;
    data->sink = (struct zpt_sink){
        .stable_id = stable_id,
        .api = api,
        .config = config,
        .state = state,
    };
    return 0;
}

static int validate_provider(const struct device *dev) {
    if (dev == NULL) {
        return -EINVAL;
    }
    if (!device_is_ready(dev)) {
        return -ENODEV;
    }
    return DEVICE_API_IS(zpt_sink_provider, dev) ? 0 : -EPROTOTYPE;
}

int zpt_zmk_sink_provider_get(const struct device *dev, struct zpt_sink **sink) {
    if (sink == NULL) {
        return -EINVAL;
    }
    int ret = validate_provider(dev);
    if (ret < 0) {
        return ret;
    }

    const struct zpt_sink_provider_driver_api *api = DEVICE_API_GET(zpt_sink_provider, dev);
    if (api->get_sink == NULL) {
        return -ENOSYS;
    }
    struct zpt_sink *candidate = api->get_sink(dev);
    if (candidate == NULL) {
        return -ENODEV;
    }
    *sink = candidate;
    return 0;
}

int zpt_zmk_sink_provider_bind_output(const struct device *dev,
                                      const struct device *output_device) {
    if (output_device == NULL) {
        return -EINVAL;
    }
    int ret = validate_provider(dev);
    if (ret < 0) {
        return ret;
    }

    const struct zpt_sink_provider_driver_api *api = DEVICE_API_GET(zpt_sink_provider, dev);
    return api->bind_output == NULL ? -ENOTSUP : api->bind_output(dev, output_device);
}
