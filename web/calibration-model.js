/* SPDX-License-Identifier: MIT */

import { STATE, TARGET_KIND } from "./protocol.js";
import { PROFILE_SCHEMA, PROFILE_VERSION } from "./profile.js";

/*
 * Repeated-gesture calibration over every-frame stage samples.
 *
 * Distances are measured in millimetres at a resolution-normalize stage,
 * before any gain, batching, or report interval, so a recommendation never
 * applies report cadence or transport coalescing a second time. Analysis is
 * a pure function of the recorded frames and options: the same recording
 * always produces the same recommendation.
 */

export const CALIBRATION_SCHEMA = "zmk-pointing-tools/calibration";
export const CALIBRATION_VERSION = 1;

/* values[0] marker of an every-frame stage sample (ZPT_STATE_STAGE_FRAME). */
export const STAGE_FRAME = 128;

export const SIGNAL_KIND = Object.freeze({
  RAW_MOTION: 1,
  NORMALIZED_MOTION: 2,
  POINTER_DELTA: 3,
  SCROLL_STEPS: 4,
  ACTION: 5,
});

export const SIGNAL_FLAG = Object.freeze({
  COALESCED: 1 << 2,
  TIMING_ESTIMATED: 1 << 3,
  CLIPPED: 1 << 4,
  SEQUENCE_GAP: 1 << 5,
  DISCONTINUITY: 1 << 6,
});

const Q16 = 65536;
const MM_PER_INCH = 25.4;

export const DEFAULT_PAUSE_MS = 150;
const MIN_GESTURE_MM = 1;
const MIN_GESTURE_FRAMES = 3;
export const MIN_GESTURES = 3;

/* Calibration modes, keyed by the tuning parameters that identify a target
 * the mode can calibrate. */
export const MODES = Object.freeze({
  cursor: Object.freeze({
    id: "cursor",
    label: "Cursor gain",
    unit: "pixels",
    defaultOutput: 800,
    keys: ["scale-multiplier", "scale-divisor"],
    instructions: "Sweep the ball as you would to move the cursor the distance you entered, " +
      "then stop. Repeat 5 to 10 times with a short pause between sweeps.",
  }),
  scroll: Object.freeze({
    id: "scroll",
    label: "Scroll speed",
    unit: "scroll steps",
    defaultOutput: 10,
    keys: ["steps-per-meter"],
    instructions: "Make one comfortable scroll gesture that should scroll the number of steps " +
      "you entered, then stop. Repeat 5 to 10 times along the same axis.",
  }),
  text: Object.freeze({
    id: "text",
    label: "Text step",
    unit: "key taps",
    defaultOutput: 5,
    keys: ["horizontal-threshold", "vertical-threshold"],
    instructions: "Make one gesture that should move the caret the number of taps you entered, " +
      "then stop. Repeat 5 to 10 times along the same axis.",
  }),
});

const ACCELERATION_KEYS = ["slow-speed", "fast-speed", "slow-gain", "fast-gain", "speed-window"];

function asTargets(targets) {
  return targets instanceof Map ? [...targets.values()] : [...targets];
}

function hasKeys(target, keys) {
  return keys.every((key) => target.parameters?.some((parameter) => parameter.key === key));
}

function parameterByKey(target, key) {
  return target?.parameters?.find((parameter) => parameter.key === key);
}

/* Every tuning target a calibration mode can set, in discovery order. */
export function calibrationTargets(targets) {
  const found = [];
  for (const target of asTargets(targets)) {
    for (const mode of Object.values(MODES)) {
      if (hasKeys(target, mode.keys)) found.push({ mode: mode.id, target });
    }
  }
  return found;
}

export function accelerationTargets(targets) {
  return asTargets(targets).filter((target) => hasKeys(target, ACCELERATION_KEYS));
}

/* The acceleration target most likely to share a pipeline with the
 * calibrated transfer: the longest common stable-id prefix wins. */
export function matchAccelerationTarget(transfer, candidates) {
  let best;
  let bestScore = -1;
  const reference = transfer?.stableId ?? transfer?.label ?? "";
  for (const candidate of candidates) {
    const name = candidate.stableId ?? candidate.label ?? "";
    let score = 0;
    while (score < name.length && score < reference.length && name[score] === reference[score]) score += 1;
    if (score > bestScore) {
      best = candidate;
      bestScore = score;
    }
  }
  return best;
}

