/* SPDX-License-Identifier: MIT */

import { STATE, TARGET_KIND, TUNING } from "./protocol.js";

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

/* A state-status payload shaped like real firmware: twelve observed stage
 * targets with their stable identities. */
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

function parameter(id, key, devicetreeProperty, minimum, maximum, compiled, label, unit, description) {
  return {
    id, type: TUNING.INTEGER, minimum, maximum, step: 1, compiled, current: compiled,
    label, unit, description, key, devicetreeProperty,
  };
}

/* The smoke keymap's cursor acceleration and coalescer as fully discovered
 * tuning targets, mirroring the firmware parameter tables, so the curve
 * editor can be tried without a keyboard. */
export function buildSimulatorTuningTargets() {
  const speeds = [0, 8, 20, 45, 90, 160, 260, 400];
  const percents = [65, 66, 73, 99, 159, 225, 264, 283];
  return [
    {
      id: 12,
      kind: TARGET_KIND.PIPELINE_STAGE,
      label: "cursor-acceleration",
      stableId: "cursor-acceleration",
      devicetreePath: "/zpt_test_cursor_acceleration",
      parameters: [
        ...speeds.map((speed, i) => parameter(i, `curve-speed-${i + 1}`, "curve-speeds-mm-per-s", 0, 10000,
          speed, `Point ${i + 1} speed`, "mm/s",
          `Ball speed of curve point ${i + 1}. Point speeds must increase from point 1 to 8.`)),
        ...percents.map((percent, i) => parameter(8 + i, `curve-sensitivity-${i + 1}`,
          "curve-sensitivity-percent", 1, 1000, percent, `Point ${i + 1} sensitivity`, "%",
          `Cursor sensitivity at curve point ${i + 1}, on top of the base gain.`)),
        parameter(16, "speed-rise", "speed-rise-ms", 1, 200, 10, "Speed rise time", "ms",
          "How quickly the speed estimate follows the ball speeding up."),
        parameter(17, "speed-fall", "speed-fall-ms", 1, 200, 5, "Speed fall time", "ms",
          "How quickly the speed estimate follows the ball slowing down."),
      ],
    },
    {
      id: 13,
      kind: TARGET_KIND.PIPELINE_STAGE,
      label: "cursor-coalescer",
      stableId: "cursor-coalescer",
      devicetreePath: "/zpt_test_cursor_coalescer",
      parameters: [
        parameter(0, "usb-report-interval", "usb-report-interval-ms", 0, 1000, 1, "USB report interval", "ms",
          "Shortest time between cursor reports over USB."),
        parameter(1, "ble-report-interval", "ble-report-interval-ms", 0, 1000, 8, "Bluetooth report interval", "ms",
          "Shortest time between cursor reports over Bluetooth."),
      ],
    },
  ];
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
