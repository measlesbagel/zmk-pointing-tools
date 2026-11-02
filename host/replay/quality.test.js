import test from "node:test";
import assert from "node:assert/strict";

import { aggregate, fitScreenScale, idealCursorOutput, scoreFixture } from "./quality.js";

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
  /* 20000 steps per metre at 254 CPI is 2 steps per count. */
  const fixture = { version: 2, id: "s", source,
    processor: { kind: "composed-scroll", settings: { stepsPerMeter: 20000 } },
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
  /* Eight output steps are four ball counts at 2 steps per count. */
  assert.ok(Math.abs(stroke.metrics.unintendedOutput - 4) < 1e-9);
  /* Against the vertical label: the one sideways step is half a count,
   * 0.05 mm; vertical locks at the second frame, 8 ms in, and the first
   * vertical step comes out at 16 ms. */
  assert.ok(Math.abs(stroke.metrics.wrongAxisOutputMm - 0.05) < 1e-9);
  assert.equal(stroke.metrics.axisLockDelayMs, 8);
  assert.equal(stroke.metrics.intendedOutputDelayMs, 16);
  /* A two-axis label wants free intent, one frame in five, and has no
   * sideways. */
  fixture.truth.strokes[0].axis = "both";
  const [both] = scoreFixture(fixture, output);
  assert.equal(both.metrics.axisLockPercent, 20);
  assert.equal(both.metrics.axisLockDelayMs, 24);
  assert.equal(both.metrics.sidewaysLeakPercent, undefined);
  assert.equal(both.metrics.wrongAxisOutputMm, undefined);
  assert.deepEqual(aggregate([stroke, { metrics: { axisSwitches: 0 } }]).axisSwitches,
    { value: 1, strokes: 2 });
});