/* An acceleration target's current values in the stage's own units. */
export function accelerationConfig(target) {
  if (!target || !hasKeys(target, ACCELERATION_KEYS)) return undefined;
  const value = (key) => parameterByKey(target, key).current;
  return {
    stableId: target.stableId,
    slowSpeed: value("slow-speed"),
    fastSpeed: value("fast-speed"),
    slowGainPercent: value("slow-gain"),
    fastGainPercent: value("fast-gain"),
    speedWindowMs: value("speed-window"),
  };
}

/* Decode an every-frame stage sample; anything else returns undefined. */
export function parseStageFrame(sample) {
  if (sample?.targetKind !== TARGET_KIND.PIPELINE_STAGE || sample.event !== STATE.EVENT_FRAME ||
      sample.values?.[0] !== STAGE_FRAME) {
    return undefined;
  }
  const [, kind, x, y, cpi, spanUs, sequence, flags, sourceId, speed] = sample.values;
  return {
    targetId: sample.targetId,
    timestamp: sample.timestamp,
    stateSequence: sample.sequence,
    intent: sample.intent,
    kind,
    x,
    y,
    cpi,
    spanUs,
    sequence,
    flags,
    sourceId,
    speed,
  };
}

/* State samples the host never received, from gaps in the shared sequence. */
export function countSequenceGaps(samples) {
  let missing = 0;
  let previous;
  for (const sample of samples) {
    if (previous !== undefined) {
      const step = (sample.sequence - previous) >>> 0;
      if (step > 1 && step < 0x80000000) missing += step - 1;
    }
    previous = sample.sequence;
  }
  return missing;
}

function elapsed(from, to) {
  return (to - from) >>> 0;
}

function median(values) {
  if (values.length === 0) return 0;
  const sorted = [...values].sort((a, b) => a - b);
  const middle = Math.floor(sorted.length / 2);
  return sorted.length % 2 ? sorted[middle] : (sorted[middle - 1] + sorted[middle]) / 2;
}

function statistics(values) {
  if (values.length === 0) return { median: 0, mean: 0, sd: 0, cv: 0 };
  const mean = values.reduce((sum, value) => sum + value, 0) / values.length;
  const variance = values.length > 1
    ? values.reduce((sum, value) => sum + (value - mean) ** 2, 0) / (values.length - 1)
    : 0;
  const sd = Math.sqrt(variance);
  return { median: median(values), mean, sd, cv: mean > 0 ? sd / mean : 0 };
}

function mode(values) {
  const counts = new Map();
  for (const value of values) counts.set(value, (counts.get(value) ?? 0) + 1);
  let best;
  for (const [value, count] of counts) if (best === undefined || count > counts.get(best)) best = value;
  return best;
}

/* Mirror of the firmware cursor-acceleration stage over millimetres: a
 * linearly decaying distance window gives speed, and the gain ramps
 * linearly between the slow and fast speeds. */
function createAccelerationModel(config) {
  let window = 0;
  let last;
  return (dx, dy, timestamp) => {
    const span = config.speedWindowMs;
    const since = last === undefined ? span : elapsed(last, timestamp);
    window = since >= span ? 0 : window * (span - since) / span;
    window += Math.hypot(dx, dy);
    last = timestamp;
    const speed = window * 1000 / span;
    const slow = config.slowGainPercent / 100;
    const fast = config.fastGainPercent / 100;
    if (speed <= config.slowSpeed) return slow;
    if (speed >= config.fastSpeed) return fast;
    return slow + (fast - slow) * (speed - config.slowSpeed) / (config.fastSpeed - config.slowSpeed);
  };
}

/* Split normalized motion into gestures at pauses or source changes. */
export function segmentGestures(frames, pauseMs = DEFAULT_PAUSE_MS) {
  const gestures = [];
  let current;
  for (const frame of frames) {
    if (frame.kind !== SIGNAL_KIND.NORMALIZED_MOTION || (frame.x === 0 && frame.y === 0)) continue;
    if (!current || frame.sourceId !== current[0].sourceId ||
        elapsed(current[current.length - 1].timestamp, frame.timestamp) > pauseMs) {
      current = [];
      gestures.push(current);
    }
    current.push(frame);
  }
  return gestures;
}

