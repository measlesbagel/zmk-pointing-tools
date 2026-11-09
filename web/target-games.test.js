import test from "node:test";
import assert from "node:assert/strict";

import { flickFlags, flickTrials, ringLevels, ringSequence } from "./target-games.js";

test("the ring crosses the circle on every movement and visits every target", () => {
  const targets = ringSequence({ amplitude: 400, width: 20, count: 13, centre: { x: 500, y: 300 } });
  assert.equal(targets.length, 14);
  for (const target of targets) {
    assert.ok(Math.abs(Math.hypot(target.x - 500, target.y - 300) - 200) < 1e-9);
  }
  const visited = new Set(targets.slice(0, 13).map(({ x, y }) => `${x.toFixed(3)},${y.toFixed(3)}`));
  assert.equal(visited.size, 13);
  /* Each move spans most of the diameter. */
  for (let i = 1; i < targets.length; i += 1) {
    const distance = Math.hypot(targets[i].x - targets[i - 1].x, targets[i].y - targets[i - 1].y);
    assert.ok(distance > 390, `move ${i} is ${distance}`);
  }
  assert.throws(() => ringSequence({ amplitude: 100, width: 10, count: 12 }), /odd/);
});

test("levels get harder and fit the playing area", () => {
  const levels = ringLevels({ width: 1000, height: 600 });
  assert.equal(levels.length, 3);
  assert.ok(levels[0].id < levels[1].id && levels[1].id < levels[2].id);
  assert.ok(levels.every(({ amplitude }) => amplitude <= 480));
  assert.ok(levels[0].id > 2 && levels[2].id < 8);
});

test("flicks cover every band in every direction, in a repeatable order", async () => {
  const { seededRandom } = await import("./measure.js");
  const size = { width: 1000, height: 600 };
  const trials = flickTrials(size, { random: seededRandom(7) });
  assert.equal(trials.length, 18);
  for (const band of ["near", "mid", "far"]) assert.equal(trials.filter((trial) => trial.band === band).length, 6);
  for (const { target } of trials) {
    assert.ok(target.x > 0 && target.x < size.width && target.y > 0 && target.y < size.height);
  }
  const distance = (trial) => Math.hypot(trial.target.x - 500, trial.target.y - 300);
  const near = trials.find((trial) => trial.band === "near");
  const far = trials.find((trial) => trial.band === "far");
  assert.ok(distance(far) > distance(near) * 3);
  assert.deepEqual(flickTrials(size, { random: seededRandom(7) }), trials);
});

test("flags a band that did clearly worse without picking a value", () => {
  const ok = { hitRate: 1, signedError: 0, submovements: 1, rerolls: 0 };
  assert.deepEqual(flickFlags({ near: ok, mid: ok, far: ok }, 16), []);
  const flags = flickFlags({
    near: { ...ok, signedError: 12, submovements: 3 },
    mid: ok,
    far: { ...ok, rerolls: 1, signedError: -30, hitRate: 0.5 },
  }, 16);
  assert.equal(flags.length, 5);
  assert.ok(flags.every((flag) => !/\d/.test(flag)));
});
