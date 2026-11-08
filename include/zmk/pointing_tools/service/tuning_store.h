/* SPDX-License-Identifier: MIT */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Saved tuning values, independent of ZMK.
 *
 * Each value has three states: compiled (devicetree), saved (on the
 * keyboard), and previewing (live in RAM, not saved). The store keeps the
 * saved set in RAM and writes it through a backend only on an explicit
 * save or clear, never on a preview.
 *
 * Saved values are named "<stable-id>/<parameter-key>", so a firmware
 * update that renames or removes a stage drops its values instead of
 * misapplying them. A loaded set that the target rejects as a whole falls
 * back to compiled values.
 *
 * Callers are externally synchronized, like the tuning registry. */

/* Every target, as in the host protocol. */
#define ZPT_TUNING_STORE_ALL_TARGETS UINT8_MAX

/* Longest value name, without a terminator. Keeps the backend's full key,
 * with its own prefix, inside Zephyr's 64-byte settings name limit. */
#define ZPT_TUNING_STORE_NAME_MAX 48

struct zpt_tuning_store_backend {
    int (*write)(void *context, const char *name, int32_t value);
    int (*erase)(void *context, const char *name);
    void *context;
};

/* Forget every saved value and use backend for later writes. NULL backend
 * keeps values in RAM only, which host tests use. */
void zpt_tuning_store_init(const struct zpt_tuning_store_backend *backend);

/* Format a value's name into buffer. Returns its length, or -ENAMETOOLONG. */
int zpt_tuning_store_name(char *buffer, size_t size, const char *stable_id, const char *key);

/* Record one value read back from storage at boot. Returns -ENOENT for a
 * name no registered target and parameter matches, -ENOMEM when the store
 * is full, and -EINVAL for a malformed name. */
int zpt_tuning_store_load(const char *name, int32_t value);

/* Apply every loaded value, one atomic batch per target. A target that
 * rejects its batch keeps compiled values and loses its saved set. Returns
 * the number of targets that fell back. */
size_t zpt_tuning_store_apply_loaded(void);

/* The saved value of one parameter, or -ENOENT when it has none. */
int zpt_tuning_store_saved(uint8_t target_id, uint8_t parameter_id, int32_t *value);

/* Save a target's current values: those that differ from compiled are
 * written, and saved values equal to compiled are erased. */
int zpt_tuning_store_save(uint8_t target_id);

/* Erase a target's saved values and reset it to compiled. */
int zpt_tuning_store_clear(uint8_t target_id);

/* Discard previews: reset a target to compiled, then reapply its saved
 * values. */
int zpt_tuning_store_restore(uint8_t target_id);

/* Number of saved values across every target. */
size_t zpt_tuning_store_count(void);
