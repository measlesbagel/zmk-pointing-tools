import test from "node:test";
import assert from "node:assert/strict";

import {
  SIGNAL_FLAG,
  SIGNAL_KIND,
  STAGE_FRAME,
  accelerationConfig,
  accelerationTargets,
  analyzeCalibration,
  calibrationTargets,
  countSequenceGaps,
  createAccelerationModel,
  createCalibrationExport,
  matchAccelerationTarget,
  parseStageFrame,
  rationalGain,
  segmentGestures,
} from "./calibration-model.js";
import { SHAPE_SPEEDS, sensitivityFunction } from "./acceleration-curve.js";
import { prepareProfileImport } from "./profile.js";
import { STATE, TARGET_KIND } from "./protocol.js";

const Q16 = 65536;

function frame(timestamp, xMm, yMm, extra = {}) {
  return {
    targetId: 0,
    timestamp,
    kind: SIGNAL_KIND.NORMALIZED_MOTION,
    x: Math.round(xMm * Q16),
    y: Math.round(yMm * Q16),
    cpi: 800,
    spanUs: 8000,
    sequence: 0,
    flags: 0,
    sourceId: 0,
    speed: 0,
    ...extra,
  };
}

/* Repeated gestures of whole-millimetre frames, 8 ms apart, separated by
 * 400 ms pauses. */
function gestures(count, frames, xMm, yMm, extra = {}) {
  const recorded = [];
  let timestamp = 1000;
  for (let gesture = 0; gesture < count; gesture += 1) {
    for (let index = 0; index < frames; index += 1) {
      recorded.push(frame(timestamp, xMm, yMm, extra));
      timestamp += 8;
    }
    timestamp += 400;
  }
  return recorded;
}

function parameter(id, key, current, minimum = 1, maximum = 1000) {
  return { id, key, devicetreeProperty: key, current, compiled: current, minimum, maximum, step: 1, type: 0 };
}

const transfer = {
  id: 3,
  kind: 4,
  label: "left-cursor-transfer",
  stableId: "left-cursor-transfer",
  devicetreePath: "/zpt_left_transfer",
  parameters: [parameter(0, "scale-multiplier", 1, 0), parameter(1, "scale-divisor", 1)],
};
const scroll = {
  id: 7,
  kind: 4,
  label: "scroll-batcher",
  stableId: "scroll-batcher",
  devicetreePath: "/zpt_scroll_batcher",
  parameters: [parameter(0, "steps-per-meter", 3445, 1, 100000), parameter(1, "report-interval", 16)],
};
const text = {
  id: 11,
  kind: 4,
  label: "text-nav",
  stableId: "text-nav",
  devicetreePath: "/zpt_text_nav",
  parameters: [
    parameter(0, "horizontal-threshold", 2721, 1, 50000),
    parameter(1, "vertical-threshold", 2721, 1, 50000),
    parameter(2, "idle-timeout", 300, 1, 5000),
  ],
};
/* The default trackball S-curve. */
const S_CURVE = [65, 66, 73, 99, 159, 225, 264, 283];

function acceleration(stableId, percents) {
  return {
    id: 2,
    kind: 4,
    label: stableId,
    stableId,
    devicetreePath: `/${stableId}`,
    parameters: [
      ...SHAPE_SPEEDS.map((speed, i) => parameter(i, `curve-speed-${i + 1}`, speed, 0, 10000)),
      ...percents.map((percent, i) => parameter(8 + i, `curve-sensitivity-${i + 1}`, percent)),
      parameter(16, "speed-rise", 10, 1, 200),
      parameter(17, "speed-fall", 5, 1, 200),
    ],
  };
}

test("decodes every-frame stage samples and ignores decisions", () => {
  const sample = {
    targetId: 4,
    targetKind: TARGET_KIND.PIPELINE_STAGE,
    event: STATE.EVENT_FRAME,
    intent: 2,
    flags: 0,
    timestamp: 1234,
    sequence: 9,
    values: [STAGE_FRAME, SIGNAL_KIND.NORMALIZED_MOTION, Q16, -Q16, 684, 8000, 17, SIGNAL_FLAG.COALESCED, 1, 0],
  };
  assert.deepEqual(parseStageFrame(sample), {
    targetId: 4,
    timestamp: 1234,
    stateSequence: 9,
    intent: 2,
    kind: SIGNAL_KIND.NORMALIZED_MOTION,
    x: Q16,
    y: -Q16,
    cpi: 684,
    spanUs: 8000,
    sequence: 17,
    flags: SIGNAL_FLAG.COALESCED,
    sourceId: 1,
    speed: 0,
  });
  assert.equal(parseStageFrame({ ...sample, values: [3, 2, 0, 0, 0, 0, 0, 0, 0, 0] }), undefined);
  assert.equal(parseStageFrame({ ...sample, event: STATE.EVENT_FLUSH }), undefined);
});

