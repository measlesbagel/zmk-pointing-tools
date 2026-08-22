/* SPDX-License-Identifier: MIT */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Wire helpers for the split sensor-control channel (see
 * docs/device-cpi-control.md). Requests travel to the owning half through
 * ZMK's INVOKE_BEHAVIOR command:
 *
 *   param1: seq:u4<<16 | opcode:u8<<8 | device-id:u8
 *   param2: requested cpi:u16 (SET_CPI), otherwise zero
 *
 * Responses ride back as one 32-bit relative input event value on a
 * reserved code, packed as:
 *
 *   31..28 tag 0xA | 27..20 status errno | 19..16 seq | 15..0 value:u16
 *
 * The tag plus exact-sequence match make accidental acceptance of foreign
 * events effectively impossible; mismatches are dropped as garbage.
 */

#define ZPT_SCW_TAG 0xAu

#define ZPT_SCW_OPCODE_GET_CPI 1u
#define ZPT_SCW_OPCODE_SET_CPI 2u

static inline uint32_t zpt_scw_param1(uint8_t seq, uint8_t opcode, uint8_t device_id) {
    return ((uint32_t)(seq & 0x0Fu) << 16) | ((uint32_t)(opcode & 0xFFu) << 8) | device_id;
}

static inline uint8_t zpt_scw_param_seq(uint32_t param1) {
    return (uint8_t)((param1 >> 16) & 0x0Fu);
}

static inline uint8_t zpt_scw_param_opcode(uint32_t param1) {
    return (uint8_t)((param1 >> 8) & 0xFFu);
}

static inline uint8_t zpt_scw_param_device_id(uint32_t param1) { return (uint8_t)(param1 & 0xFFu); }

static inline uint32_t zpt_scw_encode(uint8_t status_errno, uint8_t seq, uint16_t value) {
    return ((uint32_t)ZPT_SCW_TAG << 28) | ((uint32_t)status_errno << 20) |
           ((uint32_t)(seq & 0x0Fu) << 16) | value;
}

static inline bool zpt_scw_decode(uint32_t raw, uint8_t *status_errno, uint8_t *seq,
                                  uint16_t *value) {
    if (((raw >> 28) & 0xFu) != ZPT_SCW_TAG) {
        return false;
    }
    *status_errno = (uint8_t)((raw >> 20) & 0xFFu);
    *seq = (uint8_t)((raw >> 16) & 0xFu);
    *value = (uint16_t)(raw & 0xFFFFu);
    return true;
}
