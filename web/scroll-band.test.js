import test from "node:test";
import assert from "node:assert/strict";

import { analyseScrollBand, formatScrollBand, measureScrollTrial, scrollTrials } from "./scroll-band.js";
import { seededRandom } from "./measure.js";

test("a shown block then a hidden block, every distance both ways", () => {
  const trials = scrollTrials({ random: seededRandom(9) });
  assert.equal(trials.length, 12);
  assert.ok(trials.slice(0, 6).every(({ known }) => known));
  assert.ok(trials.slice(6).every(({ known }) => !known));
  assert.deepEqual(trials.slice(0, 6).map(({ distance }) => distance).sort((a, b) => a - b), [-2400, -900, -300, 300, 900, 2400]);
  assert.deepEqual(scrollTrials({ random: seededRandom(9) }), trials);
});

/* Wheel steps toward a target, optionally past it and back. */
function trial({ distance = 900, step = 100, pastBy = 0, gap = 16, known = true, held = true } = {}) {
  const events = [];
  const sign = Math.sign(distance);
  let offset = 0;
  let t = 1000;
  const goal = Math.abs(distance) + pastBy;
  while (offset * sign < goal) {
    const delta = Math.min(step, goal - offset * sign) * sign;
    offset += delta;
    events.push({ t, delta, offset });
    t += gap;
  }
  while (offset * sign > Math.abs(distance)) {
    const delta = -Math.min(step, offset * sign - Math.abs(distance)) * sign;
    offset += delta;
    events.push({ t, delta, offset });
    t += gap;
  }
  return { known, distance, band: 40, startedAt: 1000, heldAt: held ? t - gap : undefined, events };
}

test("measures time, overshoot, reversals, settle and wheel spacing", () => {
  const clean = measureScrollTrial(trial());
  assert.equal(clean.reached, true);
  assert.equal(clean.overshoot, 0);
  assert.equal(clean.reversals, 0);
  assert.equal(clean.wheelInterval, 16);
  assert.equal(clean.settle, 0);

  const past = measureScrollTrial(trial({ distance: -300, pastBy: 120 }));
  assert.equal(past.distance, 300);
  assert.equal(past.overshoot, 100);
  assert.equal(past.reversals, 1);
  assert.ok(past.settle > 0);

  assert.equal(measureScrollTrial(trial({ held: false })).reached, false);
});

test("splits shown from hidden, scores throughput, and flags", () => {
  const trials = [
    ...[300, -300, 900, -900, 2400, -2400].map((distance) => trial({ distance, pastBy: Math.abs(distance) === 300 ? 60 : 0 })),
    ...[300, -300, 900, -900].map((distance) => trial({ distance, known: false, gap: 60, pastBy: Math.abs(distance) === 300 ? 60 : 0 })),
    trial({ distance: 2400, known: false, held: false }), trial({ distance: -2400, known: false, gap: 60 }),
  ];
  const result = analyseScrollBand(trials);
  assert.deepEqual(Object.keys(result.scrollBand.blocks), ["Shown", "Hidden"]);
  assert.ok(result.scrollBand.blocks.Shown.throughput > 0);
  assert.equal(result.scrollBand.byDistance["2400 px"].reached, 0.75);
  assert.equal(result.notes.length, 3);
  assert.match(result.notes[0], /Short scrolls went past/);
  assert.match(result.notes[1], /Long scrolls didn't always reach/);
  assert.match(result.notes[2], /50 ms or more apart/);
  assert.match(formatScrollBand(result.scrollBand)[0], /^Target shown: reached 100%, throughput [\d.]+ bit\/s/);
});
