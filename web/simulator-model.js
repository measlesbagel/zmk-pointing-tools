/* SPDX-License-Identifier: MIT */

import { STATE, TARGET_KIND } from "./protocol.js";

/* The smoke keymap's pipeline-telemetry stage targets, in registration
 * order: cursor pipeline (normalize, gate, transfer, quantizer), scroll
 * pipeline (normalize, intent, constraint, batcher), text pipeline
 * (normalize, intent, constraint, text navigation). */
export const SIMULATED_STAGES = [
  { id: 0, label: "resolution-normalize" },
  { id: 1, label: "motion-gate" },
  { id: 2, label: "cursor-transfer" },
  { id: 3, label: "cursor-quantizer" },
  { id: 4, label: "scroll-resolution-normalize" },
  { id: 5, label: "scroll-axis-intent" },
  { id: 6, label: "scroll-constraint" },
  { id: 7, label: "scroll-batcher" },
  { id: 8, label: "text-resolution-normalize" },
  { id: 9, label: "text-axis-intent" },
  { id: 10, label: "text-constraint" },
  { id: 11, label: "text-nav" },
];

/* A state-status payload shaped like real firmware: no tuning targets,
 * ten observed stage targets with their stable identities. */
export function buildSimulatorState() {
  return {
    schemaVersion: 2,
    dropped: 0,
    queueCapacity: 64,
    levels: new Map(SIMULATED_STAGES.map(({ id }) => [id, STATE.OFF])),
    labels: new Map(SIMULATED_STAGES.map(({ id, label }) => [id, label])),
  };
}

/* A state sample exactly as firmware's stage observer would frame it. */
export function buildSimulatorEvent(targetId, event, flags, timestamp, sequence, values) {
  return {
    targetId,
    targetKind: TARGET_KIND.PIPELINE_STAGE,
    event,
    intent: 0,
    flags,
    timestamp,
    sequence,
    values,
  };
}

const SIMULATED_GESTURE_TICKS = 150;
const SIMULATED_GESTURE_FRAMES = 30;

/* Every-frame values for a normalize stage: a 30-frame gesture every 1.2 s
 * at 800 CPI, about 30 mm long and varying a few percent between gestures,
 * vertical for scroll and horizontal otherwise. Other stages and the pauses
 * between gestures produce nothing. */
export function buildSimulatorFrame(label, tick) {
  if (!label.includes("normalize") || tick % SIMULATED_GESTURE_TICKS >= SIMULATED_GESTURE_FRAMES) {
    return undefined;
  }
  const gesture = Math.floor(tick / SIMULATED_GESTURE_TICKS);
  const step = Math.round((1 + 0.06 * Math.sin(gesture * 1.7)) * 65536);
  const vertical = label.includes("scroll");
  return [128, 2, vertical ? 0 : step, vertical ? step : 0, 800, 8000, tick & 0xffff, 0, 0, 0];
}
