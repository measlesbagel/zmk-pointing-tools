/* SPDX-License-Identifier: MIT */

/* Recordings in the tuner (#124): what the panel and the agent tools keep
 * of a capture, the ground truth the tuner derives for a recorded cursor
 * trace, and score comparisons. No DOM and no replay imports, so it runs
 * in tests; recorder.js loads the replay module. */

import { CAPTURE } from "./protocol.js";

export const RECORDING_SCHEMA = "zmk-pointing-tools/recording";
export const RECORDING_VERSION = 1;

/* A rest this long ends a stroke: the acceleration stage's restart gap,
 * which is also when firmware resyncs a recording. */
const STROKE_GAP_US = 40_000;
const MIN_FRAMES = 5;
const MIN_TRAVEL_MM = 0.3;
const PATH_INTERVAL_US = 2_000;
const SMOOTH_HALF_WINDOW = 2;
const STEADY_CV = 0.15;
const SLOW_MM_PER_S = 50;
const FLICK_MM_PER_S = 250;
const FLICK_MAX_US = 300_000;

/* Where each benchmark metric's ground truth comes from in a recording
 * whose truth the tuner derived. null: these recordings carry none, so the
 * metric isn't scored. */
export const DERIVED_TRUTH = {
  speedErrorPercent: "steady strokes: stroke length over its duration",
  slowJitterPercent: "steady strokes at 50 mm/s or slower",
  sensitivityStepPercent: "steady strokes at 50 mm/s or slower",
  flickLagMs: "short strokes faster than 250 mm/s, counted as flicks",
  outputErrorPercent: "every stroke: the smoothed recorded ball path through the scored curve and gain",
  unintendedOutput: null,
  axisLockPercent: null,
  sidewaysLeakPercent: null,
  axisSwitches: null,
};

/* Truth the converter wrote into the trace itself: labelled strokes, and
 * for scroll and text pipelines post-typing and unlabelled strokes. */
export const RECORDED_TRUTH = "the trace's own strokes from the converter (labels, post-typing strokes)";

export const NO_TRUTH = "needs strokes labelled in the recording (unintended motion, or a scroll or text axis)";

export const SCORE_CAVEAT = "The benchmark doesn't judge curve shape or gain: the ideal output follows the curve and gain being scored, so a flatter curve or a lower gain can win every metric while feeling worse. Check curve and gain changes on the keyboard.";

/* Counts of a recording's records, kept up to date as batches arrive:
 * dropped records from sequence gaps, and resyncs from entered records
 * after the first. */
export function createRecordingStats() {
  return { records: 0, frames: 0, keypresses: 0, dropped: 0, entered: 0, settingsChanges: 0,
    firstMs: null, lastMs: null, nextSequence: 0 };
}

export function addRecords(stats, records) {
  for (const record of records) {
    const gap = (record.sequence - stats.nextSequence) >>> 0;
    if (gap > 0 && gap < 0x80000000) stats.dropped += gap;
    stats.nextSequence = (record.sequence + 1) >>> 0;
    stats.records += 1;
    if (record.kind === CAPTURE.RECORD_FRAME) {
      stats.frames += 1;
      stats.firstMs ??= record.timeMs;
      stats.lastMs = record.timeMs;
    } else if (record.kind === CAPTURE.RECORD_KEYPRESS) stats.keypresses += 1;
    else if (record.kind === CAPTURE.RECORD_ENTERED) stats.entered += 1;
    else if (record.kind === CAPTURE.RECORD_SETTINGS_CHANGED) stats.settingsChanges += 1;
  }
  return stats;
}

export const resyncs = (stats) => Math.max(0, stats.entered - 1);

/* Motion events of a fixture v2 trace with absolute times. */
function motionEvents(fixture) {
  let time = 0;
  const out = [];
  for (const event of fixture.events) {
    time += event[1];
    if (event[0] === "motion") out.push({ tUs: time, x: event[2], y: event[3], spanUs: event[4] ?? 0 });
  }
  return out;
}

function groupStrokes(frames) {
  const strokes = [];
  for (const frame of frames) {
    const current = strokes.at(-1);
    if (current && frame.tUs - current.at(-1).tUs < STROKE_GAP_US) current.push(frame);
    else strokes.push([frame]);
  }
  return strokes;
}

/* Positions every PATH_INTERVAL_US from startUs, interpolated between
 * frames, then smoothed with a short moving average whose ends stay put,
 * so the path keeps the stroke's net travel. */
function strokePath(frames, startUs, endUs) {
  const knots = [{ tUs: startUs, x: 0, y: 0 }];
  for (const frame of frames) {
    const last = knots.at(-1);
    knots.push({ tUs: frame.tUs, x: last.x + frame.x, y: last.y + frame.y });
  }
  const raw = [];
  let index = 0;
  for (let tUs = startUs; ; tUs += PATH_INTERVAL_US) {
    const t = Math.min(tUs, endUs);
    while (index < knots.length - 2 && knots[index + 1].tUs <= t) index += 1;
    const a = knots[index];
    const b = knots[Math.min(index + 1, knots.length - 1)];
    const f = b.tUs > a.tUs ? Math.min(Math.max((t - a.tUs) / (b.tUs - a.tUs), 0), 1) : 1;
    raw.push([a.x + (b.x - a.x) * f, a.y + (b.y - a.y) * f]);
    if (t >= endUs) break;
  }
  return raw.map((point, i) => {
    if (i === 0 || i === raw.length - 1) return point;
    const half = Math.min(SMOOTH_HALF_WINDOW, i, raw.length - 1 - i);
    const window = raw.slice(i - half, i + half + 1);
    return [0, 1].map((axis) => window.reduce((sum, p) => sum + p[axis], 0) / window.length);
  }).map(([x, y]) => [Math.round(x * 1000) / 1000, Math.round(y * 1000) / 1000]);
}