/* Per-gesture evidence. Transport effects (cadence, coalescing, gaps) are
 * kept apart from distance so they can be judged separately. */
export function measureGesture(frames, accelerate) {
  let x = 0;
  let y = 0;
  let acceleratedX = 0;
  let acceleratedY = 0;
  const magnitudes = [];
  const intervals = [];
  let coalesced = 0;
  let transportGaps = 0;
  let clipped = 0;
  let missingFrames = 0;
  for (let index = 0; index < frames.length; index += 1) {
    const frame = frames[index];
    const dx = frame.x / Q16;
    const dy = frame.y / Q16;
    x += dx;
    y += dy;
    const gain = accelerate ? accelerate(dx, dy, frame.timestamp) : 1;
    acceleratedX += dx * gain;
    acceleratedY += dy * gain;
    magnitudes.push(Math.hypot(dx, dy));
    if (frame.flags & SIGNAL_FLAG.COALESCED) coalesced += 1;
    if (frame.flags & (SIGNAL_FLAG.SEQUENCE_GAP | SIGNAL_FLAG.DISCONTINUITY)) transportGaps += 1;
    if (frame.flags & SIGNAL_FLAG.CLIPPED) clipped += 1;
    if (index > 0) {
      const previous = frames[index - 1];
      intervals.push(elapsed(previous.timestamp, frame.timestamp));
      const step = (frame.sequence - previous.sequence) & 0xffff;
      /* Sequence 0 or a large jump means the source does not number its
       * frames or restarted; neither is evidence of loss. */
      if (step > 1 && step < 1000) missingFrames += step - 1;
    }
  }

  /* Frames that move against the gesture by at least a typical frame are
   * reversals: either the hand or a sign-corrupted split frame. */
  const net = Math.hypot(x, y);
  const typical = median(magnitudes);
  let reversed = 0;
  if (net > 0) {
    for (const frame of frames) {
      const along = (frame.x * x + frame.y * y) / (Q16 * net);
      if (along < 0 && -along >= typical) reversed += 1;
    }
  }

  const first = frames[0];
  const last = frames[frames.length - 1];
  const axis = Math.abs(x) >= Math.abs(y) ? "x" : "y";
  return {
    sourceId: first.sourceId,
    startMs: first.timestamp,
    durationMs: elapsed(first.timestamp, last.timestamp) + first.spanUs / 1000,
    frames: frames.length,
    axis,
    netMm: net,
    axisMm: Math.abs(axis === "x" ? x : y),
    pathMm: magnitudes.reduce((sum, value) => sum + value, 0),
    acceleratedMm: Math.hypot(acceleratedX, acceleratedY),
    cpi: mode(frames.map((frame) => frame.cpi)),
    cpiChanged: frames.some((frame) => frame.cpi !== first.cpi),
    intervalMs: median(intervals),
    spanMs: median(frames.map((frame) => frame.spanUs / 1000)),
    coalesced,
    transportGaps,
    missingFrames,
    clipped,
    reversed,
  };
}

/* The multiplier/divisor pair closest to a gain, preferring the smallest
 * divisor within 0.1 %, so the result stays readable in devicetree. */
export function rationalGain(gain, maxMultiplier = 1000, maxDivisor = 1000) {
  let best;
  for (let divisor = 1; divisor <= maxDivisor; divisor += 1) {
    const multiplier = Math.round(gain * divisor);
    if (multiplier < 1 || multiplier > maxMultiplier) continue;
    const error = Math.abs(multiplier / divisor - gain) / gain;
    if (!best || error < best.error) best = { multiplier, divisor, error };
    if (error <= 0.001) break;
  }
  return best;
}

function clampToParameter(value, parameter) {
  const minimum = parameter?.minimum ?? -Infinity;
  const maximum = parameter?.maximum ?? Infinity;
  const clamped = Math.min(maximum, Math.max(minimum, Math.round(value)));
  return { value: clamped, clamped: clamped !== Math.round(value) };
}

