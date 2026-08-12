/* SPDX-License-Identifier: MIT */
#define DT_DRV_COMPAT measlesbagel_zpt_router

#include <errno.h>
#include <limits.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <zmk/event_manager.h>
#include <zmk/events/layer_state_changed.h>
#include <zmk/keymap.h>

#include <zmk/pointing_tools/platform/zmk/pipeline_provider.h>
#include <zmk/pointing_tools/platform/zmk/router.h>
#include <zmk/pointing_tools/platform/zmk/router_executor.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

struct zpt_layer_route_config {
    const struct device *pipeline_device;
    const uint8_t *layers;
    size_t layer_count;
};

struct zpt_router_config {
    const char *stable_id;
    const struct device *const *pipeline_devices;
    size_t pipeline_count;
    const struct device *default_pipeline_device;
    const struct zpt_layer_route_config *layer_routes;
    size_t layer_route_count;
};

struct zpt_explicit_route_override {
    uint32_t position;
    size_t pipeline_index;
    uint64_t order;
    bool active;
};

struct zpt_router_data {
    struct zpt_pipeline **pipelines;
    size_t *layer_route_pipeline_indices;
    struct k_mutex route_lock;
    struct zpt_explicit_route_override
        overrides[CONFIG_ZMK_POINTING_TOOLS_ROUTER_MAX_EXPLICIT_ROUTES];
    uint64_t next_override_order;
    struct zpt_router router;
    struct zpt_zmk_router_executor executor;
};

static int pipeline_device_index(const struct zpt_router_config *config,
                                 const struct device *pipeline_device, size_t *index) {
    for (size_t candidate = 0; candidate < config->pipeline_count; candidate++) {
        if (config->pipeline_devices[candidate] == pipeline_device) {
            *index = candidate;
            return 0;
        }
    }
    return -ENOENT;
}

static int selected_layer_pipeline(const struct device *dev, size_t *pipeline_index) {
    const struct zpt_router_config *config = dev->config;
    struct zpt_router_data *data = dev->data;

    /* Match ZMK's binding resolution: walk the keymap stack from the top,
     * compare route values against positional layer indices (stable even when
     * layer reordering reassigns layer IDs at runtime), and stop after the
     * default layer because ZMK never resolves bindings below it. */
    for (int layer_index = ZMK_KEYMAP_LAYERS_LEN - 1; layer_index >= 0; layer_index--) {
        zmk_keymap_layer_id_t layer = zmk_keymap_layer_index_to_id(layer_index);
        if (layer == ZMK_KEYMAP_LAYER_ID_INVAL) {
            continue;
        }
        if (zmk_keymap_layer_active(layer)) {
            for (size_t route_index = 0; route_index < config->layer_route_count; route_index++) {
                const struct zpt_layer_route_config *route = &config->layer_routes[route_index];
                for (size_t route_layer_index = 0; route_layer_index < route->layer_count;
                     route_layer_index++) {
                    if (route->layers[route_layer_index] == layer_index) {
                        *pipeline_index = data->layer_route_pipeline_indices[route_index];
                        return 0;
                    }
                }
            }
        }
        if (layer == zmk_keymap_layer_default()) {
            break;
        }
    }
    *pipeline_index = data->router.default_pipeline_index;
    return 0;
}

static bool selected_override_pipeline(const struct zpt_router_data *data, size_t *pipeline_index) {
    const struct zpt_explicit_route_override *selected = NULL;
    for (size_t index = 0; index < ARRAY_SIZE(data->overrides); index++) {
        const struct zpt_explicit_route_override *candidate = &data->overrides[index];
        if (candidate->active && (selected == NULL || candidate->order > selected->order)) {
            selected = candidate;
        }
    }
    if (selected == NULL) {
        return false;
    }
    *pipeline_index = selected->pipeline_index;
    return true;
}

static int select_desired_route_locked(const struct device *dev, uint32_t now_ms) {
    struct zpt_router_data *data = dev->data;
    size_t pipeline_index;
    int ret = selected_override_pipeline(data, &pipeline_index)
                  ? 0
                  : selected_layer_pipeline(dev, &pipeline_index);
    if (ret < 0) {
        return ret;
    }

    struct zpt_pipeline_result result;
    return zpt_zmk_router_executor_select(&data->executor, pipeline_index, now_ms, &result);
}

