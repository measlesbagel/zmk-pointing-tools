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
  /* Scroll and text strokes with a labelled axis: output on the other axis
   * in ball millimetres, so a lower gain can't hide it, and how long the
   * intended axis takes to lock and to give output. */
  wrongAxisOutputMm: { higherIsBetter: false, unit: "mm" },
  axisLockDelayMs: { higherIsBetter: false, unit: "ms" },
  intendedOutputDelayMs: { higherIsBetter: false, unit: "ms" },
  /* Guided cursor trials, by phase. Pixel metrics need a usable screen
   * scale (fitScreenScale). A trial's ballistic landing error aggregates as
   * its spread across trials, the headline, and as its mean, which partly
   * reflects how the user adapted to the recorded gain, so it isn't
   * ranked. */
  ballisticSpreadPercent: { higherIsBetter: false, unit: "%", spread: true },
  ballisticBiasPercent: { higherIsBetter: false, unit: "%", ranked: false },
  homingStepPercent: { higherIsBetter: false, unit: "%" },
  homingJitterPercent: { higherIsBetter: false, unit: "%" },
  homingReversalsPerS: { higherIsBetter: false, unit: "/s" },
  homingStairDegrees: { higherIsBetter: false, unit: "°" },
  transitionStepPercent: { higherIsBetter: false, unit: "%" },
};

/* Metrics that need the screen scale: they read in screen pixels. */
export const PIXEL_METRICS = ["ballisticSpreadPercent", "ballisticBiasPercent",
  "homingStepPercent", "homingJitterPercent"];

const SETTLE_US = 100000;
const TAIL_US = 100000;
const JITTER_BIN_US = 16000;
const JITTER_HALF_WINDOW = 3;
const SLOW_SPEED_MM_PER_S = 50;
const LAG_PROGRESS = [0.1, 0.5, 0.9];
const INTENT_UNDECIDED = 0;
/* A two-axis label means free intent: scrolling on both axes at once. */
const INTENT_AXIS = { horizontal: 2, vertical: 3, both: 1 };
/* Screen scale fit: output and pointer compared every SCALE_GRID_US, with
 * the browser's clock slid against the device's by up to SCALE_OFFSET_US. */
const SCALE_GRID_US = 8000;
const SCALE_OFFSET_US = 80000;
const SCALE_OFFSET_STEP_US = 2000;
const SCALE_MIN_R2 = 0.9;
/* Fast output moving the pointer this much more (or less) per count than
 * slow output is a non-linear mapping, such as the computer's own pointer
 * acceleration. */
const SCALE_NONLINEAR = 1.25;
const SCALE_MIN_STEPS = 20;
/* Phases of a cursor trial: the first submovement ends at the first speed
 * minimum below half the peak, or where speed falls under a tenth of it;
 * homing starts when the pointer first comes within three target widths. */
const SUBMOVEMENT_MINIMUM = 0.5;
const SUBMOVEMENT_STOP = 0.1;
const HOMING_WIDTHS = 3;
const PHASE_BIN_US = 16000;
const PHASE_MIN_BINS = 4;
const TRANSITION_TAIL_US = 150000;
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