test("axis output is judged in the pipeline's orientation, on a text stroke's main axis", () => {
  /* The ball moves 30 counts to -x and 5 to +y; an inverted x mounting
   * makes that rightward. At 254 CPI a 1000 um threshold is 10 counts per
   * step, so the ideal is three steps right and none up or down. */
  const fixture = { version: 2, id: "t", source, processor: { kind: "text-pipeline", stages: [
    { type: "orientation", stableId: "o", settings: { "swap-xy": 0, "invert-x": 1, "invert-y": 0 } },
    { type: "text-nav", stableId: "n", settings: { "horizontal-threshold": 1000,
      "vertical-threshold": 1000, "idle-timeout": 30 } },
  ] }, events: [0, 1, 2].map((i) => ["motion", i ? 10000 : 0, -10, i === 1 ? 5 : 0]),
  truth: { strokes: [{ id: "s", activity: "text", intended: true, startUs: 0, endUs: 30000,
    path: { intervalUs: 10000, points: [[0, 0], [-10, 0], [-20, 5], [-30, 5]] } }] } };
  const decisions = [0, 1, 2].map((i) => `D\t${10 * i}\t0\t0\t2\t0\t${10000 * i}`);
  const score = (direction) => scoreFixture(fixture, [...decisions,
    ...[0, 1, 2].map((i) => `O\t${10 * i}\t${direction}\t${10000 * i}`)].join("\n"))[0].metrics;
  /* Three steps right are exact; three left are twice the intended length off. */
  assert.ok(Math.abs(score(1).outputErrorPercent) < 1e-9);
  assert.ok(Math.abs(score(0).outputErrorPercent - 200) < 1e-9);
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

test("zero output scores as lost and unintended output reads in ball counts", () => {
  const fixture = { version: 2, id: "z", source: { device: "b", location: "central", cpi: 254,
    span: "none" }, processor: { kind: "cursor-pipeline", stages: [
    { type: "transfer", stableId: "t", settings: { "scale-multiplier": 1, "scale-divisor": 4 } },
  ] }, events: [["motion", 0, 1, 0], ["motion", 50000, 1, 0]], truth: { strokes: [
    { id: "aim", activity: "aim", intended: true, startUs: 0, endUs: 40000,
      path: { intervalUs: 10000, points: [[0, 0], [8, 0]] } },
    { id: "rest", activity: "tremor", intended: false, startUs: 50000, endUs: 60000 },
  ] } };
  /* No reports during the stroke: every count of the path is lost. */
  const output = [D(0, 1, 0, 0), D(50000, 1, 0, 0), O(50000, 3, 4)].join("\n");
  const [aim, rest] = scoreFixture(fixture, output);
  assert.equal(aim.metrics.outputErrorPercent, 100);
  /* A 5-unit report at a quarter gain is 20 ball counts. */
  assert.equal(rest.metrics.unintendedOutput, 20);
  /* With a zero gain the ideal output is zero too, which still counts as lost. */
  const zero = { schema: "zmk-pointing-tools/tuning-profile", version: 1, targets: [
    { stableId: "t", parameters: [{ key: "scale-multiplier", value: 0 }] }] };
  const [aimZero, restZero] = scoreFixture(fixture, output, zero);
  assert.equal(aimZero.metrics.outputErrorPercent, 100);
  assert.equal(restZero.metrics.unintendedOutput, undefined);
});

/* A guided cursor run's output every millisecond for two seconds, and the
 * browser pointer sampled every 4 ms by a computer that maps output to
 * pixels with move(t, report): known scale, known clock offset. */
function guidedRun(move, offsetUs) {
  const reports = [];
  let carry = [0, 0];
  for (let ms = 0; ms < 2000; ms += 1) {
    /* Speeds from rest to about 6 counts per millisecond. */
    const speed = 3 + 3 * Math.sin(ms / 140);
    carry = [carry[0] + speed * Math.cos(ms / 300), carry[1] + speed * Math.sin(ms / 230)];
    const report = carry.map(Math.trunc);
    carry = carry.map((value, axis) => value - report[axis]);
    reports.push([ms * 1000, ...report]);
  }
  const pointer = [];
  let position = [100, 100];
  let next = 0;
  for (let tUs = -100000; tUs < 2100000; tUs += 4000) {
    /* What the device had sent offsetUs later on its clock. */
    while (next < reports.length && reports[next][0] <= tUs + offsetUs) {
      position = move(position, reports[next]);
      next += 1;
    }
    pointer.push([tUs, Math.round(position[0]), Math.round(position[1])]);
  }
  const fixture = cursorFixture(reports.map(([, x]) => ["motion", 1000, x, 0]), [{ id: "t",
    activity: "target", intended: true, startUs: 0, endUs: 2000000,
    target: { x: 0, y: 0, size: 8 }, start: { x: 100, y: 100 },
    path: { intervalUs: 2000, points: [[0, 0], [1, 0]] }, pointer }]);
  fixture.truth.pointerAcceleration = "off";
  const output = reports.map(([tUs, x, y]) => `${D(tUs + 1000, x, 0, 0)}\n${O(tUs + 1000, x, y)}`)
    .join("\n");
  return { fixture, output };
}

test("the screen scale fit recovers a flat scale and flags pointer acceleration", () => {
  const flat = guidedRun(([x, y], [, rx, ry]) => [x + 1.7 * rx, y + 1.7 * ry], 24000);
  const fit = fitScreenScale([flat]);
  assert.equal(fit.usable, true, fit.reason);
  assert.ok(Math.abs(fit.pxPerCount / 1.7 - 1) < 0.03, `scale ${fit.pxPerCount}`);
  assert.ok(Math.abs(fit.offsetMs - 24) <= 4, `offset ${fit.offsetMs}`);
  assert.ok(fit.r2 > 0.99);
  /* Faster reports move the pointer further per count, as an operating
   * system's pointer acceleration does. */
  const accelerated = guidedRun(([x, y], [, rx, ry]) => {
    const gain = 1 + 0.3 * Math.hypot(rx, ry);
    return [x + gain * rx, y + gain * ry];
  }, 24000);
  const curved = fitScreenScale([accelerated]);
  assert.equal(curved.usable, false);
  assert.ok(curved.fastSlowRatio > 1.25, `ratio ${curved.fastSlowRatio}`);
  assert.match(curved.reason, /pointer acceleration/);
  /* The user's answer counts even when the mapping looks flat. */
  flat.fixture.truth.pointerAcceleration = "on";
  assert.equal(fitScreenScale([flat]).usable, false);
});
