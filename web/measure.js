/* Pointing measures for the tuner's games. Pure functions over recorded
 * trials, so they run under node:test and every game scores the same way.
 *
 * A trial is one movement from a start point to a circular target:
 *   { start: {x, y}, target: {x, y}, width, startedAt, endedAt,
 *     endpoint: {x, y}, samples: [{t, x, y}, ...] }
 * Coordinates are CSS pixels and times milliseconds.
 *
 * Throughput follows ISO 9241-9 as described by Soukoreff & MacKenzie
 * (2004): effective width We = 4.133 * SD of endpoints along the task axis,
 * IDe = log2(Ae / We + 1), and throughput is the mean of per-sequence
 * IDe / MT. Path measures follow MacKenzie, Kauppinen & Silfverberg (2001);
 * the overshoot ratio follows Casiez et al. (2008). */

const EFFECTIVE_WIDTH_FACTOR = 4.133;

function mean(values) {
  return values.length ? values.reduce((sum, value) => sum + value, 0) / values.length : NaN;
}

function standardDeviation(values) {
  if (values.length < 2) return NaN;
  const average = mean(values);
  return Math.sqrt(values.reduce((sum, value) => sum + (value - average) ** 2, 0) / (values.length - 1));
}

export function median(values) {
  if (!values.length) return NaN;
  const sorted = [...values].sort((a, b) => a - b);
  const middle = Math.floor(sorted.length / 2);
  return sorted.length % 2 ? sorted[middle] : (sorted[middle - 1] + sorted[middle]) / 2;
}

/* The task axis runs from the trial's start to the target centre. */
function taskAxis(trial) {
  const dx = trial.target.x - trial.start.x;
  const dy = trial.target.y - trial.start.y;
  const amplitude = Math.hypot(dx, dy);
  return { ux: dx / amplitude, uy: dy / amplitude, amplitude };
}

/* Distance along the task axis from the start, and signed distance across
 * it (positive to the left of the direction of travel). */
function project(trial, point) {
  const { ux, uy } = taskAxis(trial);
  const px = point.x - trial.start.x;
  const py = point.y - trial.start.y;
  return { along: px * ux + py * uy, across: px * -uy + py * ux };
}

/* Movement direction in degrees, 0 = right, 90 = up (screen y grows down). */
export function direction(trial) {
  const angle = Math.atan2(trial.start.y - trial.target.y, trial.target.x - trial.start.x) * 180 / Math.PI;
  return (angle + 360) % 360;
}

/* One of `bins` equal sectors centred on right, up, left and down. */
export function directionBin(angle, bins = 4) {
  const size = 360 / bins;
  return Math.floor(((angle + size / 2) % 360) / size);
}

export const DIRECTION_LABELS = Object.freeze({
  4: ["right", "up", "left", "down"],
  8: ["right", "up-right", "up", "up-left", "left", "down-left", "down", "down-right"],
});

function inside(trial, point) {
  return Math.hypot(point.x - trial.target.x, point.y - trial.target.y) <= trial.width / 2;
}

/* Times the cursor entered the target after its first entry. */
export function targetReEntries(trial) {
  let entries = 0;
  let wasInside = false;
  for (const sample of trial.samples) {
    const now = inside(trial, sample);
    if (now && !wasInside) entries += 1;
    wasInside = now;
  }
  return Math.max(0, entries - 1);
}

/* How far the path went past the target's far edge, as a fraction of
 * D + W/2 (Casiez et al.); 0 when it never passed. */
export function overshootRatio(trial) {
  const { amplitude } = taskAxis(trial);
  const farEdge = amplitude + trial.width / 2;
  const furthest = Math.max(0, ...trial.samples.map((sample) => project(trial, sample).along));
  return Math.max(0, furthest - farEdge) / farEdge;
}

/* Where the selection landed along the task axis relative to the target
 * centre: positive past it (overshoot), negative short of it. */
export function signedEndpointError(trial) {
  return project(trial, trial.endpoint).along - taskAxis(trial).amplitude;
}

/* Velocity peaks along the path: one for a clean movement, more for each
 * correction. A peak ends when speed falls below 70% of it, and the next
 * one has to rise half again above the dip between them, so sensor jitter
 * and rounding don't count. */
