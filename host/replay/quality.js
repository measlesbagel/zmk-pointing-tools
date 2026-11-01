/* SPDX-License-Identifier: MIT */

/*
 * Quality metrics for the trace benchmark. Each metric is defined in
 * docs/trace-benchmark.md; keep this file and that document in step.
 */

import { eventTimesUs, resolveCursorStages } from "./fixture.js";
import { chainLineLengths, parseLines } from "./metrics.js";

/* Metric names in report order, and whether a higher value is better. */
export const METRICS = {
  speedErrorPercent: { higherIsBetter: false, unit: "%" },
  slowJitterPercent: { higherIsBetter: false, unit: "%" },
  sensitivityStepPercent: { higherIsBetter: false, unit: "%" },
  flickLagMs: { higherIsBetter: false, unit: "ms" },
  outputErrorPercent: { higherIsBetter: false, unit: "%" },
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
  const chain = kind === "scroll-pipeline" || kind === "text-pipeline";
  const shape = kind === "cursor-pipeline" ? [9, 4] : kind === "composed-scroll" ? [12, 3]
    : chain ? chainLineLengths(fixture) : [5, 2];
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
      : chain ? { intent: decision[3] < 0 ? null : decision[3] } : { intent: decision[1] }),
  }));
  /* Chain runner reports carry their microsecond time last. */
  const reports = outputs.map((report) => {
    if (kind === "cursor-pipeline" || kind === "scroll-pipeline") {
      return { tUs: report.at(-1), x: report[1], y: report[2] };
    }
    if (kind === "composed-scroll") return { tUs: report[0] * 1000, x: report[1], y: report[2] };
    const [x, y] = TEXT_DIRECTIONS[report[1]];
    return { tUs: kind === "text-pipeline" ? report[2] : report[0] * 1000, x, y };
  });
  return { frames, reports };
}

/* Monotone cubic through the curve points, as the firmware draws it
 * (Fritsch-Carlson, flat beyond both ends). */
export function curvePercent(curve, speed) {
  const { speeds: xs, percents: ys } = curve;
  const n = xs.length;
  if (speed <= xs[0]) return ys[0];
  if (speed >= xs[n - 1]) return ys[n - 1];
  const tangent = (i) => {
    if (i === 0 || i === n - 1) return 0;
    const h0 = xs[i] - xs[i - 1];
    const h1 = xs[i + 1] - xs[i];
    const d0 = (ys[i] - ys[i - 1]) / h0;
    const d1 = (ys[i + 1] - ys[i]) / h1;
    if (d0 === 0 || d1 === 0 || Math.sign(d0) !== Math.sign(d1)) return 0;
    const w1 = 2 * h1 + h0;
    const w2 = h1 + 2 * h0;
    return (w1 + w2) / (w1 / d0 + w2 / d1);
  };
  let i = 0;
  while (speed >= xs[i + 1]) i += 1;
  const h = xs[i + 1] - xs[i];
  const t = (speed - xs[i]) / h;
  return ys[i] * (2 * t ** 3 - 3 * t ** 2 + 1) + tangent(i) * h * (t ** 3 - 2 * t ** 2 + t) +
    ys[i + 1] * (-2 * t ** 3 + 3 * t ** 2) + tangent(i + 1) * h * (t ** 3 - t ** 2);
}

/* What an ideal cursor pipeline does to ball motion. The acceleration curve
 * and transfer gain are choices, so they come from the stages with the
 * replay's settings. Orientation is the sensor's mounting, part of the
 * truth, so it comes from the fixture's own settings: a profile that turns
 * motion the wrong way scores as wrong. */
function cursorModel(fixture, profile) {
  if (fixture.version === 1) {
    const { scaleMultiplier, scaleDivisor } = fixture.processor.settings;
    return { orientations: [], curve: null, gain: scaleMultiplier / scaleDivisor };
  }
  const model = { orientations: [], curve: null, gain: 1 };
  for (const { type, settings } of resolveCursorStages(fixture)) {
    if (type === "orientation") {
      model.orientations.push({ swap: Boolean(settings["swap-xy"]),
        invertX: Boolean(settings["invert-x"]), invertY: Boolean(settings["invert-y"]) });
    }
  }
  for (const { type, settings } of resolveCursorStages(fixture, profile)) {
    if (type === "acceleration") {
      const points = [1, 2, 3, 4, 5, 6, 7, 8];
      model.curve = { speeds: points.map((i) => settings[`curve-speed-${i}`]),
        percents: points.map((i) => settings[`curve-sensitivity-${i}`]) };
    } else if (type === "transfer") {
      model.gain *= settings["scale-multiplier"] / settings["scale-divisor"];
    }
  }
  return model;
}

/* The output of an ideal cursor pipeline that knows the stroke's true
 * motion: each path step oriented, scaled by the transfer gain and by the
 * curve at the step's true speed, in pointer units. */
export function idealCursorOutput(fixture, stroke, profile = null) {
  const model = cursorModel(fixture, profile);
  const { intervalUs, points } = stroke.path;
  const millimetresPerCount = 25.4 / fixture.source.cpi;
  let [ox, oy] = [0, 0];
  for (let i = 1; i < points.length; i += 1) {
    let dx = points[i][0] - points[i - 1][0];
    let dy = points[i][1] - points[i - 1][1];
    const speed = (Math.hypot(dx, dy) * millimetresPerCount * 1e6) / intervalUs;
    const gain = model.gain * (model.curve ? curvePercent(model.curve, speed) / 100 : 1);
    for (const { swap, invertX, invertY } of model.orientations) {
      if (swap) [dx, dy] = [dy, dx];
      if (invertX) dx = -dx;
      if (invertY) dy = -dy;
    }
    ox += dx * gain;
    oy += dy * gain;
  }
  return [ox, oy];
}

