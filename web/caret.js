/* Caret hunt: bring the caret to a highlighted character with the
 * trackball's text movement, in blocks of horizontal-only, vertical-only
 * and mixed moves. Scored like a discrete steering task: arrow steps
 * against the fewest needed, overshoot, reversals, steps on the wrong axis
 * and steps that keep coming after the target is reached. No published
 * research covers trackball text stepping, so the flag thresholds are
 * starting points. Pure, so it runs under node:test. */

import { median as rawMedian } from "./measure.js";

const median = (values) => rawMedian(values.filter(Number.isFinite));

export const CARET_COLUMNS = 48;
export const CARET_LINES = 12;
export const CARET_HOLD_MS = 400;
export const CARET_TIMEOUT_MS = 15000;
/* Steps closer together than this belong to the same roll. */
export const BURST_GAP_MS = 150;

export const CARET_BLOCKS = Object.freeze(["horizontal", "vertical", "mixed"]);
const MOVES = Object.freeze({
  horizontal: [3, 8, 20].flatMap((columns) => [[columns, 0], [-columns, 0]]),
  vertical: [1, 3, 6].flatMap((lines) => [[0, lines], [0, -lines]]),
  mixed: [[6, 2], [-6, 2], [16, -5], [-16, -5]],
});

export const CARET_KEYS = Object.freeze({
  ArrowLeft: { column: -1, line: 0 },
  ArrowRight: { column: 1, line: 0 },
  ArrowUp: { column: 0, line: -1 },
  ArrowDown: { column: 0, line: 1 },
});

function shuffle(items, random) {
  const copy = [...items];
  for (let i = copy.length - 1; i > 0; i -= 1) {
    const j = Math.floor(random() * (i + 1));
    [copy[i], copy[j]] = [copy[j], copy[i]];
  }
  return copy;
}

/* Where a move of `delta` can start so it stays in the grid with a margin,
 * on an axis of `size` cells. A zero move starts anywhere inside. */
function startFor(delta, size, random) {
  const margin = 2;
  if (delta > 0) return margin;
  if (delta < 0) return size - 1 - margin;
  return margin + Math.floor(random() * (size - 2 * margin));
}

/* Each block shuffled on its own, in the order horizontal, vertical,
 * mixed. The caret starts each trial at `start`. */
export function caretTrials({ random = Math.random } = {}) {
  return CARET_BLOCKS.flatMap((block) => shuffle(MOVES[block], random).map(([columns, lines]) => {
    const start = { column: startFor(columns, CARET_COLUMNS, random), line: startFor(lines, CARET_LINES, random) };
    return { block, start, target: { column: start.column + columns, line: start.line + lines } };
  }));
}

/* One arrow key applied to the caret, clamped to the grid. */
export function stepCaret(caret, key) {
  const step = CARET_KEYS[key];
  if (!step) return caret;
  return {
    column: Math.max(0, Math.min(CARET_COLUMNS - 1, caret.column + step.column)),
    line: Math.max(0, Math.min(CARET_LINES - 1, caret.line + step.line)),
  };
}

const onTarget = (caret, target) => caret.column === target.column && caret.line === target.line;

/* trial: { block, start, target, heldAt | undefined,
 * events: [{ t, key, column, line }] } with column and line the caret after
 * each step. */
