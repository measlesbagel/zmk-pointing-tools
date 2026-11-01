/* SPDX-License-Identifier: MIT */
#pragma once

#include <stdint.h>

#include <zmk/pointing_tools/core/pipeline.h>

/*
 * Input capture service (CONFIG_ZMK_POINTING_TOOLS_INPUT_CAPTURE).
 *
 * The telemetry service records the input of one registered pipeline while a
 * host runs a recording, and streams it over the host protocol
 * (docs/protocol.md, "Input capture"). With no recording running, each frame
 * costs one atomic read. Key presses come from the pipeline's own keypress
 * guard, through the capture tap.
 */

/* Make a pipeline recordable, in registration order. Call during init,
 * while no frame runs (it installs the pipeline's observer); returns -ENOSPC
 * past
 * CONFIG_ZMK_POINTING_TOOLS_INPUT_CAPTURE_MAX_PIPELINES. */
int zpt_capture_register_pipeline(struct zpt_pipeline *pipeline);

/* A tuning change landed. Call while no frame runs. */
void zpt_capture_settings_changed(void);

/* Supplied by the platform: run fn(arg) while no pipeline runs a frame. */
int zpt_capture_between_frames(void (*fn)(void *arg), void *arg);
