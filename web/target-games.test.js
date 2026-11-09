import test from "node:test";
import assert from "node:assert/strict";

import { ringLevels, ringSequence } from "./target-games.js";

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
