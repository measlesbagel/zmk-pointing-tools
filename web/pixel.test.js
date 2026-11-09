import test from "node:test";
import assert from "node:assert/strict";

import { analysePixel, formatPixel, measurePixelTrial, onTarget, pixelTrials } from "./pixel.js";
import { seededRandom } from "./measure.js";

test("targets sit on whole pixels, smallest last, inside the area", () => {
  const trials = pixelTrials({ width: 800, height: 500 }, { random: seededRandom(5) });
  assert.deepEqual(trials.map(({ width }) => width), [3, 3, 3, 3, 2, 2, 2, 2, 1, 1, 1, 1]);
  for (const { target } of trials) {
    assert.ok(Number.isInteger(target.left) && Number.isInteger(target.top));
    assert.ok(target.left > 0 && target.left < 800 && target.top > 0 && target.top < 500);
  }
  assert.deepEqual(pixelTrials({ width: 800, height: 500 }, { random: seededRandom(5) }), trials);
});

test("a 1 px target is exactly one screen pixel", () => {
  const target = { left: 10, top: 20, width: 1 };
  assert.ok(onTarget(target, { x: 10, y: 20 }));
  assert.ok(onTarget(target, { x: 10.9, y: 20.5 }));
  assert.ok(!onTarget(target, { x: 11, y: 20 }));
  assert.ok(!onTarget(target, { x: 9.99, y: 20 }));
  assert.ok(onTarget({ left: 9, top: 19, width: 3 }, { x: 11.5, y: 21 }));
});

function trial(width, path, heldAt) {
  return {
    width,
    target: { left: 100, top: 100, width },
    startedAt: 0,
    heldAt,
    samples: path.map(([x, y], i) => ({ t: i * 10, x, y })),
  };
}

test("counts landings and time to the hold that counted", () => {
  const measured = measurePixelTrial(trial(1, [[50, 50], [100, 100], [102, 100], [100, 100]], 30));
  assert.deepEqual(measured, { width: 1, reached: true, acquireTime: 30, entries: 2 });
  assert.equal(measurePixelTrial(trial(1, [[50, 50], [90, 90]], undefined)).reached, false);
});

test("flags a size that wasn't always reached or took many landings", () => {
  const easy = [[0, 0], [101, 101]];
  const fiddly = [[0, 0], [100, 100], [104, 100], [100, 100], [104, 100], [100, 100]];
  const result = analysePixel([
    trial(3, easy, 10), trial(3, easy, 10),
    trial(2, fiddly, 50), trial(2, fiddly, 50),
    trial(1, fiddly, 50), trial(1, [[0, 0]], undefined),
  ]);
  assert.deepEqual(Object.keys(result.pixel), ["3 px", "2 px", "1 px"]);
  assert.equal(result.pixel["1 px"].reached, 0.5);
  assert.equal(result.notes.length, 2);
  assert.match(result.notes[0], /^2 px targets took several landings/);
  assert.match(result.notes[1], /^1 px targets weren't always reached/);
  assert.equal(formatPixel(result.pixel)[0], "3 px: reached 100%, median time to hold 10 ms, landings 1.0, trials 2");
});