static int unlock_route(struct zpt_router_data *data, int result) {
    int unlock_result = k_mutex_unlock(&data->route_lock);
    return result < 0 ? result : unlock_result;
}

static int router_refresh_layer_route(const struct device *dev, uint32_t now_ms) {
    struct zpt_router_data *data = dev->data;
    int ret = k_mutex_lock(&data->route_lock, K_FOREVER);
    if (ret < 0) {
        return ret;
    }
    return unlock_route(data, select_desired_route_locked(dev, now_ms));
}

static int router_override_press(const struct device *dev, const struct device *pipeline_device,
                                 uint32_t position, uint32_t now_ms) {
    const struct zpt_router_config *config = dev->config;
    struct zpt_router_data *data = dev->data;
    size_t pipeline_index;
    int ret = pipeline_device_index(config, pipeline_device, &pipeline_index);
    if (ret < 0) {
        return ret;
    }
    ret = k_mutex_lock(&data->route_lock, K_FOREVER);
    if (ret < 0) {
        return ret;
    }

    struct zpt_explicit_route_override *available = NULL;
    for (size_t index = 0; index < ARRAY_SIZE(data->overrides); index++) {
        struct zpt_explicit_route_override *candidate = &data->overrides[index];
        if (candidate->active && candidate->position == position) {
            available = candidate;
            break;
        }
        if (!candidate->active && available == NULL) {
            available = candidate;
        }
    }
    if (available == NULL) {
        return unlock_route(data, -ENOSPC);
    }
    *available = (struct zpt_explicit_route_override){
        .position = position,
        .pipeline_index = pipeline_index,
        .order = ++data->next_override_order,
        .active = true,
    };
    return unlock_route(data, select_desired_route_locked(dev, now_ms));
}

static int router_override_release(const struct device *dev, uint32_t position, uint32_t now_ms) {
    struct zpt_router_data *data = dev->data;
    int ret = k_mutex_lock(&data->route_lock, K_FOREVER);
    if (ret < 0) {
        return ret;
    }
    for (size_t index = 0; index < ARRAY_SIZE(data->overrides); index++) {
        if (data->overrides[index].active && data->overrides[index].position == position) {
            data->overrides[index].active = false;
            return unlock_route(data, select_desired_route_locked(dev, now_ms));
        }
    }
    return unlock_route(data, 0);
}

static int router_push(const struct device *dev, const struct zpt_signal *signal,
                       struct zpt_pipeline_result *result) {
    struct zpt_router_data *data = dev->data;
    return zpt_zmk_router_executor_push(&data->executor, signal, result);
}

static DEVICE_API(zpt_router, router_api) = {
    .push = router_push,
    .refresh_layer_route = router_refresh_layer_route,
    .override_press = router_override_press,
    .override_release = router_override_release,
};

static int validate_router_device(const struct device *dev) {
    if (dev == NULL) {
        return -EINVAL;
    }
    if (!device_is_ready(dev)) {
        return -ENODEV;
    }
    return DEVICE_API_IS(zpt_router, dev) ? 0 : -EPROTOTYPE;
}

int zpt_zmk_router_push(const struct device *dev, const struct zpt_signal *signal,
                        struct zpt_pipeline_result *result) {
    if (signal == NULL || result == NULL) {
        return -EINVAL;
    }
    int ret = validate_router_device(dev);
    if (ret < 0) {
        return ret;
    }
    const struct zpt_router_driver_api *api = DEVICE_API_GET(zpt_router, dev);
    return api->push == NULL ? -ENOSYS : api->push(dev, signal, result);
}

int zpt_zmk_router_refresh_layer_route(const struct device *dev, uint32_t now_ms) {
    int ret = validate_router_device(dev);
    if (ret < 0) {
        return ret;
    }
    const struct zpt_router_driver_api *api = DEVICE_API_GET(zpt_router, dev);
    return api->refresh_layer_route == NULL ? -ENOSYS : api->refresh_layer_route(dev, now_ms);
}

