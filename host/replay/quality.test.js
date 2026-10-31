import test from "node:test";
import assert from "node:assert/strict";

import { aggregate, idealCursorOutput, scoreFixture } from "./quality.js";

const source = { device: "ball", location: "central", cpi: 254, span: "none" };

function cursorFixture(events, strokes) {
  return { version: 2, id: "q", source, processor: { kind: "cursor-pipeline" },
    events, truth: { strokes } };
}

/* D lines: t_ms x y rx ry clipped t_us speed gain; O lines: t_ms x y t_us. */
const D = (tUs, x, y, speedMm, gain = 1) =>
  `D\t${Math.floor(tUs / 1000)}\t${x}\t${y}\t0\t0\t0\t${tUs}\t${speedMm * 65536}\t${gain * 65536}`;
const O = (tUs, x, y) => `O\t${Math.floor(tUs / 1000)}\t${x}\t${y}\t${tUs}`;

test("speed error, output error, and stepping follow their definitions", () => {
  /* Frames at 0, 50, 150, 200 ms. Only those from 100 ms on count for
   * speed error: estimates 12 and 6 against a true 10 mm/s give errors of
   * +20 % and -40 %, RMS sqrt((0.04 + 0.16) / 2) = 31.62 %. The earlier
   * estimates of 50 and 80 would push it far higher. */
  const fixture = cursorFixture(
    [["motion", 0, 1, 0], ["motion", 50000, 1, 0], ["motion", 100000, 1, 0],
      ["motion", 50000, 1, 0]],
    [{ id: "a", activity: "aim", intended: true, startUs: 0, endUs: 250000,
      speedMmPerS: 10, intendedOutput: [30, 40] }]);
  /* Report output (24, 18) against the intended (30, 40): the vector error
   * is |(-6, -22)| = 22.80, 45.61 % of 50; the magnitude falls 40 % short. */
  const output = [D(0, 1, 0, 50, 3), O(0, 24, 18), D(50000, 1, 0, 80, 3),
    D(150000, 1, 0, 12, 1.5), D(200000, 1, 0, 6, 0.5)].join("\n");
  const [stroke] = scoreFixture(fixture, output);
  assert.equal(Number(stroke.metrics.speedErrorPercent.toFixed(2)), 31.62);
  assert.equal(Number(stroke.metrics.outputErrorPercent.toFixed(2)), 45.61);
  assert.equal(Number(stroke.metrics.distanceBiasPercent.toFixed(6)), -40);
  /* Gains 1.5 then 0.5 after settling: one step of 1 over a mean of 1. */
  assert.equal(stroke.metrics.sensitivityStepPercent, 100);
});

test("flick lag compares input and output progress", () => {
  /* Input reaches 10/50/90 % at 1, 3, 4 ms; output at 3, 6, 8 ms. */
  const fixture = cursorFixture(
    [["motion", 1000, 1, 0], ["motion", 1000, 2, 0], ["motion", 1000, 2, 0],
      ["motion", 1000, 4, 0], ["motion", 1000, 1, 0]],
    [{ id: "f", activity: "flick", intended: true, startUs: 0, endUs: 6000 }]);
  const output = [1, 2, 3, 4, 5].map((ms) => D(ms * 1000, 0, 0, 0))
    .concat([O(3000, -1, 0), O(6000, -4, 0), O(8000, -5, 0)]).join("\n");
  const [stroke] = scoreFixture(fixture, output);
  /* Lags of 2, 3, and 4 ms. */
  assert.equal(stroke.metrics.flickLagMs, 3);
});

test("axis metrics count decided frames and sideways steps", () => {
  const fixture = { version: 2, id: "s", source, processor: { kind: "composed-scroll" },
    events: [0, 1, 2, 3, 4, 5].map((i) => ["motion", i ? 8000 : 0, 0, 3]),
    truth: { strokes: [{ id: "v", activity: "scroll", intended: false, axis: "vertical",
      startUs: 0, endUs: 50000 }] } };
  const intents = [0, 3, 3, 1, 3, 3];
  const decisions = intents.map((intent, i) =>
    `D\t${8 * i}\t${intent}\t0\t0\t0\t0\t0\t0\t0\t0\t0\t0`);
  const output = [...decisions, "O\t16\t1\t3", "O\t32\t0\t4"].join("\n");
  const [stroke] = scoreFixture(fixture, output);
  /* Five decided frames, four vertical; switches 3->1 and 1->3. */
  assert.equal(stroke.metrics.axisLockPercent, 80);
  assert.equal(stroke.metrics.axisSwitches, 2);
  assert.equal(stroke.metrics.sidewaysLeakPercent, 12.5);
  assert.equal(stroke.metrics.unintendedOutput, 8);
  assert.deepEqual(aggregate([stroke, { metrics: { axisSwitches: 0 } }]).axisSwitches,
    { value: 1, strokes: 2 });
});

test("ideal cursor output follows the replayed curve and gain but not orientation", () => {
  const profile = (stableId, key, value) => ({ schema: "zmk-pointing-tools/tuning-profile",
    version: 1, targets: [{ stableId, parameters: [{ key, value }] }] });
  /* 50 % up to 10 mm/s, 100 % from 11 mm/s. */
  const curve = {};
  [0, 5, 10, 11, 12, 13, 14, 15].forEach((speed, i) => { curve[`curve-speed-${i + 1}`] = speed; });
  [50, 50, 50, 100, 100, 100, 100, 100].forEach((percent, i) => {
    curve[`curve-sensitivity-${i + 1}`] = percent;
  });
  const fixture = { version: 2, source: { cpi: 254 }, processor: { kind: "cursor-pipeline",
    stages: [
      { type: "orientation", stableId: "o", settings: { "invert-x": 1 } },
      { type: "acceleration", stableId: "a", settings: curve },
      { type: "transfer", stableId: "t", settings: { "scale-multiplier": 1, "scale-divisor": 1 } },
    ] } };
  /* 10 counts per mm. Steps of 1 mm and 1.118 mm per 100 ms: 10 mm/s at
   * 50 % and 11.18 mm/s at 100 %, so (10, 0) * 0.5 + (10, 5) * 1, with x
   * inverted. Judging both steps at the stroke's average speed would not. */
  const stroke = { path: { intervalUs: 100000, points: [[0, 0], [10, 0], [20, 5]] } };
  const close = (actual, expected) => assert.deepEqual(actual.map((v) => Number(v.toFixed(6))),
    expected);
  close(idealCursorOutput(fixture, stroke), [-15, 5]);
  close(idealCursorOutput(fixture, stroke, profile("t", "scale-divisor", 2)), [-7.5, 2.5]);
  close(idealCursorOutput(fixture, stroke, profile("a", "curve-sensitivity-3", 100)), [-20, 5]);
  /* Mounting is truth: a profile that drops the inversion still expects -x. */
  close(idealCursorOutput(fixture, stroke, profile("o", "invert-x", 0)), [-15, 5]);
});