/* Ball motion turned the way the fixture's orientation stages turn it. */
function orient(orientations, x, y) {
  for (const { swap, invertX, invertY } of orientations) {
    if (swap) [x, y] = [y, x];
    if (invertX) x = -x;
    if (invertY) y = -y;
  }
  return [x, y];
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
    [dx, dy] = orient(model.orientations, dx, dy);
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
 * its net travel, oriented like the cursor's, along the intended axis, at
 * the pipeline's own steps per count. Text navigation steps along one axis,
 * so a text stroke without a labelled axis keeps only its dominant one. */
function idealAxisOutput(fixture, stroke, profile) {
  const [gx, gy] = axisGains(fixture, profile);
  const { kind } = fixture.processor;
  const chain = kind === "scroll-pipeline" || kind === "text-pipeline";
  const [x, y] = orient(chain ? cursorModel(fixture, profile).orientations : [],
    ...stroke.path.points.at(-1).map((value, axis) => value - stroke.path.points[0][axis]));
  const text = kind === "text-pipeline" || kind === "composed-text";
  const axis = stroke.axis ?? (text ? (Math.abs(x) >= Math.abs(y) ? "horizontal" : "vertical")
    : undefined);
  return [axis === "vertical" ? 0 : x * gx, axis === "horizontal" ? 0 : y * gy];
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
function axisMetrics(stroke, frames, reports, fixture, profile) {
  const decided = frames.filter((frame) => frame.intent !== null &&
    frame.intent !== INTENT_UNDECIDED);
  if (stroke.axis === undefined) {
    return decided.length ? { axisSwitches: decided.slice(1)
      .filter((frame, index) => frame.intent !== decided[index].intent).length } : {};
  }
  const intent = INTENT_AXIS[stroke.axis];
  const firstUs = frames[0]?.tUs;
  const locked = decided.find((frame) => frame.intent === intent);
  const both = stroke.axis === "both";
  const along = stroke.axis === "horizontal" ? "x" : "y";
  const across = stroke.axis === "horizontal" ? "y" : "x";
  const intendedReport = reports.find((report) =>
    (both ? report.x !== 0 || report.y !== 0 : report[along] !== 0));
  const values = {
    ...(decided.length ? {
      axisLockPercent: (100 * decided.filter((frame) => frame.intent === intent).length) /
        decided.length,
      axisSwitches: decided.slice(1)
        .filter((frame, index) => frame.intent !== decided[index].intent).length,
    } : {}),
    ...(locked ? { axisLockDelayMs: (locked.tUs - firstUs) / 1000 } : {}),
    ...(intendedReport && firstUs !== undefined
      ? { intendedOutputDelayMs: Math.max(0, intendedReport.tUs - firstUs) / 1000 } : {}),
  };
  if (both) return values;
  const alongSteps = reports.reduce((total, report) => total + Math.abs(report[along]), 0);
  const acrossSteps = reports.reduce((total, report) => total + Math.abs(report[across]), 0);
  if (alongSteps + acrossSteps) {
    values.sidewaysLeakPercent = (100 * acrossSteps) / (alongSteps + acrossSteps);
  }
  const [gx, gy] = axisGains(fixture, profile);
  const mmPerCount = 25.4 / (fixture.version === 1 ? fixture.processor.settings.cpi
    : fixture.source.cpi);
  const gain = across === "x" ? gx : gy;
  if (gain > 0) values.wrongAxisOutputMm = (acrossSteps / gain) * mmPerCount;
  return values;
}

function strokeMetrics(stroke, frames, reports, fixture, profile, screen) {
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
  if (stroke.activity === "target") {
    /* Where a trial ends depends on the user's corrections, so replay
     * judges its phases, not its outcome. */
    for (const [name, value] of Object.entries(targetMetrics(stroke, frames, reports, fixture,
      screen))) set(name, value);
    return values;
  }
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
    for (const [name, value] of Object.entries(axisMetrics(stroke, frames, reports, fixture,
      profile))) {
      set(name, value);
    }
  }
  return values;
}

/* --- Guided cursor trials ------------------------------------------- */

/* Cumulative output at device time tUs: reports up to and including it. */
function outputTrack(reports) {
  const times = reports.map((report) => report.tUs);
  const positions = [[0, 0]];
  for (const report of reports) {
    const [x, y] = positions.at(-1);
    positions.push([x + report.x, y + report.y]);
  }
  return (tUs) => {
    let low = 0;
    let high = times.length;
    while (low < high) {
      const middle = (low + high) >> 1;
      if (times[middle] <= tUs) low = middle + 1;
      else high = middle;
    }
    return positions[low];
  };
}

/* The browser pointer's position at a time, between its [tUs, x, y]
 * samples, held at both ends. */
function pointerAt(samples, tUs) {
  if (tUs <= samples[0][0]) return [samples[0][1], samples[0][2]];
  if (tUs >= samples.at(-1)[0]) return [samples.at(-1)[1], samples.at(-1)[2]];
  let low = 0;
  let high = samples.length - 1;
  while (high - low > 1) {
    const middle = (low + high) >> 1;
    if (samples[middle][0] <= tUs) low = middle;
    else high = middle;
  }
  const [t0, x0, y0] = samples[low];
  const [t1, x1, y1] = samples[high];
  const f = t1 > t0 ? (tUs - t0) / (t1 - t0) : 0;
  return [x0 + (x1 - x0) * f, y0 + (y1 - y0) * f];
}

/* Least squares pointer step = scale × output step over [dPx, dPy, dOx,
 * dOy] pairs, with the share of pointer motion it explains. */
function fitSteps(pairs) {
  let cross = 0;
  let output = 0;
  let pointer = 0;
  for (const [px, py, ox, oy] of pairs) {
    cross += px * ox + py * oy;
    output += ox * ox + oy * oy;
    pointer += px * px + py * py;
  }
  if (output === 0 || pointer === 0) return null;
  return { scale: cross / output, r2: 1 - (pointer - (cross * cross) / output) / pointer };
}

/* Screen pixels per output count, fitted from a guided cursor recording
 * itself: the recorded settings' replayed output (which is what the device
 * sent) against the browser's pointer path, with the clocks slid against
 * each other. traces are { fixture, output } for every trace of one
 * recording, output being the runner output with the recorded settings, so
 * one fit covers the session. The scale is usable when it explains the
 * pointer's motion (r² ≥ 0.9), fast and slow output move the pointer alike,
 * and the user didn't say the computer's pointer acceleration was on. Null
 * without pointer paths. */
export function fitScreenScale(traces) {
  const trials = traces.flatMap(({ fixture, output }) => {
    const strokes = (fixture.truth?.strokes ?? []).filter((stroke) => stroke.pointer?.length >= 2);
    if (!strokes.length) return [];
    const track = outputTrack(parseTrace(fixture, output).reports);
    return strokes.map((stroke) => ({ stroke, track }));
  });
  if (!trials.length) return null;
  const acceleration = traces[0].fixture.truth.pointerAcceleration ?? "unknown";
  let best = null;
  for (let offsetUs = -SCALE_OFFSET_US; offsetUs <= SCALE_OFFSET_US;
    offsetUs += SCALE_OFFSET_STEP_US) {
    const pairs = [];
    for (const { stroke, track } of trials) {
      let previous = null;
      for (let tUs = stroke.startUs; tUs <= stroke.endUs; tUs += SCALE_GRID_US) {
        const current = [...pointerAt(stroke.pointer, tUs - offsetUs), ...track(tUs)];
        if (previous) pairs.push(current.map((value, axis) => value - previous[axis]));
        previous = current;
      }
    }
    const fit = fitSteps(pairs);
    if (fit && (best === null || fit.r2 > best.r2)) best = { ...fit, offsetUs, pairs };
  }
  if (best === null) {
    return { usable: false, acceleration, reason: "the output or the pointer didn't move" };
  }
  const moving = best.pairs.filter(([, , ox, oy]) => ox !== 0 || oy !== 0)
    .sort((a, b) => Math.hypot(a[2], a[3]) - Math.hypot(b[2], b[3]));
  const third = Math.floor(moving.length / 3);
  const slow = fitSteps(moving.slice(0, third));
  const fast = fitSteps(moving.slice(moving.length - third));
  const ratio = slow && fast && slow.scale > 0 ? fast.scale / slow.scale : null;
  const nonlinear = ratio !== null && (ratio > SCALE_NONLINEAR || ratio < 1 / SCALE_NONLINEAR);
  /* Non-linearity means something only for a fit that holds. */
  const reason = acceleration === "on" ? "the computer's pointer acceleration is on"
    : moving.length < SCALE_MIN_STEPS ? "too little motion to fit"
      : best.r2 < SCALE_MIN_R2 || best.scale <= 0
        ? `the output explains too little of the pointer's motion (r² ${best.r2.toFixed(2)})`
        : nonlinear ? `fast output moved the pointer ${ratio.toFixed(2)}× as far per count as ` +
          "slow output, as the computer's pointer acceleration does"
          : null;
  const round = (value, digits) => Math.round(value * 10 ** digits) / 10 ** digits;
  return { usable: reason === null, pxPerCount: round(best.scale, 4), r2: round(best.r2, 4),
    offsetMs: best.offsetUs / 1000, fastSlowRatio: ratio === null ? null : round(ratio, 3),
    acceleration, ...(reason ? { reason } : {}) };
}

/* When the first submovement peaks and ends, from the ball's own path:
 * the user's input, the same whatever the settings. */
function trialPhases(stroke) {
  const { intervalUs, points } = stroke.path;
  const steps = points.slice(1).map((point, i) =>
    Math.hypot(point[0] - points[i][0], point[1] - points[i][1]));
  const speeds = steps.map((_, i) => mean(steps.slice(Math.max(0, i - 2), i + 3)));
  const peak = speeds.indexOf(Math.max(...speeds));
  const top = speeds[peak];
  if (!(top > 0)) return null;
  let end = speeds.length;
  for (let i = peak + 1; i < speeds.length; i += 1) {
    const minimum = speeds[i] <= speeds[i - 1] && speeds[i] <= (speeds[i + 1] ?? Infinity);
    if (speeds[i] < SUBMOVEMENT_STOP * top || (minimum && speeds[i] < SUBMOVEMENT_MINIMUM * top)) {
      end = i + 1;
      break;
    }
  }
  return { peakUs: stroke.startUs + (peak + 1) * intervalUs,
    ballisticEndUs: stroke.startUs + end * intervalUs };
}

/* Sums of items' x and y in PHASE_BIN_US bins from fromUs to toUs. */
function phaseBins(items, fromUs, toUs) {
  const count = Math.floor((toUs - fromUs) / PHASE_BIN_US);
  const bins = Array.from({ length: Math.max(count, 0) }, () => [0, 0]);
  for (const item of items) {
    const index = Math.floor((item.tUs - fromUs) / PHASE_BIN_US);
    if (index < 0 || index >= count) continue;
    bins[index][0] += item.x;
    bins[index][1] += item.y;
  }
  return bins;
}

/* Per-phase metrics of one guided cursor trial; see docs/trace-benchmark.md,
 * "Guided cursor trials". */
function targetMetrics(stroke, frames, reports, fixture, screen) {
  const values = {};
  const phases = trialPhases(stroke);
  if (!phases) return values;
  const { target, start } = stroke;
  const scale = screen?.usable ? screen.pxPerCount : null;
  const distance = Math.hypot(target.x - start.x, target.y - start.y);
  if (scale !== null && distance > target.size) {
    /* Where the candidate's output would have put the pointer when the
     * first submovement ended, along the line to the target. */
    const [ox, oy] = sum(reports.filter((report) => report.tUs < phases.ballisticEndUs));
    const along = (scale * (ox * (target.x - start.x) + oy * (target.y - start.y))) / distance;
    values.ballisticBiasPercent = (100 * (along - distance)) / distance;
    values.ballisticSpreadPercent = values.ballisticBiasPercent;
  }
  /* The gain as the speed falls out of the first submovement. */
  const gains = frames.filter((frame) => frame.gain !== null && frame.tUs >= phases.peakUs &&
    frame.tUs < phases.ballisticEndUs + TRANSITION_TAIL_US).map((frame) => frame.gain);
  if (gains.length >= 3 && mean(gains) > 0) {
    values.transitionStepPercent =
      (100 * Math.max(...gains.slice(1).map((gain, i) => Math.abs(gain - gains[i])))) / mean(gains);
  }
  /* Homing starts where the pointer the user saw came near the target. */
  const offsetUs = (screen?.offsetMs ?? 0) * 1000;
  const near = stroke.pointer?.find(([tUs, x, y]) => tUs + offsetUs >= stroke.startUs &&
    Math.hypot(x - target.x, y - target.y) <= HOMING_WIDTHS * target.size);
  if (!near) return values;
  const homeUs = near[0] + offsetUs;
  const output = phaseBins(reports, homeUs, stroke.endUs);
  if (output.length < PHASE_MIN_BINS) return values;
  const windowReports = reports.filter((report) => report.tUs >= homeUs &&
    report.tUs < stroke.endUs);
  /* Reversals: sign changes of either axis between bins with output. */
  let reversals = 0;
  for (const axis of [0, 1]) {
    const signs = output.map((bin) => Math.sign(bin[axis])).filter(Boolean);
    reversals += signs.slice(1).filter((sign, i) => sign !== signs[i]).length;
  }
  values.homingReversalsPerS = reversals / ((output.length * PHASE_BIN_US) / 1e6);
  /* Stair-stepping: how far each bin's output turns from the ball's own
   * direction in it, weighted by the output. */
  const { orientations } = cursorModel(fixture);
  const input = phaseBins(frames.map((frame) => {
    const [x, y] = orient(orientations, frame.x, frame.y);
    return { tUs: frame.tUs, x, y };
  }), homeUs, stroke.endUs);
  let turned = 0;
  let weight = 0;
  for (const [i, [ox, oy]] of output.entries()) {
    const [ix, iy] = input[i];
    const size = Math.hypot(ox, oy);
    const inputSize = Math.hypot(ix, iy);
    if (size === 0 || inputSize === 0) continue;
    const cosine = Math.min(1, Math.max(-1, (ox * ix + oy * iy) / (size * inputSize)));
    turned += size * Math.acos(cosine) * (180 / Math.PI);
    weight += size;
  }
  if (weight > 0) values.homingStairDegrees = turned / weight;
  if (scale === null) return values;
  const steps = windowReports.map((report) => Math.hypot(report.x, report.y)).filter(Boolean);
  if (steps.length) values.homingStepPercent = (100 * scale * mean(steps)) / target.size;
  /* Wobble: the pointer's path, bin by bin, against its own moving
   * average. */
  const path = [];
  for (const [x, y] of output) {
    const [px, py] = path.at(-1) ?? [0, 0];
    path.push([px + x, py + y]);
  }
  const residuals = path.map(([x, y], i) => {
    const window = path.slice(Math.max(0, i - JITTER_HALF_WINDOW), i + JITTER_HALF_WINDOW + 1);
    return Math.hypot(x - mean(window.map((point) => point[0])),
      y - mean(window.map((point) => point[1])));
  });
  values.homingJitterPercent = (100 * scale * rms(residuals)) / target.size;
  return values;
}

/* Metric values of every ground-truth stroke in a fixture. screen is the
 * fixture's fitScreenScale, for guided cursor trials. */
export function scoreFixture(fixture, output, profile = null, screen = null) {
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
        fixture, profile, screen),
    };
  });
}

/* Mean of each metric over the strokes it applies to, or for a spread
 * metric the sample standard deviation over at least two strokes. */
export function aggregate(strokes) {
  const scores = {};
  for (const [name, { spread }] of Object.entries(METRICS)) {
    const values = strokes.map((stroke) => stroke.metrics[name]).filter((v) => v !== undefined);
    if (spread) {
      if (values.length < 2) continue;
      const average = mean(values);
      scores[name] = { value: Math.sqrt(values.reduce((total, value) =>
        total + (value - average) ** 2, 0) / (values.length - 1)), strokes: values.length };
    } else if (values.length) scores[name] = { value: mean(values), strokes: values.length };
  }
  return scores;
}
