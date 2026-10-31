/* SPDX-License-Identifier: MIT */

/*
 * Quality metrics for the trace benchmark. Each metric is defined in
 * docs/trace-benchmark.md; keep this file and that document in step.
 */

import { eventTimesUs } from "./fixture.js";
import { parseLines } from "./metrics.js";

/* Metric names in report order, and whether a higher value is better. */
export const METRICS = {
  speedErrorPercent: { higherIsBetter: false, unit: "%" },
  slowJitterPercent: { higherIsBetter: false, unit: "%" },
  sensitivityStepPercent: { higherIsBetter: false, unit: "%" },
  flickLagMs: { higherIsBetter: false, unit: "ms" },
  distanceErrorPercent: { higherIsBetter: false, unit: "%" },
  unintendedOutput: { higherIsBetter: false, unit: "" },
  axisLockPercent: { higherIsBetter: true, unit: "%" },
  sidewaysLeakPercent: { higherIsBetter: false, unit: "%" },
  axisSwitches: { higherIsBetter: false, unit: "" },
};

const SETTLE_US = 100000;
const TAIL_US = 100000;
const JITTER_BIN_US = 16000;
const JITTER_HALF_WINDOW = 3;
const SLOW_SPEED_MM_PER_S = 50;
const LAG_PROGRESS = [0.1, 0.5, 0.9];
const INTENT_UNDECIDED = 0;
const INTENT_AXIS = { horizontal: 2, vertical: 3 };
const TEXT_DIRECTIONS = [[-1, 0], [1, 0], [0, -1], [0, 1]];

const mean = (values) => values.reduce((sum, value) => sum + value, 0) / values.length;
const rms = (values) => Math.sqrt(mean(values.map((value) => value * value)));

/* Frames and reports of a runner output, timed in microseconds. Frames
 * keep the fixture's input counts; cursor frames add the acceleration
 * stage's speed estimate, scroll and text frames the axis intent. */
export function parseTrace(fixture, output) {
  const times = eventTimesUs(fixture);
  const motions = fixture.events
    .map((event, index) => ({ event, timeUs: times[index] }))
    .filter(({ event }) => event[0] === "motion");
  const kind = fixture.processor.kind;
  const shape = kind === "cursor-pipeline" ? [9, 4] : kind === "composed-scroll" ? [12, 3] : [5, 2];
  const { decisions, outputs } = parseLines(output, fixture, ...shape);
  if (decisions.length !== motions.length) {
    throw new Error(`${fixture.id}: ${decisions.length} decisions for ${motions.length} frames`);
  }
  const frames = decisions.map((decision, index) => ({
    tUs: motions[index].timeUs,
    x: motions[index].event[2],
    y: motions[index].event[3],
    ...(kind === "cursor-pipeline"
      ? { speed: decision[7] < 0 ? null : decision[7] / 65536,
        gain: decision[8] < 0 ? null : decision[8] / 65536 }
      : { intent: decision[1] }),
  }));
  const reports = outputs.map((report) => {
    if (kind === "cursor-pipeline") return { tUs: report[3], x: report[1], y: report[2] };
    if (kind === "composed-scroll") return { tUs: report[0] * 1000, x: report[1], y: report[2] };
    const [x, y] = TEXT_DIRECTIONS[report[1]];
    return { tUs: report[0] * 1000, x, y };
  });
  return { frames, reports };
}

function sum(items) {
  return items.reduce(([x, y], item) => [x + item.x, y + item.y], [0, 0]);
}

/* The time progress along the net direction first reaches each fraction. */
function progressTimes(items) {
  const [nx, ny] = sum(items);
  const total = Math.hypot(nx, ny);
  if (total === 0) return null;
  let travelled = 0;
  const reached = [];
  for (const item of items) {
    travelled += (item.x * nx + item.y * ny) / total;
    for (const [index, fraction] of LAG_PROGRESS.entries()) {
      if (reached[index] === undefined && travelled >= fraction * total) reached[index] = item.tUs;
    }
  }
  return LAG_PROGRESS.every((_, index) => reached[index] !== undefined) ? reached : null;
}

function speedError(stroke, frames) {
  const errors = frames
    .filter((frame) => frame.tUs >= stroke.startUs + SETTLE_US && frame.speed !== null)
    .map((frame) => (frame.speed - stroke.speedMmPerS) / stroke.speedMmPerS);
  return errors.length ? 100 * rms(errors) : undefined;
}

function sensitivityStep(stroke, frames) {
  const gains = frames
    .filter((frame) => frame.tUs >= stroke.startUs + SETTLE_US && frame.gain !== null)
    .map((frame) => frame.gain);
  if (gains.length < 2) return undefined;
  return (100 * rms(gains.slice(1).map((gain, index) => gain - gains[index]))) / mean(gains);
}

