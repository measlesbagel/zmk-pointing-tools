/* SPDX-License-Identifier: MIT */

/* The browser scoring API against bench.js. Needs both runner builds:
 * ZPT_NATIVE_DIR (default build/host) and ZPT_WASM_DIR (default
 * build/wasm); the wasm:test dev task sets them. */

import test from "node:test";
import assert from "node:assert/strict";
import { spawnSync } from "node:child_process";
import { mkdtempSync, readdirSync, readFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";

import { eventTimesUs } from "../replay/fixture.js";
import { scoreTraces } from "../replay/score.js";
import { compileWasmRunners, runFixtureWasm } from "../replay/wasm.js";

const REPOSITORY = resolve(dirname(fileURLToPath(import.meta.url)), "../..");
const NATIVE = process.env.ZPT_NATIVE_DIR ?? join(REPOSITORY, "build/host");
const WASM = process.env.ZPT_WASM_DIR ?? join(REPOSITORY, "build/wasm");
const TUNE = join(REPOSITORY, "host/bench/fixtures/tune");
const SHORT_RISE = join(REPOSITORY, "host/bench/profiles/short-rise.json");

const runners = await compileWasmRunners((file) => readFileSync(join(WASM, file)));
const readJson = (path) => JSON.parse(readFileSync(path, "utf8"));
const tune = readdirSync(TUNE).filter((name) => name.endsWith(".json")).sort()
  .map((name) => readJson(join(TUNE, name)));

/* bench.js's report for the tune set, natively replayed. */
function bench(...options) {
  const json = join(mkdtempSync(join(tmpdir(), "zpt-bench-")), "bench.json");
  const result = spawnSync(process.execPath, [join(REPOSITORY, "host/replay/bench.js"),
    "--build-dir", NATIVE, "--json", json, ...options], { encoding: "utf8" });
  assert.equal(result.status, 0, result.stderr);
  return readJson(json).sets.tune.fixtureResults.map(({ path, ...rest }) => rest);
}

test("scoreTraces reports what bench.js reports for the same traces", () => {
  const result = scoreTraces(tune, null, runners);
  assert.equal(result.fixtures, tune.length);
  assert.deepEqual(result.fixtureResults, bench());
});

test("a profile replaces the recorded settings as bench.js --profile does", () => {
  const profile = readJson(SHORT_RISE);
  const withProfile = scoreTraces(tune, profile, runners);
  assert.deepEqual(withProfile.fixtureResults, bench("--profile", SHORT_RISE));
  assert.notDeepEqual(withProfile.scores, scoreTraces(tune, null, runners).scores);
});

test("invalid traces and runner failures are reported", () => {
  assert.throws(() => scoreTraces([{ ...tune[0], events: [] }], null, runners),
    /events are required/);
  const cursor = tune.find((trace) => trace.processor.kind === "cursor-pipeline");
  const stage = cursor.processor.stages.find((candidate) => candidate.type === "transfer");
  const profile = { schema: "zmk-pointing-tools/tuning-profile", version: 1,
    targets: [{ stableId: stage.stableId, parameters: [{ key: "no-such-key", value: 1 }] }] };
  assert.throws(() => scoreTraces([cursor], profile, runners),
    new RegExp(`runner failed: ${stage.stableId}: unknown parameter no-such-key`));
});

/* A guided precision run with known targets: each trial is a fast first
 * submovement that lands 10 % short, then slow homing with a 6 Hz wobble
 * onto the target. The browser pointer follows the recorded settings'
 * output at one pixel per count, as a computer without pointer
 * acceleration would, and each target is where that output ends. */
function guidedPrecisionRun() {
  const base = tune.find((trace) => trace.id === "slow-aim-precision");
  const mmPerCount = 25.4 / base.source.cpi;
  const trials = [30, 12, 45, 20, 8, 35].map((distanceMm, index) => ({ distanceMm,
    angle: index * 2.1 }));
  const events = [];
  const strokes = [];
  let carry = [0, 0];
  let ball = [0, 0];
  let lastUs = 0;
  let nowUs = 200000;
  for (const [index, { distanceMm, angle }] of trials.entries()) {
    const counts = distanceMm / mmPerCount;
    const startUs = nowUs;
    const points = [[0, 0]];
    const origin = [...ball];
    const along = (ms) => {
      if (ms <= 300) {
        const t = ms / 300;
        return 0.9 * counts * (10 * t ** 3 - 15 * t ** 4 + 6 * t ** 5);
      }
      const t = (ms - 300) / 500;
      return counts * (0.9 + 0.1 * t) + (0.3 / mmPerCount) * Math.sin(2 * Math.PI * 6 * t / 2);
    };
    let previous = 0;
    for (let ms = 1; ms <= 800; ms += 1) {
      const now = along(ms);
      carry = [carry[0] + (now - previous) * Math.cos(angle), carry[1] + (now - previous) * Math.sin(angle)];
      previous = now;
      const step = carry.map(Math.trunc);
      carry = carry.map((value, axis) => value - step[axis]);
      nowUs += 1000;
      if (step[0] || step[1]) {
        events.push(["motion", nowUs - lastUs, step[0], step[1], 0]);
        lastUs = nowUs;
        ball = [ball[0] + step[0], ball[1] + step[1]];
      }
      if (ms % 2 === 0) points.push([ball[0] - origin[0], ball[1] - origin[1]]);
    }
    strokes.push({ id: `label-${index + 1}`, source: "label", activity: "target", intended: true,
      startUs, endUs: nowUs, path: { intervalUs: 2000, points } });
    nowUs += 200000;
  }
  const fixture = { ...structuredClone(base), id: "guided-precision", events,
    truth: { pointerAcceleration: "off", strokes } };
  /* The pointer the user saw: the recorded output, a pixel per count. */
  const reports = runFixtureWasm(fixture, runners).trim().split("\n")
    .filter((line) => line.startsWith("O")).map((line) => line.split("\t").map(Number));
  const times = eventTimesUs(fixture);
  const pointer = [];
  let position = [400, 300];
  let next = 0;
  for (let tUs = 0; tUs <= times.at(-1) + 100000; tUs += 4000) {
    while (next < reports.length && reports[next][4] <= tUs) {
      position = [position[0] + reports[next][2], position[1] + reports[next][3]];
      next += 1;
    }
    pointer.push([tUs, ...position]);
  }
  const at = (tUs) => pointer.findLast(([time]) => time <= tUs);
  for (const stroke of strokes) {
    const [, sx, sy] = at(stroke.startUs);
    const [, tx, ty] = at(stroke.endUs);
    Object.assign(stroke, { start: { x: sx, y: sy }, target: { x: tx, y: ty, size: 16 },
      pointer: pointer.filter(([time]) => time >= stroke.startUs - 150000 &&
        time < stroke.endUs + 150000) });
  }
  return fixture;
}

test("guided cursor trials score a too-high gain and a gain jump as worse", () => {
  const fixture = guidedPrecisionRun();
  const acceleration = fixture.processor.stages.find(({ type }) => type === "acceleration");
  const transfer = fixture.processor.stages.find(({ type }) => type === "transfer");
  const profile = (stableId, values) => ({ schema: "zmk-pointing-tools/tuning-profile",
    version: 1, targets: [{ stableId, parameters: Object.entries(values)
      .map(([key, value]) => ({ key, value })) }] });
  const recorded = scoreTraces([fixture], null, runners);
  const [{ screen }] = recorded.fixtureResults;
  assert.equal(screen.usable, true, screen.reason);
  assert.ok(Math.abs(screen.pxPerCount - 1) < 0.03, `scale ${screen.pxPerCount}`);
  assert.equal(recorded.scores.homingStepPercent.strokes, 6);
  assert.equal(recorded.scores.ballisticSpreadPercent.strokes, 6);
  /* Every first submovement was built to land 10 % short; the homing
   * wobble starts as it ends, moving its speed minimum, and so the
   * landing, up to a few percent on. */
  assert.ok(Math.abs(recorded.scores.ballisticBiasPercent.value + 10) < 4,
    `bias ${recorded.scores.ballisticBiasPercent.value}`);
  /* Three times the gain: every homing step and wobble is three times the
   * size against the same targets. */
  const high = scoreTraces([fixture], profile(transfer.stableId, { "scale-multiplier": 3 }),
    runners).scores;
  for (const name of ["homingStepPercent", "homingJitterPercent"]) {
    assert.ok(high[name].value > 2 * recorded.scores[name].value,
      `${name}: ${high[name].value} vs ${recorded.scores[name].value}`);
  }
  /* A third or a tenth of the gain shrinks homing steps and wobble against
   * the same targets, but lands every trial at the same share of its
   * distance, so it can't win on spread, and moves the bias so far that
   * the pixel metrics count as worse. */
  for (const divisor of [3, 10]) {
    const low = scoreTraces([fixture], profile(transfer.stableId, { "scale-divisor": divisor }),
      runners).scores;
    assert.ok(low.ballisticSpreadPercent.value > 0.95 * recorded.scores.ballisticSpreadPercent.value,
      `÷${divisor} spread ${low.ballisticSpreadPercent.value} vs ${recorded.scores.ballisticSpreadPercent.value}`);
    for (const name of ["ballisticSpreadPercent", "homingStepPercent", "homingJitterPercent"]) {
      assert.match(low[name].guard ?? "", /more than 25 points/, `÷${divisor} ${name}`);
    }
  }
  assert.equal(recorded.scores.homingJitterPercent.guard, undefined);
  /* A gain cut only at slow speeds, or one too small to move the bias far,
   * still shrinks homing steps and wobble; the homing output against the
   * recorded settings gives it away. A tenth off the gain is within it. */
  const slowCut = {};
  [25, 25, 25, 25, 165, 190, 200, 200].forEach((percent, i) => {
    slowCut[`curve-sensitivity-${i + 1}`] = percent;
  });
  const candidates = [["slow points at 25 %", profile(acceleration.stableId, slowCut)],
    ["÷1.25", profile(transfer.stableId, { "scale-multiplier": 4, "scale-divisor": 5 })]];
  for (const [label, candidate] of candidates) {
    const scores = scoreTraces([fixture], candidate, runners).scores;
    for (const name of ["homingStepPercent", "homingJitterPercent"]) {
      assert.match(scores[name].guard ?? "", /homing output/, `${label} ${name}`);
    }
  }
  const slight = scoreTraces([fixture],
    profile(transfer.stableId, { "scale-multiplier": 10, "scale-divisor": 11 }), runners).scores;
  assert.equal(slight.homingJitterPercent.guard, undefined);
  /* The same sensitivity range, but all of it at 40 mm/s, which every
   * first submovement slows through. */
  const jump = {};
  [0, 10, 20, 40, 41, 100, 250, 400].forEach((speed, i) => { jump[`curve-speed-${i + 1}`] = speed; });
  [100, 100, 100, 100, 200, 200, 200, 200].forEach((percent, i) => {
    jump[`curve-sensitivity-${i + 1}`] = percent;
  });
  const jumped = scoreTraces([fixture], profile(acceleration.stableId, jump), runners).scores;
  assert.ok(jumped.transitionStepPercent.value > 1.5 * recorded.scores.transitionStepPercent.value,
    `${jumped.transitionStepPercent.value} vs ${recorded.scores.transitionStepPercent.value}`);
  /* A steep ramp from 30 to 60 mm/s is smoother than the jump but rougher
   * than the recorded curve, which spreads the same rise from 25 to
   * 250 mm/s. */
  const ramp = { ...jump, "curve-speed-4": 30, "curve-speed-5": 60 };
  const ramped = scoreTraces([fixture], profile(acceleration.stableId, ramp), runners).scores;
  assert.ok(ramped.transitionStepPercent.value > recorded.scores.transitionStepPercent.value &&
    ramped.transitionStepPercent.value < jumped.transitionStepPercent.value,
  `ramp ${ramped.transitionStepPercent.value}`);
});
