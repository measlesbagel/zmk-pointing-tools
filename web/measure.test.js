import test from "node:test";
import assert from "node:assert/strict";

import {
  bootstrapDifference,
  direction,
  directionBin,
  measureTrial,
  overshootRatio,
  rerolls,
  sequenceThroughput,
  signedEndpointError,
  submovements,
  summarizeBy,
  targetReEntries,
  throughput,
} from "./measure.js";
import { clearHistory, formatRun, loadHistory, saveRun } from "./game-history.js";

/* A straight movement right from (0,0) to a target at (200,0), sampled every
 * 8 ms, that ends at `endX`, optionally passing through `via` first. */
function trial({ endX = 200, via = [], width = 20, duration = 400, endY = 0 } = {}) {
  const path = [{ x: 0, y: 0 }, ...via, { x: endX, y: endY }];
  const samples = [];
  const steps = 50;
  for (let leg = 1; leg < path.length; leg += 1) {
    for (let i = 0; i <= steps; i += 1) {
      const f = i / steps;
      samples.push({
        x: path[leg - 1].x + (path[leg].x - path[leg - 1].x) * f,
        y: path[leg - 1].y + (path[leg].y - path[leg - 1].y) * f,
      });
    }
  }
  samples.forEach((sample, i) => { sample.t = (i * duration) / (samples.length - 1); });
  return {
    start: { x: 0, y: 0 }, target: { x: 200, y: 0 }, width,
    startedAt: 0, endedAt: duration, endpoint: { x: endX, y: endY }, samples,
  };
}

test("directions use screen up as 90 degrees", () => {
  assert.equal(direction(trial()), 0);
  const up = { start: { x: 0, y: 0 }, target: { x: 0, y: -100 } };
  assert.equal(direction(up), 90);
  assert.equal(directionBin(10), 0);
  assert.equal(directionBin(350), 0);
  assert.equal(directionBin(90), 1);
  assert.equal(directionBin(200), 2);
  assert.equal(directionBin(270, 8), 6);
});

test("signed error and overshoot tell too far from too short", () => {
  assert.equal(signedEndpointError(trial({ endX: 205 })), 5);
  assert.equal(signedEndpointError(trial({ endX: 194 })), -6);
  /* Went to 260, came back: overshoot past the far edge (210) of 50 px. */
  const corrected = trial({ endX: 200, via: [{ x: 260, y: 0 }] });
  assert.ok(Math.abs(overshootRatio(corrected) - 50 / 210) < 1e-9);
  assert.equal(overshootRatio(trial()), 0);
});

test("re-entries count every return into the target after the first", () => {
  assert.equal(targetReEntries(trial()), 0);
  assert.equal(targetReEntries(trial({ via: [{ x: 260, y: 0 }] })), 1);
  assert.equal(targetReEntries(trial({ via: [{ x: 260, y: 0 }, { x: 150, y: 0 }] })), 2);
});

test("a correction adds a submovement and a pause counts as a re-roll", () => {
  const clean = trial();
  /* Constant speed: one movement. */
  assert.equal(submovements(clean), 1);
  /* Speed up and down twice: a main movement and a correction. */
  const profile = [0.2, 0.8, 1.6, 0.8, 0.1, 0.05, 0.3, 0.6, 0.3, 0.05];
  let x = 0;
  const corrected = {
    ...clean,
    samples: [{ t: 0, x: 0, y: 0 }, ...profile.map((speed, i) => ({ t: (i + 1) * 10, x: (x += speed * 10), y: 0 }))],
  };
  assert.equal(submovements(corrected), 2);

  const paused = trial();
  paused.samples = paused.samples.map((s, i) => ({ ...s, t: s.t + (i > 25 ? 60 : 0) }));
  assert.equal(rerolls(paused), 1);
  assert.equal(rerolls(trial()), 0);
});

test("throughput follows the ISO effective-width formula", () => {
  /* Endpoints 195, 200, 205, 200: SD along the axis = sqrt(50/3). */
  const sequence = [195, 200, 205, 200].map((endX) => trial({ endX, duration: 500 }));
  const effectiveWidth = 4.133 * Math.sqrt(50 / 3);
  const expected = Math.log2(200 / effectiveWidth + 1) / 0.5;
  assert.ok(Math.abs(sequenceThroughput(sequence) - expected) < 1e-9);
  /* Mean of per-sequence values, skipping sequences too short to score. */
  assert.ok(Math.abs(throughput([sequence, [trial()]]) - expected) < 1e-9);
  assert.ok(Number.isNaN(throughput([[trial()]])));
});

test("measures group by direction and band", () => {
  const measured = [trial({ endX: 205 }), trial({ endX: 195 }), trial({ endX: 260 })].map(measureTrial);
  assert.equal(measured[2].hit, false);
  const byDirection = summarizeBy(measured, (item) => directionBin(item.direction));
  assert.equal(byDirection.get(0).trials, 3);
  assert.ok(Math.abs(byDirection.get(0).hitRate - 2 / 3) < 1e-9);
  assert.equal(byDirection.get(0).signedError, 5);
});

test("bootstrap reports a difference with an interval that excludes zero when clear", () => {
  const a = [3.0, 3.1, 2.9, 3.05, 2.95, 3.0, 3.1, 2.9];
  const b = a.map((value) => value + 0.5);
  const result = bootstrapDifference(a, b);
  assert.ok(Math.abs(result.difference - 0.5) < 1e-9);
  assert.ok(result.low > 0 && result.high < 1);
  assert.equal(result.probabilityBAhead, 1);

  const same = bootstrapDifference(a, a);
  assert.ok(same.low <= 0 && same.high >= 0);
  assert.equal(bootstrapDifference([], b), undefined);
});

test("history survives in storage and formats as labelled text", () => {
  const store = new Map();
  const storage = { getItem: (key) => store.get(key) ?? null, setItem: (key, value) => store.set(key, value) };
  const run = {
    game: "throughput-ring", title: "Throughput ring", finishedAt: "2026-11-09T00:00:00.000Z",
    throughput: 3.04, summary: measureTrial(trial()) && { trials: 1, hitRate: 1, movementTime: 400,
      signedError: 0, reEntries: 0, overshoot: 0, submovements: 1, rerolls: 0 },
    settings: [{ target: "trackball-cursor", key: "scale-multiplier", value: 3 }],
  };
  assert.equal(saveRun(run, storage), true);
  assert.equal(loadHistory("throughput-ring", storage).length, 1);
  assert.equal(loadHistory("other", storage).length, 0);
  const text = formatRun(run);
  assert.match(text, /^Game: Throughput ring \(throughput-ring\)/);
  assert.match(text, /Throughput: 3\.04 bit\/s/);
  assert.match(text, /trackball-cursor \/ scale-multiplier = 3/);
  clearHistory("throughput-ring", storage);
  assert.equal(loadHistory(undefined, storage).length, 0);
  /* Broken storage never throws. */
  assert.deepEqual(loadHistory("x", { getItem() { throw new Error("blocked"); } }), []);
});
