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
