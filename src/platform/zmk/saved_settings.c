/* SPDX-License-Identifier: MIT */

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/util.h>

#include <zmk/pointing_tools/platform/zmk/saved_settings.h>
#include <zmk/pointing_tools/service/tuning_store.h>

#if IS_ENABLED(CONFIG_ZMK_POINTING_TOOLS_DEVICE_TABLE)
#include <zmk/pointing_tools/platform/zmk/device_table.h>
#include <zmk/pointing_tools/source/device_caps.h>
#endif

/* Peripheral-owned devices get their saved CPI through the split tunnel. */
#define ZPT_SAVED_REMOTE_DEVICES                                                                   \
    (IS_ENABLED(CONFIG_ZMK_POINTING_TOOLS_DEVICE_TABLE) &&                                         \
     IS_ENABLED(CONFIG_ZMK_POINTING_TOOLS_SENSOR_TUNNEL) &&                                        \
     IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL))

#if ZPT_SAVED_REMOTE_DEVICES
#include <zmk/event_manager.h>
#include <zmk/events/split_peripheral_status_changed.h>
#include <zmk/pointing_tools/platform/zmk/sensor_control_proxy.h>
#endif

LOG_MODULE_REGISTER(zpt_saved, CONFIG_ZMK_LOG_LEVEL);

#define ZPT_SAVED_ROOT "zpt"
#define ZPT_SAVED_TUNING_PREFIX "t/"
#define ZPT_SAVED_DEVICE_PREFIX "d/"
/* Room for "zpt/t/" plus the longest tuning value name. */
#define ZPT_SAVED_KEY_MAX                                                                          \
    (sizeof(ZPT_SAVED_ROOT "/" ZPT_SAVED_TUNING_PREFIX) + ZPT_TUNING_STORE_NAME_MAX)

BUILD_ASSERT(ZPT_SAVED_KEY_MAX <= SETTINGS_MAX_NAME_LEN + 1,
             "saved tuning names must fit Zephyr's settings name limit");

/* --- Tuning values ---------------------------------------------------- */

static int zpt_saved_tuning_write(void *context, const char *name, int32_t value) {
    ARG_UNUSED(context);
    char key[ZPT_SAVED_KEY_MAX];
    (void)snprintf(key, sizeof(key), ZPT_SAVED_ROOT "/" ZPT_SAVED_TUNING_PREFIX "%s", name);
    return settings_save_one(key, &value, sizeof(value));
}

static int zpt_saved_tuning_erase(void *context, const char *name) {
    ARG_UNUSED(context);
    char key[ZPT_SAVED_KEY_MAX];
    (void)snprintf(key, sizeof(key), ZPT_SAVED_ROOT "/" ZPT_SAVED_TUNING_PREFIX "%s", name);
    return settings_delete(key);
}

static const struct zpt_tuning_store_backend zpt_saved_backend = {
    .write = zpt_saved_tuning_write,
    .erase = zpt_saved_tuning_erase,
};

/* --- Pointing-device CPI ---------------------------------------------- */

#if IS_ENABLED(CONFIG_ZMK_POINTING_TOOLS_DEVICE_TABLE)

#define ZPT_SAVED_DEVICE_SLOTS MAX(DT_NUM_INST_STATUS_OKAY(measlesbagel_zpt_pointing_device), 1)

/* 0 means no saved value. Written by the settings loader at boot and the
 * telemetry thread afterwards; read by the reconnect worker. */
static uint16_t zpt_saved_cpi[ZPT_SAVED_DEVICE_SLOTS];

static int zpt_saved_device_key(char *key, size_t size, const struct zpt_pointing_device *device) {
    const int length =
        snprintf(key, size, ZPT_SAVED_ROOT "/" ZPT_SAVED_DEVICE_PREFIX "%s", device->stable_id);
    return length < 0 || (size_t)length >= size ? -ENAMETOOLONG : 0;
}

static int zpt_saved_device_apply(const struct zpt_pointing_device *device, uint16_t cpi) {
#if ZPT_SAVED_REMOTE_DEVICES
    if (device->location != 0) {
        return zpt_sensor_control_preview_remote(device, cpi, NULL);
    }
#endif
    if (device->location != 0) {
        return -ENOSYS;
    }
    uint16_t effective;
    return cpi == device->default_cpi ? zpt_device_control_reset(device)
                                      : zpt_device_control_preview(device, cpi, &effective);
}

uint16_t zpt_saved_device_cpi(uint8_t device_id) {
    return device_id < ARRAY_SIZE(zpt_saved_cpi) ? zpt_saved_cpi[device_id] : 0;
}