int zpt_zmk_router_override_press(const struct device *dev, const struct device *pipeline_device,
                                  uint32_t position, uint32_t now_ms) {
    if (pipeline_device == NULL) {
        return -EINVAL;
    }
    int ret = validate_router_device(dev);
    if (ret < 0) {
        return ret;
    }
    const struct zpt_router_driver_api *api = DEVICE_API_GET(zpt_router, dev);
    return api->override_press == NULL
               ? -ENOSYS
               : api->override_press(dev, pipeline_device, position, now_ms);
}

int zpt_zmk_router_override_release(const struct device *dev, uint32_t position, uint32_t now_ms) {
    int ret = validate_router_device(dev);
    if (ret < 0) {
        return ret;
    }
    const struct zpt_router_driver_api *api = DEVICE_API_GET(zpt_router, dev);
    return api->override_release == NULL ? -ENOSYS : api->override_release(dev, position, now_ms);
}

static int router_init(const struct device *dev) {
    const struct zpt_router_config *config = dev->config;
    struct zpt_router_data *data = dev->data;
    k_mutex_init(&data->route_lock);

    for (size_t index = 0; index < config->pipeline_count; index++) {
        int ret = zpt_zmk_pipeline_provider_prepare(config->pipeline_devices[index], dev,
                                                    &data->pipelines[index]);
        if (ret < 0) {
            LOG_ERR("Router %s failed to prepare pipeline %u: %d", config->stable_id,
                    (unsigned int)index, ret);
            return ret;
        }
    }

    size_t default_pipeline_index;
    int ret =
        pipeline_device_index(config, config->default_pipeline_device, &default_pipeline_index);
    if (ret < 0) {
        LOG_ERR("Router %s default pipeline is not in pipelines", config->stable_id);
        return ret;
    }
    for (size_t index = 0; index < config->layer_route_count; index++) {
        for (size_t layer_index = 0; layer_index < config->layer_routes[index].layer_count;
             layer_index++) {
            if (config->layer_routes[index].layers[layer_index] >= ZMK_KEYMAP_LAYERS_LEN) {
                LOG_ERR("Router %s route %u has invalid layer %u", config->stable_id,
                        (unsigned int)index, config->layer_routes[index].layers[layer_index]);
                return -EINVAL;
            }
        }
        ret = pipeline_device_index(config, config->layer_routes[index].pipeline_device,
                                    &data->layer_route_pipeline_indices[index]);
        if (ret < 0) {
            LOG_ERR("Router %s layer route %u pipeline is not in pipelines", config->stable_id,
                    (unsigned int)index);
            return ret;
        }
    }

    data->router = (struct zpt_router){
        .stable_id = config->stable_id,
        .input_kind = ZPT_SIGNAL_RAW_MOTION,
        .pipelines = data->pipelines,
        .pipeline_count = config->pipeline_count,
        .default_pipeline_index = default_pipeline_index,
    };
    ret = zpt_zmk_router_executor_init(&data->executor, &data->router);
    if (ret < 0) {
        LOG_ERR("Failed to validate motion router %s: %d", config->stable_id, ret);
        return ret;
    }
    ret = zpt_zmk_router_executor_activate(&data->executor, ZPT_RESET_PIPELINE_ENTERED);
    if (ret < 0) {
        LOG_ERR("Failed to activate motion router %s: %d", config->stable_id, ret);
        return ret;
    }

    size_t initial_pipeline;
    ret = selected_layer_pipeline(dev, &initial_pipeline);
    if (ret < 0 || initial_pipeline == default_pipeline_index) {
        return ret;
    }
    struct zpt_pipeline_result result;
    ret = zpt_zmk_router_executor_select(&data->executor, initial_pipeline, k_uptime_get_32(),
                                         &result);
    if (ret < 0) {
        LOG_ERR("Failed to select initial route for %s: %d", config->stable_id, ret);
    }
    return ret;
}

