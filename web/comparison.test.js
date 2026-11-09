import test from "node:test";
import assert from "node:assert/strict";

import { compareConditions, describeComparison, describePreference, hiddenLabels, planBlocks } from "./comparison.js";
import { seededRandom } from "./measure.js";

test("sessions warm up, balance the order and adapt after every switch", () => {
  const abba = planBlocks(() => 0.1);
  assert.equal(abba.order, "ABBA");
  assert.deepEqual(abba.blocks.map(({ condition, kind }) => `${condition}:${kind}`), [
    "A:warm-up", "A:scored", "B:adaptation", "B:scored", "B:scored", "A:adaptation", "A:scored",
  ]);
  const baab = planBlocks(() => 0.9);
  assert.equal(baab.order, "BAAB");
  for (const { blocks } of [abba, baab]) {
    /* Each side gets the same number of scored blocks, and every scored
     * block follows an unscored one on the same side or another scored one. */
    assert.equal(blocks.filter((b) => b.scored && b.condition === "A").length, 2);
    assert.equal(blocks.filter((b) => b.scored && b.condition === "B").length, 2);
    for (let i = 1; i < blocks.length; i += 1) {
      if (blocks[i].condition !== blocks[i - 1].condition) assert.equal(blocks[i].scored, false);
    }
  }
  assert.deepEqual(hiddenLabels(() => 0.1), { A: "X", B: "Y" });
  assert.deepEqual(hiddenLabels(() => 0.9), { A: "Y", B: "X" });
});

function measured(times, { overshoot = 0, hit = true } = {}) {
  return times.map((movementTime) => ({ movementTime, hit, overshoot, signedError: 0, reEntries: 0, submovements: 1, rerolls: 0 }));
}

test("compares only scored blocks, with an interval on the difference", () => {
  const random = seededRandom(5);
  const jitter = (base) => Array.from({ length: 13 }, () => base + (random() - 0.5) * 60);
  const blocks = [
    { condition: "A", scored: false, measured: measured(jitter(2000)) },
    { condition: "A", scored: true, measured: measured(jitter(600)), throughput: 3 },
    { condition: "B", scored: false, measured: measured(jitter(2000)) },
    { condition: "B", scored: true, measured: measured(jitter(500), { overshoot: 0.05 }), throughput: 3.4 },
    { condition: "B", scored: true, measured: measured(jitter(500), { overshoot: 0.05 }), throughput: 3.6 },
    { condition: "A", scored: false, measured: measured(jitter(2000)) },
    { condition: "A", scored: true, measured: [...measured(jitter(600)), ...measured([100], { hit: false })], throughput: 3.2 },
  ];
  const result = compareConditions(blocks);
  assert.equal(result.A.summary.trials, 27);
  assert.equal(result.B.summary.trials, 26);
  assert.ok(Math.abs(result.A.throughput - 3.1) < 1e-9);
  assert.ok(result.time.difference < -80 && result.time.difference > -120);
  assert.ok(result.time.high < 0);
  assert.ok(result.time.probabilityBAhead < 0.05);
  assert.ok(result.overshoot.difference > 0.04);

  const lines = describeComparison(result);
  assert.match(lines[0], /doesn't cross zero/);
  assert.match(lines[2], /Throughput: A 3.10, B 3.50 bit\/s/);
  assert.equal(describePreference("Y", { A: "X", B: "Y" }, result), "You preferred Y, which was B, and it was also faster.");
  assert.equal(describePreference("X", { A: "X", B: "Y" }, result), "You preferred X, which was A, but B was faster.");
  assert.equal(describePreference("same", { A: "X", B: "Y" }, result), "You felt no difference.");
});

test("says when a session can't tell the settings apart", () => {
  const random = seededRandom(9);
  const same = () => measured(Array.from({ length: 15 }, () => 500 + (random() - 0.5) * 200));
  const result = compareConditions([
    { condition: "A", scored: true, measured: same() },
    { condition: "B", scored: true, measured: same() },
  ]);
  assert.ok(result.time.low < 0 && result.time.high > 0);
  assert.match(describeComparison(result)[0], /can't tell them apart/);
  assert.match(describePreference("X", { A: "Y", B: "X" }, result), /couldn't tell them apart/);
});