/* Output steps per ball count on each axis of a scroll or text pipeline,
 * with the replay's settings: the scroll batcher's steps per millimetre,
 * or one text step per threshold of travel. */
function axisGains(fixture, profile) {
  const { kind } = fixture.processor;
  const cpi = fixture.version === 1 ? fixture.processor.settings.cpi : fixture.source.cpi;
  const mmPerCount = 25.4 / cpi;
  const chain = kind === "scroll-pipeline" || kind === "text-pipeline";
  const settings = chain
    ? Object.assign({}, ...resolveCursorStages(fixture, profile)
      .filter(({ type }) => type === "scroll-batcher" || type === "text-nav")
      .map((stage) => stage.settings))
    : fixture.processor.settings;
  if (kind === "scroll-pipeline" || kind === "composed-scroll") {
    const perMm = (chain ? settings["steps-per-meter"] : settings.stepsPerMeter) / 1000;
    return [perMm * mmPerCount, perMm * mmPerCount];
  }
  const thresholds = chain
    ? [settings["horizontal-threshold"], settings["vertical-threshold"]]
    : [settings.horizontalThresholdMicrometers, settings.verticalThresholdMicrometers];
  return thresholds.map((micrometres) => (mmPerCount * 1000) / micrometres);
}

/* The output of an ideal scroll or text pipeline for a stroke's ball path:
 * its net travel, along the intended axis when there is one, at the
 * pipeline's own steps per count. */
function idealAxisOutput(fixture, stroke, profile) {
  const [gx, gy] = axisGains(fixture, profile);
  const [x, y] = stroke.path.points.at(-1).map((value, axis) =>
    value - stroke.path.points[0][axis]);
  return [stroke.axis === "vertical" ? 0 : x * gx, stroke.axis === "horizontal" ? 0 : y * gy];
}

/* Ball travel along a stroke's path, in counts. */
function pathTravel(stroke) {
  const points = stroke.path?.points ?? [];
  return points.slice(1).reduce((total, point, i) =>
    total + Math.hypot(point[0] - points[i][0], point[1] - points[i][1]), 0);
}

/* The pipeline's gain for slow, unintended motion: transfer gain times the
 * curve's first-point sensitivity. Zero leaves the metric undefined. */
function slowGain(model) {
  return model.gain * (model.curve ? model.curve.percents[0] / 100 : 1);
}

/* The intended output: explicit truth, such as a playground target, or for
 * the cursor the ideal output of the stroke's ball path. */
function intendedOutput(stroke, fixture, profile) {
  if (stroke.intendedOutput) return stroke.intendedOutput;
  if (stroke.path && fixture.processor.kind === "cursor-pipeline") {
    return idealCursorOutput(fixture, stroke, profile);
  }
  if (stroke.path) return idealAxisOutput(fixture, stroke, profile);
  return undefined;
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

/* Axis metrics against the stroke's intended axis. Without one, as for an
 * unlabelled or derived recording stroke, only axis switches apply: they
 * need no truth. */
function axisMetrics(stroke, frames, reports) {
  const decided = frames.filter((frame) => frame.intent !== null &&
    frame.intent !== INTENT_UNDECIDED);
  if (stroke.axis === undefined) {
    return decided.length ? { axisSwitches: decided.slice(1)
      .filter((frame, index) => frame.intent !== decided[index].intent).length } : {};
  }
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

function strokeMetrics(stroke, frames, reports, fixture, profile) {
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
  const intended = stroke.intended ? intendedOutput(stroke, fixture, profile) : undefined;
  if (intended) {
    /* Net output against the intended vector, so direction counts too. */
    const [x, y] = sum(reports);
    const size = Math.hypot(...intended);
    if (size > 0 && Math.hypot(x, y) > 0) {
      set("outputErrorPercent", (100 * Math.hypot(x - intended[0], y - intended[1])) / size);
      set("distanceBiasPercent", (100 * (Math.hypot(x, y) - size)) / size);
    } else if (size > 0 || pathTravel(stroke) > 0) {
      /* The ball moved but either side gave nothing: all of it is lost. */
      set("outputErrorPercent", 100);
      set("distanceBiasPercent", -100);
    }
  }
  if (!stroke.intended) {
    /* Output counts in ball counts, so lowering the gain can't hide it. */
    if (cursor) {
      const scale = slowGain(cursorModel(fixture, profile));
      set("unintendedOutput", reports.reduce((total, report) =>
        total + Math.hypot(report.x, report.y), 0) / scale);
    } else {
      const [gx, gy] = axisGains(fixture, profile);
      set("unintendedOutput", reports.reduce((total, report) =>
        total + Math.abs(report.x) / gx + Math.abs(report.y) / gy, 0));
    }
  }
  if (!cursor && (stroke.axis || ["scroll", "text"].includes(stroke.activity))) {
    for (const [name, value] of Object.entries(axisMetrics(stroke, frames, reports))) {
      set(name, value);
    }
  }
  return values;
}

/* Metric values of every ground-truth stroke in a fixture. */
export function scoreFixture(fixture, output, profile = null) {
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
        fixture, profile),
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
