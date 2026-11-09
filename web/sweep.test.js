import test from "node:test";
import assert from "node:assert/strict";

import { SWEEP_MEASURES, describeSweep, planSweep, summarizeSweep, sweepChart, sweepValues } from "./sweep.js";
import { seededRandom } from "./measure.js";

test("values are evenly spaced whole numbers inside the setting's range", () => {
  assert.deepEqual(sweepValues(1, 4, 4), [1, 2, 3, 4]);
  assert.deepEqual(sweepValues(10, 30, 3), [10, 20, 30]);
  assert.deepEqual(sweepValues(40, 0, 5, { minimum: 1, maximum: 20 }), [1, 6, 11, 15, 20]);
  /* A narrow range collapses repeats rather than playing a value twice. */
  assert.deepEqual(sweepValues(1, 2, 5), [1, 2]);
  assert.equal(sweepValues(1, 100, 9).length, 5);
});

test("every value plays twice in a mirrored order, with warm-up and adaptation", () => {
  const plan = planSweep(4, seededRandom(3));
  assert.equal(plan.order.length, 8);
  assert.deepEqual(plan.order.slice(4), [...plan.order.slice(0, 4)].reverse());
  for (let key = 0; key < 4; key += 1) {
    assert.equal(plan.blocks.filter((block) => block.scored && block.condition === key).length, 2);
  }
  assert.equal(plan.blocks[0].kind, "warm-up");
  for (let i = 1; i < plan.blocks.length; i += 1) {
    if (plan.blocks[i].condition !== plan.blocks[i - 1].condition) assert.equal(plan.blocks[i].scored, false);
  }
  assert.deepEqual(new Set(Object.values(plan.labels)), new Set(["P", "Q", "R", "S"]));
});

function measured(time, overshoot, count = 10) {
  return Array.from({ length: count }, () => ({ movementTime: time, hit: true, overshoot, signedError: 0, reEntries: 0, submovements: 1, rerolls: 0 }));
}

test("summarises scored blocks per value, in value order, and never picks one", () => {
  const values = [30, 10, 20];
  const blocks = [
    { condition: 0, scored: false, measured: measured(9000, 1) },
    { condition: 0, scored: true, measured: measured(500, 0.12), throughput: 3.3 },
    { condition: 1, scored: true, measured: measured(700, 0.02), throughput: 2.8 },
    { condition: 2, scored: true, measured: measured(560, 0.06), throughput: 3.2 },
    { condition: 2, scored: true, measured: measured(540, 0.06), throughput: 3.4 },
  ];
  const rows = summarizeSweep(blocks, values);
  assert.deepEqual(rows.map(({ value }) => value), [10, 20, 30]);
  assert.equal(rows[2].trials, 10);
  assert.ok(Math.abs(rows[1].throughput - 3.3) < 1e-9);
  assert.equal(rows[1].movementTime, 550);
  const lines = describeSweep("scale-multiplier", rows);
  assert.match(lines[0], /^Sweep of scale-multiplier over 10, 20, 30/);
  assert.match(lines[3], /^scale-multiplier 30: throughput 3.30 bit\/s, median time on hits 500 ms, median overshoot 12.0%/);
  assert.match(lines.at(-1), /doesn't pick a value/);

  const chart = sweepChart(rows, SWEEP_MEASURES[0]);
  assert.match(chart, /^<svg/);
  assert.equal((chart.match(/<circle/g) ?? []).length, 3);
  assert.equal(sweepChart(rows.slice(0, 1), SWEEP_MEASURES[0]), "");
});