function recommend(mode, desiredOutput, central, axis, cpi, target) {
  if (mode === "cursor") {
    /* The quantizer emits CPI/25.4 pointer units per millimetre at unity
     * gain; transfer gain multiplies on top. */
    const unity = central * cpi / MM_PER_INCH;
    const gain = desiredOutput / unity;
    const pair = rationalGain(gain, parameterByKey(target, "scale-multiplier")?.maximum ?? 1000,
      parameterByKey(target, "scale-divisor")?.maximum ?? 1000);
    if (!pair) return { gain, parameters: [], clamped: true };
    return {
      gain,
      clamped: pair.error > 0.01,
      parameters: [
        { key: "scale-multiplier", value: pair.multiplier },
        { key: "scale-divisor", value: pair.divisor },
      ],
    };
  }
  if (mode === "scroll") {
    const fitted = clampToParameter(desiredOutput * 1000 / central,
      parameterByKey(target, "steps-per-meter"));
    return { clamped: fitted.clamped, parameters: [{ key: "steps-per-meter", value: fitted.value }] };
  }
  /* Partial travel is dropped after the idle timeout, so a gesture yields
   * floor(distance / threshold) taps. Centring the typical gesture between
   * N and N+1 taps tolerates variation in both directions. */
  const key = axis === "x" ? "horizontal-threshold" : "vertical-threshold";
  const fitted = clampToParameter(central * 1000 / (desiredOutput + 0.5), parameterByKey(target, key));
  return { clamped: fitted.clamped, parameters: [{ key, value: fitted.value }] };
}

/*
 * Analyse a recording. Options:
 *   mode            "cursor" | "scroll" | "text"
 *   desiredOutput   pixels, scroll steps, or key taps per gesture
 *   target          tuning target being calibrated (optional; ranges)
 *   acceleration    accelerationConfig() for cursor mode (optional)
 *   telemetryDropped  state samples lost during the recording
 *   pauseMs         gesture separation
 */