test("segments gestures at pauses and source changes, skipping still frames", () => {
  const frames = [
    frame(0, 1, 0),
    frame(8, 0, 0),
    frame(16, 1, 0),
    frame(200, 1, 0),
    frame(208, 1, 0, { sourceId: 1 }),
  ];
  assert.deepEqual(segmentGestures(frames).map((gesture) => gesture.length), [2, 1, 1]);
  /* Timestamps are u32 milliseconds and may wrap mid-gesture. */
  assert.equal(segmentGestures([frame(0xfffffffc, 1, 0), frame(4, 1, 0)]).length, 1);
});

test("recommends scroll steps per metre from the main axis distance", () => {
  const analysis = analyzeCalibration(gestures(5, 20, 0.05, 1), {
    mode: "scroll",
    desiredOutput: 10,
    target: scroll,
  });
  assert.equal(analysis.summary.gestureCount, 5);
  assert.equal(analysis.summary.axis, "y");
  assert.equal(analysis.summary.distanceMm.median, 20);
  assert.equal(analysis.summary.cpi, 800);
  assert.equal(analysis.summary.intervalMs, 8);
  assert.equal(analysis.summary.durationMs, 160);
  assert.deepEqual(analysis.recommendation.parameters, [{ key: "steps-per-meter", value: 500 }]);
  assert.equal(analysis.confidence.level, "high");
});

test("centres text steps between N and N+1 taps on the gesture axis", () => {
  const analysis = analyzeCalibration(gestures(5, 11, -1, 0), { mode: "text", desiredOutput: 5, target: text });
  assert.deepEqual(analysis.recommendation.parameters, [{ key: "horizontal-threshold", value: 2000 }]);
});

test("recommends cursor gain from CPI and the modelled acceleration", () => {
  /* 25.4 mm at 800 CPI is 800 pointer units at unity gain. */
  const recording = gestures(5, 20, 1.27, 0);
  const flat = analyzeCalibration(recording, { mode: "cursor", desiredOutput: 1600, target: transfer });
  assert.deepEqual(flat.recommendation.parameters, [
    { key: "scale-multiplier", value: 2 },
    { key: "scale-divisor", value: 1 },
  ]);

  const halved = analyzeCalibration(recording, {
    mode: "cursor",
    desiredOutput: 1600,
    target: transfer,
    acceleration: accelerationConfig(acceleration("left-cursor-acceleration", new Array(8).fill(50))),
  });
  assert.deepEqual(halved.recommendation.parameters.map(({ value }) => value), [4, 1]);

  /* 1.27 mm every 8 ms is 158.75 mm/s. The estimate settles there within
   * the first gesture, so the median gesture is scaled by the curve's
   * sensitivity at that speed. */
  const curved = analyzeCalibration(recording, {
    mode: "cursor",
    desiredOutput: 1600,
    target: transfer,
    acceleration: accelerationConfig(acceleration("left-cursor-acceleration", S_CURVE)),
  });
  const ratio = curved.summary.distanceMm.median / flat.summary.distanceMm.median;
  const expected = sensitivityFunction(SHAPE_SPEEDS.map((speed, i) => ({ speed, percent: S_CURVE[i] })))(158.75) / 100;
  assert.ok(Math.abs(ratio - expected) < 0.005, `ratio ${ratio}, expected ${expected}`);
});

test("models the acceleration stage's speed estimate", () => {
  const config = accelerationConfig(acceleration("cursor-acceleration", S_CURVE));
  assert.deepEqual(config.points[3], { speed: 45, percent: 99 });
  assert.equal(config.speedRiseMs, 10);
  assert.equal(config.speedFallMs, 5);

  /* The S-curve rises strictly, so each gain pins down one speed. */
  const curve = sensitivityFunction(config.points);
  const gainAt = (speed) => curve(speed) / 100;
  const close = (actual, speed) => assert.ok(Math.abs(actual - gainAt(speed)) < 1e-9, `${actual} vs ${speed} mm/s`);
  const step = (model, mm, extra) => model(mm, 0, frame(extra.timestamp, mm, 0, extra));

  /* One 10 ms span covering one rise time closes 1 - 1/e of the gap to
   * 100 mm/s, as the firmware test of the same name expects. */
  let model = createAccelerationModel(config);
  close(step(model, 1, { timestamp: 10, spanUs: 10000 }), 100 * (1 - Math.exp(-1)));

  /* Without spans the first frame restarts over the 40 ms interval, and a
   * pause restarts again instead of carrying the old speed. */
  model = createAccelerationModel(config);
  close(step(model, 2, { timestamp: 0, spanUs: 0 }), 50);
  assert.ok(step(model, 2, { timestamp: 8, spanUs: 0 }) > gainAt(50));
  close(step(model, 1, { timestamp: 400, spanUs: 0 }), 25);

  /* A clipped span is a restart even when a span is present. */
  close(step(model, 4, { timestamp: 404, spanUs: 30000, flags: SIGNAL_FLAG.SAMPLE_SPAN_CLIPPED }), 100);
});