export function measureCaretTrial(trial) {
  const { block, start, target, events } = trial;
  const delta = { column: target.column - start.column, line: target.line - start.line };
  const minimum = Math.abs(delta.column) + Math.abs(delta.line);
  const lastSign = { column: 0, line: 0 };
  const furthest = { column: 0, line: 0 };
  let reversals = 0;
  let wrongAxis = 0;
  let arrivedAt;
  let runaway = 0;
  let inBurst = false;
  events.forEach((event, index) => {
    const step = CARET_KEYS[event.key];
    if (!step) return;
    for (const axis of ["column", "line"]) {
      const sign = Math.sign(step[axis]);
      if (!sign) continue;
      if (lastSign[axis] && sign !== lastSign[axis]) reversals += 1;
      lastSign[axis] = sign;
      if (!delta[axis] && block !== "mixed") wrongAxis += 1;
      const direction = Math.sign(delta[axis]);
      if (direction) furthest[axis] = Math.max(furthest[axis], (event[axis] - target[axis]) * direction);
    }
    /* Steps after the first arrival that are part of the same roll. */
    if (arrivedAt !== undefined && inBurst) {
      if (event.t - events[index - 1].t < BURST_GAP_MS) runaway += 1;
      else inBurst = false;
    }
    if (arrivedAt === undefined && onTarget(event, target)) {
      arrivedAt = event.t;
      inBurst = true;
    }
  });
  const gaps = [];
  for (let i = 1; i < events.length; i += 1) gaps.push(events[i].t - events[i - 1].t);
  const reached = trial.heldAt !== undefined;
  const time = reached && events.length ? trial.heldAt - events[0].t : NaN;
  return {
    block,
    minimum,
    reached,
    steps: events.length,
    extra: reached ? events.length / minimum : NaN,
    overshoot: {
      column: delta.column ? furthest.column : NaN,
      line: delta.line ? furthest.line : NaN,
    },
    reversals,
    wrongAxis: block === "mixed" || !events.length ? NaN : wrongAxis / events.length,
    runaway,
    time,
    timePerStep: time / minimum,
    stepInterval: median(gaps),
  };
}

function summarize(measured) {
  const reached = measured.filter((item) => item.reached);
  return {
    trials: measured.length,
    reached: measured.length ? reached.length / measured.length : NaN,
    extra: median(reached.map(({ extra }) => extra)),
    overshootColumns: median(measured.map(({ overshoot }) => overshoot.column)),
    overshootLines: median(measured.map(({ overshoot }) => overshoot.line)),
    reversals: median(measured.map(({ reversals }) => reversals)),
    wrongAxis: median(measured.map(({ wrongAxis }) => wrongAxis)),
    runaway: median(measured.map(({ runaway }) => runaway)),
    time: median(reached.map(({ time }) => time)),
    timePerStep: median(reached.map(({ timePerStep }) => timePerStep)),
  };
}

export function analyseCaret(trials) {
  const measured = trials.map(measureCaretTrial);
  const blocks = {};
  for (const block of CARET_BLOCKS) {
    const own = measured.filter((item) => item.block === block);
    if (own.length) blocks[block] = summarize(own);
  }
  const notes = [];
  const { horizontal, vertical } = blocks;
  const all = summarize(measured);
  if (horizontal?.overshootColumns >= 2) {
    notes.push("Horizontal moves went 2 or more characters past the target: horizontal-threshold may be low.");
  }
  if (vertical?.overshootLines >= 1) {
    notes.push("Vertical moves went a line or more past the target: vertical-threshold may be low.");
  }
  if (horizontal?.timePerStep >= 300) {
    notes.push("Horizontal moves took 300 ms or more per character: horizontal-threshold may need too much roll per step.");
  }
  if (vertical?.timePerStep >= 300) {
    notes.push("Vertical moves took 300 ms or more per line: vertical-threshold may need too much roll per step.");
  }
  if ([horizontal, vertical].some((item) => item?.wrongAxis >= 0.1)) {
    notes.push("A tenth or more of the steps in straight moves went on the other axis: the text pipeline's axis intent may be choosing too easily.");
  }
  if (all.runaway >= 1) {
    notes.push("Steps kept coming after the caret reached the target: the ball's coast may still cross the thresholds, or idle-timeout may keep partial travel too long.");
  }
  return { caret: { blocks, overall: all }, notes, trials: measured };
}

export function formatCaret({ blocks }) {
  const cell = (value, digits, scale = 1) => (Number.isFinite(value) ? (value * scale).toFixed(digits) : "n/a");
  return Object.entries(blocks).map(([name, item]) => `${name[0].toUpperCase()}${name.slice(1)} moves: `
    + `reached ${cell(item.reached, 0, 100)}%, steps ${cell(item.extra, 2)}× the fewest, `
    + (name === "vertical" ? "" : `overshoot ${cell(item.overshootColumns, 1)} chars, `)
    + (name === "horizontal" ? "" : `overshoot ${cell(item.overshootLines, 1)} lines, `)
    + `reversals ${cell(item.reversals, 1)}, `
    + (name === "mixed" ? "" : `wrong-axis steps ${cell(item.wrongAxis, 1, 100)}%, `)
    + `runaway steps ${cell(item.runaway, 1)}, median time ${cell(item.time, 0)} ms (${cell(item.timePerStep, 0)} ms per step)`);
}