export function analyzeCalibration(frames, options) {
  const calibration = MODES[options.mode];
  if (!calibration) throw new Error(`Unknown calibration mode ${options.mode}`);
  const desiredOutput = Number(options.desiredOutput);
  if (!(desiredOutput > 0)) throw new Error("Desired output must be positive");

  const accelerate = options.mode === "cursor" && options.acceleration
    ? createAccelerationModel(options.acceleration)
    : undefined;
  const all = segmentGestures(frames, options.pauseMs ?? DEFAULT_PAUSE_MS)
    .map((gesture) => measureGesture(gesture, accelerate))
    .filter((gesture) => gesture.netMm >= MIN_GESTURE_MM && gesture.frames >= MIN_GESTURE_FRAMES);

  /* Calibrate the source that moved the most; other sources are ignored
   * rather than mixed, since each ball has its own gain. */
  const travel = new Map();
  for (const gesture of all) travel.set(gesture.sourceId, (travel.get(gesture.sourceId) ?? 0) + gesture.netMm);
  let sourceId;
  for (const [source, distance] of travel) if (sourceId === undefined || distance > travel.get(sourceId)) sourceId = source;
  const gestures = all.filter((gesture) => gesture.sourceId === sourceId);
  const ignored = all.length - gestures.length;

  const axis = mode(gestures.map((gesture) => gesture.axis)) ?? "x";
  const distanceOf = (gesture) => options.mode === "cursor"
    ? gesture.acceleratedMm
    : gesture.axisMm;
  const distance = statistics(gestures.map(distanceOf));
  const cpi = mode(gestures.map((gesture) => gesture.cpi)) ?? 0;
  const sum = (field) => gestures.reduce((total, gesture) => total + gesture[field], 0);
  const summary = {
    sourceId,
    gestureCount: gestures.length,
    ignoredGestures: ignored,
    axis,
    distanceMm: distance,
    netMm: statistics(gestures.map((gesture) => gesture.netMm)),
    durationMs: median(gestures.map((gesture) => gesture.durationMs)),
    framesPerGesture: median(gestures.map((gesture) => gesture.frames)),
    totalFrames: sum("frames"),
    intervalMs: median(gestures.map((gesture) => gesture.intervalMs)),
    spanMs: median(gestures.map((gesture) => gesture.spanMs)),
    cpi,
    coalescedFrames: sum("coalesced"),
    transportGapFrames: sum("transportGaps"),
    missingFrames: sum("missingFrames"),
    clippedFrames: sum("clipped"),
    reversedFrames: sum("reversed"),
    telemetryDropped: options.telemetryDropped ?? 0,
  };

  const limitations = [];
  let level;
  if (gestures.length < MIN_GESTURES || distance.median <= 0 || cpi <= 0) {
    level = "insufficient";
    limitations.push(`Record at least ${MIN_GESTURES} gestures of ${MIN_GESTURE_MM} mm or more; 5 to 10 is better.`);
  } else {
    level = distance.cv <= 0.1 ? "high" : distance.cv <= 0.2 ? "medium" : "low";
    if (distance.cv > 0.2) limitations.push("Gestures varied by more than 20 %; repeat them more consistently.");
  }
  const lower = () => { if (level === "high" || level === "medium") level = "low"; };
  if (summary.telemetryDropped > 0) {
    lower();
    limitations.push(`${summary.telemetryDropped} telemetry samples were dropped, so distances are under-counted. Enable only the measured stage and record again.`);
  }
  if (summary.missingFrames > 0 || summary.transportGapFrames > 0) {
    lower();
    limitations.push(`The sensor or split link lost frames (${summary.missingFrames} missing, ${summary.transportGapFrames} flagged gaps); distances may be short.`);
  }
  if (summary.clippedFrames > 0) {
    lower();
    limitations.push(`${summary.clippedFrames} frames were clipped at a range limit.`);
  }
  if (summary.reversedFrames > summary.totalFrames * 0.05) {
    limitations.push(`${summary.reversedFrames} frames moved against the gesture, which can mean sign-corrupted split frames or a ball rolling back.`);
  }
  if (gestures.some((gesture) => gesture.cpiChanged) || new Set(gestures.map((gesture) => gesture.cpi)).size > 1) {
    lower();
    limitations.push("The sensor resolution changed during the recording.");
  }
  if (ignored > 0) limitations.push(`${ignored} gestures from another source were ignored.`);
  if (options.mode !== "cursor" && gestures.some((gesture) => gesture.axis !== axis)) {
    limitations.push("Some gestures ran along the other axis; only the main axis was used.");
  }
  if (summary.coalescedFrames > 0) {
    limitations.push(`${summary.coalescedFrames} frames arrived coalesced. Distance is unaffected, since it is measured before batching.`);
  }
  if (options.mode === "cursor") {
    limitations.push(`Cursor gain is relative to ${cpi} CPI; changing CPI later scales cursor speed with it.`);
    limitations.push(options.acceleration
      ? `Acceleration modelled from ${options.acceleration.stableId ?? "the acceleration stage"}'s current values; change them first if you plan to.`
      : "No acceleration was modelled; with acceleration enabled the cursor will travel differently.");
    limitations.push("Operating-system pointer acceleration is not modelled; use a flat pointer profile.");
  }

  const recommendation = level === "insufficient"
    ? { parameters: [], clamped: false }
    : recommend(options.mode, desiredOutput, distance.median, axis, cpi, options.target);
  if (recommendation.clamped) {
    lower();
    limitations.push("The ideal value is outside the parameter's range; the nearest allowed value is shown.");
  }

  return {
    mode: calibration.id,
    unit: calibration.unit,
    desiredOutput,
    summary,
    gestures,
    recommendation,
    confidence: { level, limitations },
  };
}

/* A calibration export is a tuning profile carrying only the recommended
 * values, so Import profile applies it, plus the evidence behind it. */
export function createCalibrationExport(analysis, context, exportedAt = new Date().toISOString()) {
  const { target } = context;
  const profileTargets = target && analysis.recommendation.parameters.length ? [{
    stableId: target.stableId,
    kind: target.kind,
    label: target.label,
    devicetreePath: target.devicetreePath,
    parameters: analysis.recommendation.parameters.map(({ key, value }) => {
      const parameter = parameterByKey(target, key);
      return { key, devicetreeProperty: parameter?.devicetreeProperty, value, compiled: parameter?.compiled };
    }),
  }] : [];
  return {
    schema: PROFILE_SCHEMA,
    version: PROFILE_VERSION,
    exportedAt,
    targets: profileTargets,
    calibration: {
      schema: CALIBRATION_SCHEMA,
      version: CALIBRATION_VERSION,
      mode: analysis.mode,
      unit: analysis.unit,
      desiredOutput: analysis.desiredOutput,
      measuredAt: context.measureLabel,
      acceleration: context.acceleration,
      summary: analysis.summary,
      gestures: analysis.gestures,
      recommendation: analysis.recommendation,
      confidence: analysis.confidence,
      frames: context.frames,
    },
  };
}