static int zpt_saved_device_save_one(uint8_t device_id) {
    const struct zpt_pointing_device *device = zpt_device_table_at(device_id);
    if (device == NULL || device_id >= ARRAY_SIZE(zpt_saved_cpi)) {
        return -ENODEV;
    }
    if (!device->caps.settable) {
        return -ENOSYS;
    }
    char key[SETTINGS_MAX_NAME_LEN + 1];
    int ret = zpt_saved_device_key(key, sizeof(key), device);
    if (ret < 0) {
        return ret;
    }
    const uint16_t current = zpt_device_current_cpi(device);
    if (current == device->default_cpi) {
        if (zpt_saved_cpi[device_id] == 0) {
            return 0;
        }
        ret = settings_delete(key);
        if (ret == 0) {
            zpt_saved_cpi[device_id] = 0;
        }
        return ret;
    }
    if (zpt_saved_cpi[device_id] == current) {
        return 0;
    }
    ret = settings_save_one(key, &current, sizeof(current));
    if (ret == 0) {
        zpt_saved_cpi[device_id] = current;
    }
    return ret;
}

static int zpt_saved_device_clear_one(uint8_t device_id) {
    const struct zpt_pointing_device *device = zpt_device_table_at(device_id);
    if (device == NULL || device_id >= ARRAY_SIZE(zpt_saved_cpi)) {
        return -ENODEV;
    }
    if (!device->caps.settable) {
        return -ENOSYS;
    }
    if (zpt_saved_cpi[device_id] != 0) {
        char key[SETTINGS_MAX_NAME_LEN + 1];
        int ret = zpt_saved_device_key(key, sizeof(key), device);
        if (ret == 0) {
            ret = settings_delete(key);
        }
        if (ret < 0) {
            return ret;
        }
        zpt_saved_cpi[device_id] = 0;
    }
    return zpt_saved_device_apply(device, device->default_cpi);
}

static int zpt_saved_for_devices(uint8_t device_id, int (*action)(uint8_t device_id)) {
    if (device_id != ZPT_SAVED_ALL_DEVICES) {
        return action(device_id);
    }
    int result = 0;
    for (size_t i = 0; i < zpt_device_table_count() && i < ZPT_SAVED_ALL_DEVICES; i++) {
        const struct zpt_pointing_device *device = zpt_device_table_at(i);
        if (!device->caps.settable) {
            continue;
        }
        const int ret = action((uint8_t)i);
        if (ret < 0 && result == 0) {
            result = ret;
        }
    }
    return result;
}

int zpt_saved_device_save(uint8_t device_id) {
    return zpt_saved_for_devices(device_id, zpt_saved_device_save_one);
}

int zpt_saved_device_clear(uint8_t device_id) {
    return zpt_saved_for_devices(device_id, zpt_saved_device_clear_one);
}

static int zpt_saved_device_load(const char *stable_id, uint16_t cpi) {
    const struct zpt_pointing_device *device = zpt_device_table_find(stable_id);
    if (device == NULL || device->id >= ARRAY_SIZE(zpt_saved_cpi) || !device->caps.settable) {
        return -ENOENT;
    }
    uint16_t snapped = cpi;
    if (cpi == 0 || zpt_cpi_validate(&device->caps, cpi, &snapped) < 0 || snapped != cpi) {
        /* No longer a supported value: run at compiled. */
        return -ERANGE;
    }
    zpt_saved_cpi[device->id] = cpi;
    return 0;
}

#else

uint16_t zpt_saved_device_cpi(uint8_t device_id) {
    ARG_UNUSED(device_id);
    return 0;
}

int zpt_saved_device_save(uint8_t device_id) {
    ARG_UNUSED(device_id);
    return -ENODEV;
}

int zpt_saved_device_clear(uint8_t device_id) {
    ARG_UNUSED(device_id);
    return -ENODEV;
}

#endif /* CONFIG_ZMK_POINTING_TOOLS_DEVICE_TABLE */

/* --- Re-sending peripheral CPI ---------------------------------------- */

#if ZPT_SAVED_REMOTE_DEVICES

/* Give the new connection time to finish discovery before relaying. */
#define ZPT_SAVED_RESEND_DELAY K_MSEC(1500)

/* Relays block for up to the tunnel timeout per device, too long for the
 * system work queue, so the re-send runs on its own small queue. */
K_THREAD_STACK_DEFINE(zpt_saved_stack, 1024);
static struct k_work_q zpt_saved_queue;