/* Whether a path's speed stays within STEADY_CV of its mean, leaving out
 * the first and last fifth where the ball speeds up and slows down. */
function steady(points) {
  const steps = points.slice(1).map((point, i) => Math.hypot(point[0] - points[i][0], point[1] - points[i][1]));
  const middle = steps.slice(Math.floor(steps.length / 5), Math.ceil((steps.length * 4) / 5));
  if (middle.length < 5) return false;
  const mean = middle.reduce((a, b) => a + b, 0) / middle.length;
  if (mean === 0) return false;
  const sd = Math.sqrt(middle.reduce((a, b) => a + (b - mean) ** 2, 0) / middle.length);
  return sd / mean <= STEADY_CV;
}

/* Ground truth for a recorded cursor trace that has none, as
 * docs/trace-benchmark.md describes for recordings: every stroke (motion
 * between rests of 40 ms) is intended, its path is the recorded counts
 * smoothed, a steady stroke's true speed is its length over its duration,
 * and a short fast stroke is a flick. Traces with truth, such as labelled
 * ones, and other kinds are returned unchanged. */
export function deriveTruth(fixture) {
  if (fixture.truth || fixture.processor?.kind !== "cursor-pipeline" || fixture.version !== 2) return fixture;
  const mmPerCount = 25.4 / fixture.source.cpi;
  const strokes = [];
  for (const frames of groupStrokes(motionEvents(fixture))) {
    const travel = frames.reduce((sum, { x, y }) => sum + Math.hypot(x, y), 0) * mmPerCount;
    if (frames.length < MIN_FRAMES || travel < MIN_TRAVEL_MM) continue;
    const startUs = Math.max(0, frames[0].tUs - (frames[0].spanUs || 1000));
    const endUs = frames.at(-1).tUs + 1;
    const points = strokePath(frames, startUs, endUs);
    const speed = (travel * 1e6) / (endUs - startUs);
    const isSteady = steady(points);
    const activity = speed >= FLICK_MM_PER_S && endUs - startUs <= FLICK_MAX_US ? "flick"
      : isSteady && speed <= SLOW_MM_PER_S ? "slow-aim" : "aim";
    strokes.push({
      id: `stroke-${strokes.length + 1}`, activity, intended: true, startUs, endUs,
      ...(isSteady ? { speedMmPerS: Math.round(speed * 10) / 10 } : {}),
      path: { intervalUs: PATH_INTERVAL_US, points },
    });
  }
  return { ...fixture, truth: { strokes, derived: "tuner" } };
}

/* Rows of a score comparison: per metric, each set's mean and stroke
 * count, its change from the first set, and whether that change is better.
 * metrics is quality.js's METRICS. */
export function compareScoreSets(sets, metrics) {
  return Object.entries(metrics).map(([metric, { higherIsBetter, unit }]) => {
    const values = sets.map(({ scores }) => scores[metric] ?? null);
    const base = values[0]?.value;
    return {
      metric, unit, higherIsBetter,
      truth: DERIVED_TRUTH[metric] ?? null,
      values: values.map((entry) => (entry ? { value: Math.round(entry.value * 1000) / 1000, strokes: entry.strokes } : null)),
      change: values.map((entry) => (entry && base !== undefined ? Math.round((entry.value - base) * 1000) / 1000 : null)),
      better: values.map((entry) => (entry && base !== undefined && entry.value !== base
        ? (entry.value > base) === higherIsBetter : null)),
    };
  });
}

function toBase64(bytes) {
  let text = "";
  for (let i = 0; i < bytes.length; i += 0x8000) text += String.fromCharCode(...bytes.subarray(i, i + 0x8000));
  return btoa(text);
}

function fromBase64(text) {
  const binary = atob(text);
  return Uint8Array.from(binary, (c) => c.charCodeAt(0));
}

/* A recording as a file: the capture stream exactly as received, which
 * host/replay/capture.js reads too, and the profiles it ran with. */
export function exportRecording(recording) {
  const { stream, ...rest } = recording;
  return { schema: RECORDING_SCHEMA, version: RECORDING_VERSION, ...rest, stream: toBase64(stream) };
}

export function importRecording(data) {
  if (data?.schema !== RECORDING_SCHEMA || data.version !== RECORDING_VERSION) {
    throw new Error(`expected a ${RECORDING_SCHEMA} v${RECORDING_VERSION} file`);
  }
  if (typeof data.stream !== "string" || !Array.isArray(data.profiles) || typeof data.pipeline !== "string") {
    throw new Error("the recording has no stream, pipeline or profiles");
  }
  const { schema, version, ...rest } = data;
  return { ...rest, label: String(rest.label ?? "Imported").slice(0, 60), stream: fromBase64(data.stream) };
}