static int layer_state_changed_listener(const zmk_event_t *event) {
    const struct zmk_layer_state_changed *layer_event = as_zmk_layer_state_changed(event);
    if (layer_event == NULL) {
        return ZMK_EV_EVENT_BUBBLE;
    }

#define ZPT_ROUTER_REFRESH_INSTANCE(inst)                                                          \
    do {                                                                                           \
        const struct device *dev = DEVICE_DT_INST_GET(inst);                                       \
        if (device_is_ready(dev)) {                                                                \
            int ret = router_refresh_layer_route(dev, (uint32_t)layer_event->timestamp);           \
            if (ret < 0) {                                                                         \
                LOG_ERR("Failed to refresh motion router route: %d", ret);                         \
            }                                                                                      \
        }                                                                                          \
    } while (false);

    DT_INST_FOREACH_STATUS_OKAY(ZPT_ROUTER_REFRESH_INSTANCE)
#undef ZPT_ROUTER_REFRESH_INSTANCE
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(zpt_pointing_router, layer_state_changed_listener);
ZMK_SUBSCRIPTION(zpt_pointing_router, zmk_layer_state_changed);

#define ZPT_ROUTER_PIPELINE_DEVICE(node_id, prop, index)                                           \
    DEVICE_DT_GET(DT_PHANDLE_BY_IDX(node_id, prop, index))

#define ZPT_ROUTER_DECLARE_ROUTE(node_id)                                                          \
    static const uint8_t zpt_router_route_layers_##node_id[] = DT_PROP(node_id, layers);

#define ZPT_ROUTER_ROUTE_CONFIG(node_id)                                                           \
    {                                                                                              \
        .pipeline_device = DEVICE_DT_GET(DT_PHANDLE(node_id, pipeline)),                           \
        .layers = zpt_router_route_layers_##node_id,                                               \
        .layer_count = ARRAY_SIZE(zpt_router_route_layers_##node_id),                              \
    },

#define ZPT_ROUTER_DEFINE(inst)                                                                    \
    BUILD_ASSERT(DT_INST_PROP_LEN(inst, pipelines) > 0, "router requires at least one pipeline");  \
    DT_INST_FOREACH_CHILD_STATUS_OKAY(inst, ZPT_ROUTER_DECLARE_ROUTE)                              \
    static const struct device *const zpt_router_pipeline_devices_##inst[] = {                     \
        DT_INST_FOREACH_PROP_ELEM_SEP(inst, pipelines, ZPT_ROUTER_PIPELINE_DEVICE, (, ))};         \
    static const struct zpt_layer_route_config zpt_router_layer_routes_##inst[] = {                \
        DT_INST_FOREACH_CHILD_STATUS_OKAY(inst, ZPT_ROUTER_ROUTE_CONFIG){0}};                      \
    static struct zpt_pipeline *zpt_router_pipelines_##inst[DT_INST_PROP_LEN(inst, pipelines)];    \
    static size_t                                                                                  \
        zpt_router_route_pipeline_indices_##inst[MAX(1, DT_INST_CHILD_NUM_STATUS_OKAY(inst))];     \
    static struct zpt_router_data zpt_router_data_##inst = {                                       \
        .pipelines = zpt_router_pipelines_##inst,                                                  \
        .layer_route_pipeline_indices = zpt_router_route_pipeline_indices_##inst,                  \
    };                                                                                             \
    static const struct zpt_router_config zpt_router_config_##inst = {                             \
        .stable_id = DT_INST_PROP(inst, stable_id),                                                \
        .pipeline_devices = zpt_router_pipeline_devices_##inst,                                    \
        .pipeline_count = ARRAY_SIZE(zpt_router_pipeline_devices_##inst),                          \
        .default_pipeline_device = DEVICE_DT_GET(DT_INST_PHANDLE(inst, default_pipeline)),         \
        .layer_routes = zpt_router_layer_routes_##inst,                                            \
        .layer_route_count = DT_INST_CHILD_NUM_STATUS_OKAY(inst),                                  \
    };                                                                                             \
    DEVICE_DT_INST_DEFINE(inst, router_init, NULL, &zpt_router_data_##inst,                        \
                          &zpt_router_config_##inst, POST_KERNEL,                                  \
                          CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &router_api);

DT_INST_FOREACH_STATUS_OKAY(ZPT_ROUTER_DEFINE)