static void zpt_saved_resend(struct k_work *work) {
    ARG_UNUSED(work);
    for (size_t i = 0; i < zpt_device_table_count() && i < ARRAY_SIZE(zpt_saved_cpi); i++) {
        const struct zpt_pointing_device *device = zpt_device_table_at(i);
        const uint16_t cpi = zpt_saved_cpi[i];
        if (device->location == 0 || cpi == 0) {
            continue;
        }
        /* A half that isn't connected yet times out here and gets the value
         * on its own connect. */
        const int ret = zpt_sensor_control_preview_remote(device, cpi, NULL);
        if (ret < 0) {
            LOG_DBG("Saved CPI for %s not delivered: %d", device->stable_id, ret);
        }
    }
}

static K_WORK_DELAYABLE_DEFINE(zpt_saved_resend_work, zpt_saved_resend);

static int zpt_saved_peripheral_listener(const zmk_event_t *event) {
    const struct zmk_split_peripheral_status_changed *status =
        as_zmk_split_peripheral_status_changed(event);
    if (status != NULL && status->connected) {
        (void)k_work_reschedule_for_queue(&zpt_saved_queue, &zpt_saved_resend_work,
                                          ZPT_SAVED_RESEND_DELAY);
    }
    return ZMK_EV_EVENT_BUBBLE;
}

/* External linkage is intrinsic to ZMK's listener/subscription macros. */
// NOLINTNEXTLINE(misc-use-internal-linkage)
ZMK_LISTENER(zpt_saved_settings, zpt_saved_peripheral_listener);
// NOLINTNEXTLINE(misc-use-internal-linkage)
ZMK_SUBSCRIPTION(zpt_saved_settings, zmk_split_peripheral_status_changed);

#endif /* ZPT_SAVED_REMOTE_DEVICES */

/* --- Settings handler ------------------------------------------------- */

static int zpt_saved_read(settings_read_cb read_cb, void *cb_arg, size_t len, void *value,
                          size_t size) {
    if (len != size) {
        return -EINVAL;
    }
    const ssize_t read = read_cb(cb_arg, value, size);
    return read == (ssize_t)size ? 0 : -EIO;
}

static int zpt_saved_set(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg) {
    int ret = -ENOENT;
    if (strncmp(name, ZPT_SAVED_TUNING_PREFIX, 2) == 0) {
        int32_t value;
        ret = zpt_saved_read(read_cb, cb_arg, len, &value, sizeof(value));
        if (ret == 0) {
            ret = zpt_tuning_store_load(name + 2, value);
        }
    }
#if IS_ENABLED(CONFIG_ZMK_POINTING_TOOLS_DEVICE_TABLE)
    else if (strncmp(name, ZPT_SAVED_DEVICE_PREFIX, 2) == 0) {
        uint16_t cpi;
        ret = zpt_saved_read(read_cb, cb_arg, len, &cpi, sizeof(cpi));
        if (ret == 0) {
            ret = zpt_saved_device_load(name + 2, cpi);
        }
    }
#endif
    if (ret < 0) {
        /* Left in flash, so a firmware that knows the name again finds it. */
        LOG_WRN("Ignoring saved setting %s: %d", name, ret);
    }
    /* Never fail the whole load over one stale value. */
    return 0;
}

static int zpt_saved_commit(void) {
    const size_t fallbacks = zpt_tuning_store_apply_loaded();
    if (fallbacks > 0) {
        LOG_WRN("%u stages fell back to compiled values", (unsigned int)fallbacks);
    }
#if IS_ENABLED(CONFIG_ZMK_POINTING_TOOLS_DEVICE_TABLE)
    for (size_t i = 0; i < zpt_device_table_count() && i < ARRAY_SIZE(zpt_saved_cpi); i++) {
        const struct zpt_pointing_device *device = zpt_device_table_at(i);
        if (device->location == 0 && zpt_saved_cpi[i] != 0) {
            uint16_t effective;
            const int ret = zpt_device_control_preview(device, zpt_saved_cpi[i], &effective);
            if (ret < 0) {
                LOG_WRN("Saved CPI for %s not applied: %d", device->stable_id, ret);
                zpt_saved_cpi[i] = 0;
            }
        }
    }
#endif
    return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(zpt_saved, ZPT_SAVED_ROOT, NULL, zpt_saved_set, zpt_saved_commit,
                               NULL);

static int zpt_saved_init(void) {
    zpt_tuning_store_init(&zpt_saved_backend);
#if ZPT_SAVED_REMOTE_DEVICES
    k_work_queue_start(&zpt_saved_queue, zpt_saved_stack, K_THREAD_STACK_SIZEOF(zpt_saved_stack),
                       K_LOWEST_APPLICATION_THREAD_PRIO, NULL);
#endif
    return 0;
}

/* Before main() loads settings, after every tuning target registered. */
SYS_INIT(zpt_saved_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
