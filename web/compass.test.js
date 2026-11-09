import test from "node:test";
import assert from "node:assert/strict";

import {
  ORIENTATIONS,
  analyseCompass,
  correctedOrientation,
  cueDirections,
  describeMismatch,
  fitOrientation,
  initialMovement,
  orientationMatrix,
} from "./compass.js";

function apply(matrix, { x, y }) {
  return { x: matrix[0][0] * x + matrix[0][1] * y, y: matrix[1][0] * x + matrix[1][1] * y };
}

/* Cursor paths that start toward where `mismatch` sends each cue, with a
 * little wobble, then wander off. */
function play(mismatch, { wobble = 8 } = {}) {
  const matrix = orientationMatrix(mismatch);
  return [...cueDirections(), ...cueDirections()].map((cue, i) => {
    const heading = apply(matrix, cue);
    const turn = ((i % 3) - 1) * wobble * Math.PI / 180;
    const dx = heading.x * Math.cos(turn) - heading.y * Math.sin(turn);
    const dy = heading.x * Math.sin(turn) + heading.y * Math.cos(turn);
    const start = { x: 300, y: 200 };
    const samples = Array.from({ length: 12 }, (_, step) => ({ t: step * 8, x: start.x + dx * step * 4, y: start.y + dy * step * 4 }));
    return { cue, start, samples };
  });
}

test("cues go round the compass on screen, up being negative y", () => {
  const cues = cueDirections();
  assert.equal(cues.length, 8);
  assert.ok(Math.abs(cues[2].x) < 1e-9 && Math.abs(cues[2].y + 1) < 1e-9);
});

test("the start of a movement is its direction over the first pixels", () => {
  const movement = initialMovement([{ x: 0, y: 0 }, { x: 5, y: 1 }, { x: 30, y: 0 }, { x: 0, y: 90 }], { x: 0, y: 0 });
  assert.deepEqual(movement, { x: 1, y: 0 });
  assert.equal(initialMovement([{ x: 0, y: 0 }, { x: 3, y: 3 }], { x: 0, y: 0 }), undefined);
});

test("finds every flip and turn from the starts of movements", () => {
  for (const mismatch of ORIENTATIONS) {
    const trials = play(mismatch).map(({ cue, start, samples }) => ({ cue, movement: initialMovement(samples, start) }));
    const fit = fitOrientation(trials);
    assert.deepEqual(fit.mismatch, mismatch);
    assert.equal(fit.agreement, 1);
    assert.ok(fit.angleError <= 8.01);
  }
});

test("the suggested setting undoes the mismatch on top of the current one", () => {
  for (const current of ORIENTATIONS) {
    for (const mismatch of ORIENTATIONS) {
      const fixed = correctedOrientation(current, mismatch);
      /* What the cursor does with the fix, relative to the correct mapping,
       * must be nothing: mismatch · current⁻¹ · fixed = identity. */
      const m = orientationMatrix(mismatch);
      const c = orientationMatrix(current);
      const f = orientationMatrix(fixed);
      for (const cue of cueDirections()) {
        /* The ball that the current setting turned into m·cue. */
        const ballThroughCurrent = apply(m, cue);
        const raw = apply([[c[0][0], c[1][0]], [c[0][1], c[1][1]]], ballThroughCurrent);
        const after = apply(f, raw);
        assert.ok(Math.abs(after.x - cue.x) < 1e-9 && Math.abs(after.y - cue.y) < 1e-9, JSON.stringify({ current, mismatch }));
      }
    }
  }
});

test("reports the fix in words and settings", () => {
  const mirrored = analyseCompass(play({ swapXY: false, invertX: true, invertY: false }), { swapXY: false, invertX: false, invertY: false });
  assert.equal(mirrored.notes[0], "Left and right are mirrored.");
  assert.equal(mirrored.notes[1], "To fix it, set swap-xy off, invert-x on, invert-y off.");
  assert.equal(mirrored.compass.correct, false);

  const fine = analyseCompass(play({}), { swapXY: true, invertX: false, invertY: true });
  assert.equal(fine.notes[0], "The cursor follows the ball.");
  assert.match(fine.notes[1], /^Median start-angle error/);

  assert.equal(describeMismatch({ swapXY: true, invertX: false, invertY: true }), "The motion is turned a quarter turn anticlockwise.");
  assert.deepEqual(analyseCompass([]), { notes: ["No movement was recorded."] });
});

test("inconsistent movements ask for another go instead of a fix", () => {
  const trials = play({}).map((trial, i) => (i % 2 ? play({ invertX: true, invertY: true })[i] : trial));
  const result = analyseCompass(trials, { swapXY: false, invertX: false, invertY: false });
  assert.match(result.notes.join(" "), /not consistent enough/);
});
