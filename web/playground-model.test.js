import test from "node:test";
import assert from "node:assert/strict";

import {
  PRECISION_LAYOUT,
  PRECISION_SIZES,
  PRECISION_START,
  createRun,
  exportRun,
  finishPrecisionTrial,
  finishRun,
  observePrecisionPointer,
  precisionTrials,
  recordClick,
  recordPointer,
  recordWheel,
  startPrecisionTrial,
  summarizePrecision,
} from "./playground-model.js";

test("records repeatable activity metrics and exports only associated telemetry", () => {
  const run = createRun("cursor-targets", { nextSequence: 10, tuningProfile: { version: 1 } }, 1000);
  recordWheel(run, -2, 4);
  let previous = recordPointer(run, undefined, { x: 1, y: 1 });
  previous = recordPointer(run, previous, { x: 4, y: 5 });
  recordClick(run, false);
  recordClick(run, true);
  finishRun(run, true, 1250);

  const exported = exportRun(run, {
    stateEvents: [{ sequence: 8 }, { sequence: 12 }],
    queueCapacity: 64,
  });
  assert.equal(previous.x, 4);
  assert.equal(exported.durationMs, 250);
  assert.equal(exported.metrics.pointerDistance, 5);
  assert.equal(exported.metrics.clicks, 2);
  assert.equal(exported.metrics.errors, 1);
  assert.deepEqual(exported.telemetry.stateEvents.map(({ sequence }) => sequence), [12]);
  assert.equal("startedMs" in exported, false);
});

test("precision trials shrink block by block and mix long and short moves", () => {
  const trials = precisionTrials();
  assert.equal(trials.length, PRECISION_SIZES.length * 6);
  assert.deepEqual([...new Set(trials.map(({ size }) => size))], [...PRECISION_SIZES]);
  for (const { x, y, size } of trials) {
    assert.ok(x - size / 2 >= 0 && x + size / 2 <= PRECISION_LAYOUT.width);
    assert.ok(y - size / 2 >= 0 && y + size / 2 <= PRECISION_LAYOUT.height);
  }
  const distances = trials.slice(1, 6).map(({ distance }) => distance);
  assert.ok(Math.min(...distances) < 200 && Math.max(...distances) > 400);
  assert.equal(trials[0].distance, Math.hypot(80 - PRECISION_START.x, 60 - PRECISION_START.y));
});

test("a precision trial measures corrections, overshoot, and time to land", () => {
  const trial = { index: 0, size: 8, x: 100, y: 0, distance: 100 };
  const state = startPrecisionTrial(trial, { x: 0, y: 0 }, 1000);
  observePrecisionPointer(state, { x: 50, y: 0 }, 1100);
  assert.equal(observePrecisionPointer(state, { x: 98, y: 0 }, 1200), true);
  /* Sails 6 px past the far edge, comes back, and lands. */
  observePrecisionPointer(state, { x: 110, y: 0 }, 1250);
  assert.equal(observePrecisionPointer(state, { x: 101, y: 1 }, 1400), true);
  state.misses += 1;
  const hover = finishPrecisionTrial(state, 1800, true);
  assert.equal(hover.timeMs, 400);
  assert.equal(hover.corrections, 1);
  assert.equal(hover.overshootPx, 6);
  assert.equal(hover.misses, 1);
  assert.equal(hover.pathPx, 119);
  assert.equal(finishPrecisionTrial(state, 1800, false).timeMs, 800);
});

test("precision summaries report per-size medians and Fitts throughput", () => {
  const results = [
    { size: 32, distance: 96, timeMs: 500, corrections: 0, overshootPx: 0, misses: 0 },
    { size: 32, distance: 96, timeMs: 1000, corrections: 1, overshootPx: 4, misses: 0 },
    { size: 4, distance: 124, timeMs: 2500, corrections: 3, overshootPx: 10, misses: 2 },
  ];
  const summary = summarizePrecision(results);
  assert.equal(summary.trials, 3);
  assert.equal(summary.totalTimeMs, 4000);
  assert.deepEqual(summary.sizes.map(({ size }) => size), [32, 4]);
  const [large, small] = summary.sizes;
  assert.equal(large.medianTimeMs, 750);
  assert.equal(large.corrections, 1);
  assert.equal(large.medianOvershootPx, 2);
  /* log2(96 / 32 + 1) = 2 bits over 0.5 s and 1 s. */
  assert.equal(large.throughput, 3);
  assert.equal(small.misses, 2);
  assert.equal(small.throughput, 2);
});