export function submovements(trial, { minimumSpeed = 0.05 } = {}) {
  let peaks = 0;
  let inPeak = false;
  let peak = 0;
  let trough = 0;
  for (let i = 1; i < trial.samples.length; i += 1) {
    const a = trial.samples[i - 1];
    const b = trial.samples[i];
    const dt = b.t - a.t;
    if (dt <= 0) continue;
    const speed = Math.hypot(b.x - a.x, b.y - a.y) / dt;
    if (inPeak) {
      peak = Math.max(peak, speed);
      if (speed < peak * 0.7) {
        inPeak = false;
        trough = speed;
      }
    } else if (speed >= Math.max(minimumSpeed, trough * 1.5)) {
      inPeak = true;
      peaks += 1;
      peak = speed;
    } else {
      trough = Math.min(trough, speed);
    }
  }
  return peaks;
}

/* Pauses inside one movement long enough to be the thumb lifting to roll
 * the ball again. The threshold needs calibrating on real hardware. */
export function rerolls(trial, { gapMs = 30 } = {}) {
  let count = 0;
  for (let i = 1; i < trial.samples.length; i += 1) {
    if (trial.samples[i].t - trial.samples[i - 1].t >= gapMs) count += 1;
  }
  return count;
}

/* Every measure of one trial, flat, for history and copying. */
export function measureTrial(trial) {
  const angle = direction(trial);
  return {
    amplitude: taskAxis(trial).amplitude,
    width: trial.width,
    movementTime: trial.endedAt - trial.startedAt,
    hit: inside(trial, trial.endpoint),
    direction: angle,
    signedError: signedEndpointError(trial),
    acrossError: project(trial, trial.endpoint).across,
    reEntries: targetReEntries(trial),
    overshoot: overshootRatio(trial),
    submovements: submovements(trial),
    rerolls: rerolls(trial),
  };
}

/* ISO 9241-9 throughput of one sequence of trials sharing a nominal A and W.
 * Uses each endpoint's along-axis position, so it needs at least two. */
export function sequenceThroughput(trials) {
  if (trials.length < 2) return NaN;
  const along = trials.map((trial) => project(trial, trial.endpoint).along);
  const effectiveWidth = EFFECTIVE_WIDTH_FACTOR * standardDeviation(along.map((value, i) => value - taskAxis(trials[i]).amplitude));
  const effectiveAmplitude = mean(along);
  const movementSeconds = mean(trials.map((trial) => (trial.endedAt - trial.startedAt) / 1000));
  if (!(effectiveWidth > 0) || !(movementSeconds > 0)) return NaN;
  return Math.log2(effectiveAmplitude / effectiveWidth + 1) / movementSeconds;
}

/* Mean of per-sequence throughputs, as the standard recommends rather than
 * pooling all trials. */
export function throughput(sequences) {
  const values = sequences.map(sequenceThroughput).filter(Number.isFinite);
  return values.length ? mean(values) : NaN;
}

/* Medians of the per-trial measures for a group of measured trials. */
export function summarize(measured) {
  const pick = (key) => median(measured.map((item) => item[key]));
  return {
    trials: measured.length,
    hitRate: measured.length ? measured.filter(({ hit }) => hit).length / measured.length : NaN,
    movementTime: pick("movementTime"),
    signedError: pick("signedError"),
    reEntries: mean(measured.map(({ reEntries }) => reEntries)),
    overshoot: pick("overshoot"),
    submovements: pick("submovements"),
    rerolls: mean(measured.map(({ rerolls: count }) => count)),
  };
}

/* Group measured trials by a key function into summaries. */
export function summarizeBy(measured, key) {
  const groups = new Map();
  for (const item of measured) {
    const group = key(item);
    if (!groups.has(group)) groups.set(group, []);
    groups.get(group).push(item);
  }
  return new Map([...groups].map(([group, items]) => [group, summarize(items)]));
}

/* A deterministic generator for bootstrap resampling, so results and tests
 * repeat. */
export function seededRandom(seed = 1) {
  let state = seed >>> 0 || 1;
  return () => {
    state ^= state << 13;
    state ^= state >>> 17;
    state ^= state << 5;
    return (state >>> 0) / 4294967296;
  };
}

/* Median difference b - a with a percentile bootstrap interval, and the
 * share of resamples where b came out ahead. */
export function bootstrapDifference(a, b, { iterations = 2000, confidence = 0.95, random = seededRandom() } = {}) {
  if (!a.length || !b.length) return undefined;
  const resample = (values) => values.map(() => values[Math.floor(random() * values.length)]);
  const differences = [];
  for (let i = 0; i < iterations; i += 1) differences.push(median(resample(b)) - median(resample(a)));
  differences.sort((x, y) => x - y);
  const tail = (1 - confidence) / 2;
  return {
    difference: median(b) - median(a),
    low: differences[Math.floor(tail * (iterations - 1))],
    high: differences[Math.ceil((1 - tail) * (iterations - 1))],
    probabilityBAhead: differences.filter((value) => value > 0).length / iterations,
  };
}
