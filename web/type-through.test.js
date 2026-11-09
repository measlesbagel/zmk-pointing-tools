import test from "node:test";
import assert from "node:assert/strict";

import { TYPE_PASSAGE, analyseTypeThrough, formatTypeThrough, measureTypeThrough } from "./type-through.js";

/* `count` correct characters `gap` ms apart from `from`. */
function typed(count, from = 0, gap = 200) {
  return Array.from({ length: count }, (_, i) => ({ t: from + i * gap, kind: "char", correct: true }));
}

test("a clean passage reports nothing unintended", () => {
  const events = typed(100);
  const result = measureTypeThrough({ passage: TYPE_PASSAGE, events, endedAt: 19800 });
  assert.equal(result.characters, 100);
  assert.equal(result.errors, 0);
  /* 100 characters is 20 words in 19.8 s. */
  assert.equal(Math.round(result.wordsPerMinute), 61);
  assert.deepEqual(result.resting, { steps: 0, wheel: 0, pointer: 0 });
  assert.equal(result.typing.stepsPer100, 0);
  assert.deepEqual(analyseTypeThrough({ events, endedAt: 19800 }).notes, []);
});

test("splits stray output into typing and resting", () => {
  const events = [
    ...typed(50, 500),
    { t: 100, kind: "pointer", distance: 400 },
    { t: 1000, kind: "step" },
    { t: 2000, kind: "pointer", distance: 15 },
    { t: 5000, kind: "wheel" },
    /* The last key is at 10300; this is 1.7 s later, so resting. */
    { t: 12000, kind: "step" },
    { t: 12500, kind: "pointer", distance: 30 },
    ...typed(50, 15000),
  ].sort((a, b) => a.t - b.t);
  const run = { events, endedAt: 24800 };
  const result = measureTypeThrough(run);
  assert.deepEqual(result.resting, { steps: 1, wheel: 0, pointer: 30 });
  assert.equal(result.typing.steps, 1);
  assert.equal(result.typing.wheel, 1);
  assert.equal(result.typing.pointer, 15);
  const { notes } = analyseTypeThrough(run);
  assert.ok(notes.some((note) => note.includes("caret stepped")));
  assert.ok(notes.some((note) => note.includes("Scroll events")));
  assert.ok(notes.some((note) => note.includes("during pauses")));
  assert.ok(!notes.some((note) => note.includes("pointer moved")));
  assert.equal(formatTypeThrough(result).length, 3);
});

test("counts wrong characters and backspaces", () => {
  const events = [...typed(3), { t: 600, kind: "char", correct: false }, { t: 800, kind: "backspace" }];
  const result = measureTypeThrough({ events });
  assert.equal(result.characters, 4);
  assert.equal(result.errors, 0.25);
  assert.equal(result.backspaces, 1);
  assert.equal(result.finished, false);
});