test("analysis is reproducible from the recording alone", () => {
  const recording = gestures(6, 15, 1, 0.2);
  const options = { mode: "cursor", desiredOutput: 900, target: transfer };
  assert.deepEqual(analyzeCalibration(recording, options), analyzeCalibration(recording, options));
});

test("explains limited confidence from drops, gaps, reversals, and too few gestures", () => {
  const recording = gestures(5, 20, 1, 0);
  recording[5] = { ...recording[5], flags: SIGNAL_FLAG.SEQUENCE_GAP };
  recording[30] = { ...recording[30], x: -3 * Q16 };
  for (let index = 0; index < recording.length; index += 1) recording[index].sequence = index + (index > 50 ? 2 : 0);
  const analysis = analyzeCalibration(recording, {
    mode: "scroll",
    desiredOutput: 10,
    target: scroll,
    telemetryDropped: 3,
  });
  assert.equal(analysis.confidence.level, "low");
  assert.equal(analysis.summary.transportGapFrames, 1);
  assert.equal(analysis.summary.missingFrames, 2);
  assert.equal(analysis.summary.reversedFrames, 1);
  assert.ok(analysis.confidence.limitations.some((line) => line.includes("telemetry samples were dropped")));
  assert.ok(analysis.confidence.limitations.some((line) => line.includes("lost frames")));

  const short = analyzeCalibration(gestures(2, 20, 1, 0), { mode: "scroll", desiredOutput: 10 });
  assert.equal(short.confidence.level, "insufficient");
  assert.deepEqual(short.recommendation.parameters, []);
});

test("calibrates the busiest source and ignores the other ball", () => {
  const recording = [...gestures(5, 20, 0, 1), ...gestures(1, 5, 0, 1, { sourceId: 1 })];
  const analysis = analyzeCalibration(recording, { mode: "scroll", desiredOutput: 10, target: scroll });
  assert.equal(analysis.summary.sourceId, 0);
  assert.equal(analysis.summary.ignoredGestures, 1);
  assert.equal(analysis.summary.gestureCount, 5);
});

test("finds calibratable targets and the matching acceleration stage", () => {
  const left = acceleration("left-cursor-acceleration", S_CURVE);
  const right = acceleration("right-cursor-acceleration", S_CURVE);
  const targets = new Map([[3, transfer], [7, scroll], [11, text], [2, left], [12, right]]);
  assert.deepEqual(calibrationTargets(targets).map(({ mode, target }) => `${mode}:${target.stableId}`), [
    "cursor:left-cursor-transfer",
    "scroll:scroll-batcher",
    "text:text-nav",
  ]);
  assert.equal(matchAccelerationTarget(transfer, accelerationTargets(targets)), left);
});

test("approximates gains with small multiplier and divisor pairs", () => {
  assert.deepEqual(rationalGain(1.5), { multiplier: 3, divisor: 2, error: 0 });
  const fraction = rationalGain(0.37);
  assert.ok(fraction.error <= 0.001 && fraction.divisor < 100);
  const pi = rationalGain(Math.PI);
  assert.ok(pi.error <= 0.001);
  assert.equal(rationalGain(0.0001), undefined);
});

test("counts state samples lost between received sequences", () => {
  assert.equal(countSequenceGaps([{ sequence: 1 }, { sequence: 2 }, { sequence: 5 }, { sequence: 6 }]), 2);
  assert.equal(countSequenceGaps([{ sequence: 0xffffffff }, { sequence: 0 }]), 0);
});

test("exports an importable profile with the calibration evidence", () => {
  const recording = gestures(5, 20, 0, 1);
  const analysis = analyzeCalibration(recording, { mode: "scroll", desiredOutput: 10, target: scroll });
  const exported = createCalibrationExport(analysis, {
    target: scroll,
    measureLabel: "scroll-resolution-normalize",
    frames: recording,
  }, "2026-01-01T00:00:00.000Z");
  assert.equal(exported.calibration.summary.gestureCount, 5);
  assert.equal(exported.calibration.frames.length, 100);
  assert.deepEqual(exported.targets[0].parameters, [
    { key: "steps-per-meter", devicetreeProperty: "steps-per-meter", value: 500, compiled: 3445 },
  ]);
  assert.deepEqual(prepareProfileImport(exported, new Map([[7, scroll]])), [
    { targetId: 7, stableId: "scroll-batcher", values: [{ parameterId: 0, value: 500 }] },
  ]);
});