function slowJitter(stroke, reports) {
  const start = stroke.startUs + SETTLE_US;
  const count = Math.floor((stroke.endUs - start) / JITTER_BIN_US);
  if (count < 3) return undefined;
  const bins = Array.from({ length: count }, () => [0, 0]);
  for (const report of reports) {
    const index = Math.floor((report.tUs - start) / JITTER_BIN_US);
    if (report.tUs < start || index >= count) continue;
    bins[index][0] += report.x;
    bins[index][1] += report.y;
  }
  const [nx, ny] = bins.reduce(([x, y], bin) => [x + bin[0], y + bin[1]], [0, 0]);
  const total = Math.hypot(nx, ny);
  if (total === 0) return undefined;
  const values = bins.map(([x, y]) => (x * nx + y * ny) / total);
  const residuals = values.map((value, index) => {
    const window = values.slice(Math.max(0, index - JITTER_HALF_WINDOW),
      index + JITTER_HALF_WINDOW + 1);
    return value - mean(window);
  });
  return (100 * rms(residuals)) / mean(values);
}

function flickLag(frames, reports) {
  const input = progressTimes(frames);
  const output = progressTimes(reports);
  if (!input || !output) return undefined;
  return mean(input.map((time, index) => (output[index] - time) / 1000));
}

/* Output magnitude as each kind measures it: the summed vector for the
 * cursor, the net steps along the intended axis for scroll and text. */
function outputMagnitude(stroke, reports, cursor) {
  const [x, y] = sum(reports);
  if (cursor) return Math.hypot(x, y);
  return Math.abs(stroke.axis === "horizontal" ? x : y);
}

function axisMetrics(stroke, frames, reports) {
  const decided = frames.filter((frame) => frame.intent !== INTENT_UNDECIDED);
  const along = stroke.axis === "horizontal" ? "x" : "y";
  const across = stroke.axis === "horizontal" ? "y" : "x";
  const alongSteps = reports.reduce((total, report) => total + Math.abs(report[along]), 0);
  const acrossSteps = reports.reduce((total, report) => total + Math.abs(report[across]), 0);
  return {
    ...(decided.length ? {
      axisLockPercent: (100 * decided.filter((frame) =>
        frame.intent === INTENT_AXIS[stroke.axis]).length) / decided.length,
      axisSwitches: decided.slice(1)
        .filter((frame, index) => frame.intent !== decided[index].intent).length,
    } : {}),
    ...(alongSteps + acrossSteps ? {
      sidewaysLeakPercent: (100 * acrossSteps) / (alongSteps + acrossSteps),
    } : {}),
  };
}

function strokeMetrics(stroke, frames, reports, fixture) {
  const cursor = fixture.processor.kind === "cursor-pipeline";
  const values = {};
  const set = (name, value) => {
    if (value !== undefined && Number.isFinite(value)) values[name] = value;
  };
  if (cursor && stroke.speedMmPerS !== undefined) {
    set("speedErrorPercent", speedError(stroke, frames));
    if (stroke.intended && stroke.speedMmPerS <= SLOW_SPEED_MM_PER_S) {
      set("slowJitterPercent", slowJitter(stroke, reports));
      set("sensitivityStepPercent", sensitivityStep(stroke, frames));
    }
  }
  if (stroke.activity === "flick") set("flickLagMs", flickLag(frames, reports));
  if (stroke.intended && stroke.intendedOutput) {
    const intended = Math.hypot(...stroke.intendedOutput);
    const actual = outputMagnitude(stroke, reports, cursor);
    set("distanceErrorPercent", (100 * Math.abs(actual - intended)) / intended);
    set("distanceBiasPercent", (100 * (actual - intended)) / intended);
  }
  if (!stroke.intended) {
    set("unintendedOutput", reports.reduce((total, report) =>
      total + (cursor ? Math.hypot(report.x, report.y)
        : Math.abs(report.x) + Math.abs(report.y)), 0));
  }
  if (!cursor && stroke.axis) {
    for (const [name, value] of Object.entries(axisMetrics(stroke, frames, reports))) {
      set(name, value);
    }
  }
  return values;
}

/* Metric values of every ground-truth stroke in a fixture. */
export function scoreFixture(fixture, output) {
  const { frames, reports } = parseTrace(fixture, output);
  const strokes = [...(fixture.truth?.strokes ?? [])].sort((a, b) => a.startUs - b.startUs);
  return strokes.map((stroke, index) => {
    const outputEnd = Math.min(stroke.endUs + TAIL_US, strokes[index + 1]?.startUs ?? Infinity);
    /* Reports carry the whole millisecond the runner reports them at. */
    const outputStart = Math.floor(stroke.startUs / 1000) * 1000;
    return {
      id: stroke.id,
      activity: stroke.activity,
      metrics: strokeMetrics(stroke,
        frames.filter((frame) => frame.tUs >= stroke.startUs && frame.tUs < stroke.endUs),
        reports.filter((report) => report.tUs >= outputStart && report.tUs < outputEnd),
        fixture),
    };
  });
}

/* Mean of each metric over the strokes it applies to. */
export function aggregate(strokes) {
  const scores = {};
  for (const name of Object.keys(METRICS)) {
    const values = strokes.map((stroke) => stroke.metrics[name]).filter((v) => v !== undefined);
    if (values.length) scores[name] = { value: mean(values), strokes: values.length };
  }
  return scores;
}
