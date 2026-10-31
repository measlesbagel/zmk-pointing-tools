#!/usr/bin/env node
/* SPDX-License-Identifier: MIT */

/*
 * Generates the synthetic benchmark fixtures in host/bench/fixtures.
 *
 * Every fixture is labelled synthetic. The sensor model and the strokes are
 * described in docs/trace-benchmark.md; real recordings replace these as the
 * trace capture tool (#115) produces them. Output is deterministic.
 */

import { mkdirSync, readFileSync, writeFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const ROOT = dirname(fileURLToPath(import.meta.url));
const GENERATOR = "host/bench/generate.js";

/* ---- Deterministic randomness ------------------------------------------ */

function random(seed) {
  let state = seed >>> 0;
  return () => {
    state = (state + 0x6d2b79f5) >>> 0;
    let t = state;
    t = Math.imul(t ^ (t >>> 15), t | 1);
    t ^= t + Math.imul(t ^ (t >>> 7), t | 61);
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
}

/* ---- Pipelines in the shape of the Bridges V2 keymap -------------------- */

const BRIDGES_CURVE = {
  speeds: [0, 25, 40, 63, 100, 158, 250, 400],
  percents: [100, 100, 110, 135, 165, 190, 200, 200],
};

function curveSettings(curve) {
  const settings = {};
  curve.speeds.forEach((speed, i) => { settings[`curve-speed-${i + 1}`] = speed; });
  curve.percents.forEach((percent, i) => { settings[`curve-sensitivity-${i + 1}`] = percent; });
  return settings;
}

function bridgesCursorStages() {
  return [
    { type: "orientation", stableId: "right-cursor-orientation",
      settings: { "invert-x": 1, "invert-y": 1 } },
    { type: "normalize", stableId: "right-cursor-normalize" },
    { type: "gate", stableId: "right-cursor-gate",
      settings: { enabled: 0, "activation-distance": 223, coherence: 60,
        "qualification-timeout": 160, "idle-timeout": 120 } },
    { type: "acceleration", stableId: "right-cursor-acceleration",
      settings: { ...curveSettings(BRIDGES_CURVE), "speed-rise": 10, "speed-fall": 5 } },
    { type: "transfer", stableId: "right-cursor-transfer",
      settings: { "scale-multiplier": 1, "scale-divisor": 1 } },
    { type: "quantizer", stableId: "right-cursor-quantizer" },
    { type: "coalescer", stableId: "right-cursor-coalescer",
      settings: { "usb-report-interval": 1, "ble-report-interval": 8 } },
  ];
}

const LEFT_SCROLL_SETTINGS = {
  stepsPerMeter: 3366, reportIntervalMs: 16, idleTimeoutMs: 120, suppressAfterKeypressMs: 40,
  discardUnclassified: false, engageRatioPercent: 300, releaseRatioPercent: 180,
  activationDistanceMicrometers: 594, intentWindowMs: 64,
};

const RIGHT_TEXT_SETTINGS = {
  engageRatioPercent: 150, releaseRatioPercent: 100, activationDistanceMicrometers: 1300,
  intentWindowMs: 32, idleTimeoutMs: 40, discardUnclassified: true,
  horizontalThresholdMicrometers: 2785, verticalThresholdMicrometers: 2785,
  mapperIdleTimeoutMs: 40,
};

/* ---- Ground truth ------------------------------------------------------- */

/* The intended ball motion of a stroke, in sensor counts relative to its
 * start, sampled every intervalUs. Replay derives the ideal output from it
 * with whatever settings it runs, so the truth holds under --profile. */
function ballPath(stroke, cpi, intervalUs) {
  const countsPerMm = cpi / 25.4;
  const points = [];
  const steps = Math.ceil((stroke.duration * 1e6) / intervalUs);
  for (let i = 0; i <= steps; i += 1) {
    const [x, y] = stroke.position(Math.min((i * intervalUs) / 1e6, stroke.duration));
    points.push([round(x * countsPerMm, 3), round(y * countsPerMm, 3)]);
  }
  return { intervalUs, points };
}

const round = (value, digits) => Number(value.toFixed(digits));

/* ---- Strokes: ball position in millimetres over a duration in seconds -- */

function linear(speed, angle, duration) {
  return {
    duration,
    position: (t) => [speed * t * Math.cos(angle), speed * t * Math.sin(angle)],
  };
}

/* Minimum-jerk travel over length L; peak speed is 1.875 L / T. */
function minimumJerk(length, angle, duration) {
  return {
    duration,
    position: (t) => {
      const u = Math.min(Math.max(t / duration, 0), 1);
      const s = length * (10 * u ** 3 - 15 * u ** 4 + 6 * u ** 5);
      return [s * Math.cos(angle), s * Math.sin(angle)];
    },
  };
}

/* An arc of radius r, starting along angle, turning by sweep radians. */
function arc(speed, angle, sweep, duration) {
  const radius = (speed * duration) / Math.abs(sweep);
  const turn = Math.sign(sweep);
  return {
    duration,
    position: (t) => {
      const theta = (speed * t) / radius;
      const along = radius * Math.sin(theta);
      const across = turn * radius * (1 - Math.cos(theta));
      return [along * Math.cos(angle) - across * Math.sin(angle),
        along * Math.sin(angle) + across * Math.cos(angle)];
    },
  };
}

/* Physiological tremor: a few-hundredths-of-a-millimetre wobble near 9.5 Hz. */
function tremor(amplitude, duration, rand) {
  const fx = 8.5 + 2 * rand();
  const fy = 8.5 + 2 * rand();
  const px = 2 * Math.PI * rand();
  const py = 2 * Math.PI * rand();
  return {
    duration,
    position: (t) => [amplitude * (Math.sin(2 * Math.PI * fx * t + px) - Math.sin(px)),
      amplitude * 0.7 * (Math.sin(2 * Math.PI * fy * t + py) - Math.sin(py))],
  };
}

function withTremor(stroke, amplitude, rand) {
  const wobble = tremor(amplitude, stroke.duration, rand);
  return {
    duration: stroke.duration,
    position: (t) => {
      const [x, y] = stroke.position(t);
      const [wx, wy] = wobble.position(t);
      return [x + wx, y + wy];
    },
  };
}

/* A keypress jolts the ball: a damped ~25 Hz shake of a few tenths of a mm. */
function keypressShake(amplitude, angle, duration) {
  return {
    duration,
    position: (t) => {
      const s = amplitude * Math.exp(-t / 0.012) * Math.sin(2 * Math.PI * 25 * t);
      return [s * Math.cos(angle), s * Math.sin(angle)];
    },
  };
}

/* ---- Sensor model ------------------------------------------------------- */

const SAMPLE_US = 333;
const MIN_READ_GAP_US = 1000;
const DELIVERY_US = 100;

/*
 * Turns a timeline of ball segments into frames. The sensor samples the
 * quantized position every SAMPLE_US; a new count raises the motion
 * interrupt and a read follows 200-600 us later, at least MIN_READ_GAP_US
 * after the previous read, returning every count since that read. Frames
 * arrive DELIVERY_US after the read and carry the time since the previous
 * read as their sample span.
 */
class Trace {
  constructor(cpi, seed) {
    this.cpi = cpi;
    this.rand = random(seed);
    this.timeUs = 0;
    this.offset = [0, 0];
    this.reported = [0, 0];
    this.lastReadUs = -1e9;
    this.pendingReadUs = null;
    this.events = [];
    this.strokes = [];
  }

  counts([x, y]) {
    const scale = this.cpi / 25.4;
    return [Math.round((this.offset[0] + x) * scale), Math.round((this.offset[1] + y) * scale)];
  }

  read(readUs, position) {
    const [cx, cy] = this.counts(position);
    const dx = cx - this.reported[0];
    const dy = cy - this.reported[1];
    this.pendingReadUs = null;
    if (dx === 0 && dy === 0) return;
    const arrival = readUs + DELIVERY_US;
    this.events.push({ timeUs: arrival, type: "motion", x: dx, y: dy,
      spanUs: this.lastReadUs < 0 ? 0 : readUs - this.lastReadUs });
    this.reported = [cx, cy];
    this.lastReadUs = readUs;
  }

  /* Advance through one segment, then hold its final position. */
  segment(path, truth = null) {
    const startUs = this.timeUs;
    const endUs = startUs + Math.round(path.duration * 1e6);
    for (let t = startUs; t <= endUs; t += SAMPLE_US) {
      const position = path.position((t - startUs) / 1e6);
      const [cx, cy] = this.counts(position);
      if (this.pendingReadUs !== null && t >= this.pendingReadUs) {
        this.read(this.pendingReadUs, path.position((this.pendingReadUs - startUs) / 1e6));
      }
      if (this.pendingReadUs === null &&
          (cx !== this.reported[0] || cy !== this.reported[1])) {
        const delay = 200 + Math.floor(this.rand() * 400);
        this.pendingReadUs = Math.max(t + delay, this.lastReadUs + MIN_READ_GAP_US);
      }
    }
    if (this.pendingReadUs !== null) this.read(this.pendingReadUs, path.position(path.duration));
    const end = path.position(path.duration);
    this.offset = [this.offset[0] + end[0], this.offset[1] + end[1]];
    this.timeUs = Math.max(endUs, this.events.at(-1)?.timeUs ?? 0);
    if (truth) {
      this.strokes.push({ id: truth.id ?? `stroke-${this.strokes.length + 1}`,
        startUs, endUs: this.timeUs + 1, ...truth.fields });
    }
  }

  pause(seconds) {
    this.timeUs += Math.round(seconds * 1e6);
  }

  keypress() {
    this.events.push({ timeUs: this.timeUs, type: "keypress" });
  }

  encodedEvents() {
    const events = [...this.events].sort((a, b) => a.timeUs - b.timeUs);
    let previous = 0;
    return events.map((event) => {
      const delta = event.timeUs - previous;
      previous = event.timeUs;
      return event.type === "motion"
        ? ["motion", delta, event.x, event.y, event.spanUs]
        : ["keypress", delta];
    });
  }
}

/* ---- Fixtures ----------------------------------------------------------- */

function fixture({ id, description, mode, model, source, processor, trace }) {
  return {
    schema: "zmk-pointing-tools/trace-fixture",
    version: 2,
    id,
    description,
    metadata: {
      mode,
      synthetic: true,
      provenance: `Synthetic, generated by ${GENERATOR}: ${model}`,
    },
    source,
    processor,
    events: trace.encodedEvents(),
    truth: { strokes: trace.strokes },
  };
}

const RIGHT_BALL = (cpi) => ({ device: "right-ball", location: "central", cpi, span: "none" });
const LEFT_BALL = { device: "left-ball", location: "peripheral", cpi: 114, span: "none" };

function cursorProcessor(link = "usb", profile = undefined) {
  return { kind: "cursor-pipeline", link, stages: bridgesCursorStages(),
    ...(profile ? { profile } : {}) };
}

function slowAim({ id, cpi, speeds, seed, profile, description }) {
  const trace = new Trace(cpi, seed);
  const rand = random(seed + 1);
  trace.pause(0.05);
  speeds.forEach((speed, index) => {
    const angle = rand() * 2 * Math.PI;
    const stroke = linear(speed, angle, 0.8);
    trace.segment(stroke, { id: `slow-${index + 1}`, fields: {
      activity: "slow-aim", intended: true, speedMmPerS: speed,
      path: ballPath(stroke, cpi, 50000) } });
    trace.pause(0.3);
  });
  return fixture({ id, description, mode: "cursor", trace, source: RIGHT_BALL(cpi),
    model: `steady straight strokes at ${speeds.join(", ")} mm/s, ${cpi} CPI`,
    processor: cursorProcessor("usb", profile) });
}

function tremorFixture() {
  const cpi = 684;
  const trace = new Trace(cpi, 31);
  const rand = random(32);
  trace.pause(0.05);
  for (let index = 0; index < 4; index += 1) {
    trace.segment(tremor(0.03 + 0.02 * rand(), 0.6, rand),
      { id: `rest-${index + 1}`, fields: { activity: "tremor", intended: false } });
    trace.pause(0.2);
    const speed = 6 + 6 * index;
    const stroke = linear(speed, rand() * 2 * Math.PI, 0.8);
    trace.segment(withTremor(stroke, 0.03, rand), { id: `aim-${index + 1}`, fields: {
      activity: "slow-aim", intended: true, speedMmPerS: speed,
      path: ballPath(stroke, cpi, 50000) } });
    trace.pause(0.3);
  }
  return fixture({ id: "tremor", mode: "cursor", trace, source: RIGHT_BALL(cpi),
    description: "A resting hand's tremor, then slow aiming strokes with the same tremor on top.",
    model: "8.5-10.5 Hz tremor of 0.03-0.05 mm alone and over 6-24 mm/s strokes, 684 CPI",
    processor: cursorProcessor() });
}

function flicks({ id, link, seed }) {
  const cpi = 684;
  const trace = new Trace(cpi, seed);
  const rand = random(seed + 1);
  trace.pause(0.05);
  for (let index = 0; index < 6; index += 1) {
    const peak = 250 + 70 * index;
    const duration = 0.12 + 0.08 * rand();
    const length = (peak * duration) / 1.875;
    const stroke = minimumJerk(length, rand() * 2 * Math.PI, duration);
    trace.segment(stroke, { id: `flick-${index + 1}`, fields: {
      activity: "flick", intended: true, path: ballPath(stroke, cpi, 1000) } });
    trace.pause(0.25);
  }
  return fixture({ id, mode: "cursor", trace, source: RIGHT_BALL(cpi),
    description: `Fast flicks across the screen over ${link === "usb" ? "USB" : "Bluetooth"}.`,
    model: "minimum-jerk flicks peaking at 250-600 mm/s over 120-200 ms, 684 CPI",
    processor: cursorProcessor(link) });
}

/* Gaps from the last keypress to the start of the next stroke: inside, at,
 * and outside the 40 ms keypress guard. */
const POST_TYPING_GAPS_MS = [20, 40, 80, 150];

/* A burst of keypresses, each shaking the ball. The last shake is cut off
 * where the next stroke starts, gapMs after the last keypress. Returns the
 * burst's unintended stroke. */
function typingBurst(trace, rand, { keys, shakeMm, shakeS, gapMs, id }) {
  const startUs = trace.timeUs;
  for (let key = 0; key < keys; key += 1) {
    const last = key === keys - 1;
    trace.keypress();
    const keyUs = trace.timeUs;
    trace.segment(keypressShake(shakeMm[0] + (shakeMm[1] - shakeMm[0]) * rand(),
      rand() * 2 * Math.PI, last ? Math.min(shakeS, gapMs / 1000) : shakeS));
    if (last) {
      trace.timeUs = Math.max(trace.timeUs, keyUs + gapMs * 1000);
    } else {
      trace.pause(0.04 + 0.06 * rand());
    }
  }
  trace.strokes.push({ id, startUs, endUs: trace.timeUs, activity: "post-typing",
    intended: false });
}

function postTypingCursor() {
  const cpi = 684;
  const trace = new Trace(cpi, 51);
  const rand = random(52);
  trace.pause(0.05);
  POST_TYPING_GAPS_MS.forEach((gapMs, burst) => {
    typingBurst(trace, rand, { keys: 5, shakeMm: [0.15, 0.3], shakeS: 0.06, gapMs,
      id: `typing-${burst + 1}` });
    const speed = 20 + 15 * burst;
    const stroke = linear(speed, rand() * 2 * Math.PI, 0.5);
    trace.segment(stroke, { id: `reach-${burst + 1}`, fields: {
      activity: "post-typing", intended: true, speedMmPerS: speed,
      path: ballPath(stroke, cpi, 50000) } });
    trace.pause(0.4);
  });
  return fixture({ id: "post-typing-cursor", mode: "cursor", trace, source: RIGHT_BALL(cpi),
    description: "Typing shakes the ball, then the hand moves to the cursor 20, 40, 80, or 150 ms after the last key.",
    model: "damped 25 Hz shake of 0.15-0.3 mm per keypress, then 20-65 mm/s strokes " +
      "20-150 ms after the last key, 684 CPI",
    processor: cursorProcessor() });
}

function scrollProcessor() {
  return { kind: "composed-scroll", policy: "adaptive", settings: LEFT_SCROLL_SETTINGS };
}

const scrollSteps = (millimetres) => round((millimetres * LEFT_SCROLL_SETTINGS.stepsPerMeter) / 1000, 2);

function scrollStroke(trace, stroke, axis, id, extra = {}) {
  const [x, y] = stroke.position(stroke.duration);
  trace.segment(stroke, { id, fields: { activity: "scroll", intended: true, axis,
    intendedOutput: axis === "vertical" ? [0, scrollSteps(y)] : [scrollSteps(x), 0],
    ...extra } });
}

function scrollSlow() {
  const trace = new Trace(114, 61);
  trace.pause(0.05);
  [8, 12, 18, 25, 35].forEach((speed, index) => {
    const vertical = index % 2 === 0;
    const angle = (vertical ? Math.PI / 2 : 0) + (index % 3 === 1 ? Math.PI : 0) + 0.08;
    scrollStroke(trace, linear(speed, angle, 1.0), vertical ? "vertical" : "horizontal",
      `scroll-${index + 1}`, { speedMmPerS: speed });
    trace.pause(0.3);
  });
  return fixture({ id: "scroll-slow", mode: "scroll", trace, source: LEFT_BALL,
    description: "Slow, nearly straight scroll strokes on the left ball.",
    model: "8-35 mm/s strokes 5 degrees off axis, effective 114 CPI", processor: scrollProcessor() });
}

function scrollCurved() {
  const trace = new Trace(114, 71);
  trace.pause(0.05);
  [[40, 0.5], [30, -0.6], [50, 0.8], [25, -0.4], [45, 0.7]].forEach(([speed, sweep], index) => {
    const down = index % 2 === 0;
    scrollStroke(trace, arc(speed, down ? Math.PI / 2 : -Math.PI / 2, sweep, 0.8), "vertical",
      `arc-${index + 1}`);
    trace.pause(0.3);
  });
  return fixture({ id: "scroll-curved", mode: "scroll", trace, source: LEFT_BALL,
    description: "Vertical scroll strokes that curve sideways as the thumb rolls the ball.",
    model: "25-50 mm/s arcs turning 23-46 degrees, effective 114 CPI", processor: scrollProcessor() });
}

function scrollPostTyping() {
  const trace = new Trace(114, 81);
  const rand = random(82);
  trace.pause(0.05);
  POST_TYPING_GAPS_MS.forEach((gapMs, burst) => {
    typingBurst(trace, rand, { keys: 4, shakeMm: [0.4, 0.7], shakeS: 0.05, gapMs,
      id: `typing-${burst + 1}` });
    scrollStroke(trace, linear(20 + 10 * burst, Math.PI / 2 + 0.1, 0.6), "vertical",
      `scroll-${burst + 1}`);
    trace.pause(0.4);
  });
  return fixture({ id: "scroll-post-typing", mode: "scroll", trace, source: LEFT_BALL,
    description: "Typing shakes the left ball, then a scroll stroke starts 20, 40, 80, or 150 ms after the last key.",
    model: "damped 25 Hz shake of 0.4-0.7 mm per keypress, then 20-50 mm/s strokes " +
      "20-150 ms after the last key, effective 114 CPI",
    processor: scrollProcessor() });
}

function textStrokes() {
  const cpi = 684;
  const trace = new Trace(cpi, 91);
  const rand = random(92);
  trace.pause(0.05);
  for (let index = 0; index < 8; index += 1) {
    const horizontal = index % 2 === 0;
    const base = (horizontal ? 0 : Math.PI / 2) + (index % 4 >= 2 ? Math.PI : 0);
    const stroke = linear(35 + 20 * rand(), base + (rand() - 0.5) * 0.5, 0.4);
    const [x, y] = stroke.position(stroke.duration);
    const threshold = RIGHT_TEXT_SETTINGS.horizontalThresholdMicrometers / 1000;
    trace.segment(stroke, { id: `text-${index + 1}`, fields: {
      activity: "text", intended: true, axis: horizontal ? "horizontal" : "vertical",
      intendedOutput: horizontal ? [round(x / threshold, 2), 0] : [0, round(y / threshold, 2)] } });
    trace.pause(0.25);
  }
  return fixture({ id: "text-strokes", mode: "text", trace, source: RIGHT_BALL(cpi),
    description: "Text navigation strokes left, right, up, and down with up to 14 degrees of drift.",
    model: "35-55 mm/s strokes up to 14 degrees off axis, 684 CPI",
    processor: { kind: "composed-text", settings: RIGHT_TEXT_SETTINGS } });
}

/* Indented JSON with each event and other flat array on one line, so a
 * fixture diffs line by line. */
function format(value) {
  return `${JSON.stringify(value, null, 2)
    .replace(/\[\n\s+([^[\]{}]*?)\n\s+\]/g, (_, body) => `[${body.split(/,\n\s+/).join(", ")}]`)}\n`;
}

function main() {
  const precisionProfile = JSON.parse(readFileSync(join(ROOT, "profiles/precision.json"), "utf8"));
  const sets = {
    tune: [
      slowAim({ id: "slow-aim-bridges", cpi: 684, seed: 11,
        speeds: [4, 6, 8, 10, 12, 15],
        description: "Slow steady aiming strokes that arrive as sparse single counts." }),
      slowAim({ id: "slow-aim-precision", cpi: 684, seed: 21,
        speeds: [5, 8, 12, 16, 20, 30], profile: precisionProfile,
        description: "Slow aiming on the steep slow end of the Precision curve, the #113 case." }),
      tremorFixture(),
      flicks({ id: "flicks-usb", link: "usb", seed: 41 }),
      postTypingCursor(),
      scrollSlow(),
      scrollCurved(),
      textStrokes(),
    ],
    holdout: [
      slowAim({ id: "slow-aim-2052cpi", cpi: 2052, seed: 111,
        speeds: [5, 7, 9, 13, 18, 24],
        description: "Slow steady aiming strokes at 2052 CPI." }),
      flicks({ id: "flicks-ble", link: "ble", seed: 141 }),
      scrollPostTyping(),
    ],
  };
  for (const [set, fixtures] of Object.entries(sets)) {
    const directory = join(ROOT, "fixtures", set);
    mkdirSync(directory, { recursive: true });
    for (const generated of fixtures) {
      writeFileSync(join(directory, `${generated.id}.json`), format(generated));
    }
  }
}

main();
