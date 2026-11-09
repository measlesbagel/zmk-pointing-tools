import test from "node:test";
import assert from "node:assert/strict";

import {
  CARET_COLUMNS, CARET_LINES, analyseCaret, caretTrials, formatCaret, measureCaretTrial, stepCaret,
} from "./caret.js";
import { seededRandom } from "./measure.js";

test("plans three blocks of moves that stay inside the grid", () => {
  const trials = caretTrials({ random: seededRandom(1) });
  assert.deepEqual(trials.map(({ block }) => block), [
    ...Array(6).fill("horizontal"), ...Array(6).fill("vertical"), ...Array(4).fill("mixed"),
  ]);
  for (const { block, start, target } of trials) {
    for (const point of [start, target]) {
      assert.ok(point.column >= 0 && point.column < CARET_COLUMNS);
      assert.ok(point.line >= 0 && point.line < CARET_LINES);
    }
    if (block === "horizontal") assert.equal(start.line, target.line);
    if (block === "vertical") assert.equal(start.column, target.column);
  }
});

test("steps the caret and clamps it to the grid", () => {
  assert.deepEqual(stepCaret({ column: 0, line: 3 }, "ArrowLeft"), { column: 0, line: 3 });
  assert.deepEqual(stepCaret({ column: 4, line: 3 }, "ArrowDown"), { column: 4, line: 4 });
  assert.deepEqual(stepCaret({ column: 4, line: 3 }, "KeyA"), { column: 4, line: 3 });
});

/* Plays keys from the trial's start, `gap` ms apart, and holds on the
 * target at the end. */
function play(trial, keys, gap = 50) {
  let caret = trial.start;
  const events = keys.map((key, index) => {
    caret = stepCaret(caret, key);
    return { t: index * gap, key, ...caret };
  });
  return { ...trial, events, heldAt: (keys.length - 1) * gap };
}

const right8 = { block: "horizontal", start: { column: 2, line: 4 }, target: { column: 10, line: 4 } };

test("scores a clean move as the fewest steps with nothing extra", () => {
  const measured = measureCaretTrial(play(right8, Array(8).fill("ArrowRight")));
  assert.equal(measured.extra, 1);
  assert.equal(measured.overshoot.column, 0);
  assert.ok(Number.isNaN(measured.overshoot.line));
  assert.equal(measured.reversals, 0);
  assert.equal(measured.wrongAxis, 0);
  assert.equal(measured.runaway, 0);
  assert.equal(measured.time, 350);
});

test("counts overshoot, reversals, runaway and wrong-axis steps", () => {
  const keys = [...Array(8).fill("ArrowRight"), "ArrowRight", "ArrowRight", "ArrowDown", "ArrowUp"];
  const trial = play(right8, keys);
  /* Then a pause and a slow walk back. */
  trial.events.push({ t: 1000, key: "ArrowLeft", column: 9, line: 4 }, { t: 1300, key: "ArrowLeft", column: 8, line: 4 });
  trial.events.push({ t: 1600, key: "ArrowRight", column: 9, line: 4 }, { t: 1900, key: "ArrowRight", column: 10, line: 4 });
  trial.heldAt = 1900;
  const measured = measureCaretTrial(trial);
  assert.equal(measured.overshoot.column, 2);
  assert.equal(measured.runaway, 4);
  assert.equal(measured.wrongAxis, 2 / 16);
  /* Right to left, left to right, and down to up. */
  assert.equal(measured.reversals, 3);
  assert.equal(measured.extra, 16 / 8);
});

test("flags overshoot and slow steps per axis, and runaway overall", () => {
  const down3 = { block: "vertical", start: { column: 5, line: 2 }, target: { column: 5, line: 5 } };
  const over = { ...right8, events: [], heldAt: undefined };
  let caret = right8.start;
  for (let i = 0; i < 11; i += 1) {
    caret = stepCaret(caret, "ArrowRight");
    over.events.push({ t: i * 40, key: "ArrowRight", ...caret });
  }
  for (let i = 0; i < 3; i += 1) {
    caret = stepCaret(caret, "ArrowLeft");
    over.events.push({ t: 2000 + i * 400, key: "ArrowLeft", ...caret });
  }
  over.heldAt = 2800;
  const slow = play(down3, Array(3).fill("ArrowDown"), 600);
  const { caret: result, notes } = analyseCaret([over, over, slow, slow]);
  assert.equal(result.blocks.horizontal.overshootColumns, 3);
  assert.ok(notes.some((note) => note.includes("horizontal-threshold may be low")));
  assert.ok(notes.some((note) => note.includes("vertical-threshold may need")));
  assert.ok(notes.some((note) => note.includes("idle-timeout")));
  assert.ok(!notes.some((note) => note.includes("axis intent")));
  assert.equal(formatCaret(result).length, 2);
});

test("a move that never arrives counts as not reached", () => {
  const measured = measureCaretTrial({ ...right8, events: [], heldAt: undefined });
  assert.equal(measured.reached, false);
  assert.ok(Number.isNaN(measured.time));
  const { caret: result } = analyseCaret([{ ...right8, events: [], heldAt: undefined }]);
  assert.equal(result.blocks.horizontal.reached, 0);
});
