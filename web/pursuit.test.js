import test from "node:test";
import assert from "node:assert/strict";

import { PURSUIT_PHASES, analysePursuit, formatPursuit, measurePursuitPhase, pursuitPosition, reversalTimes } from "./pursuit.js";

const area = { centre: { x: 400, y: 300 }, amplitude: 200 };

test("the target sweeps at a steady speed and reverses at the ends", () => {
  const phase = { axis: "x", speed: 100, duration: 10000 };
  assert.deepEqual(pursuitPosition(phase, 0, area), { x: 400, y: 300 });
  assert.deepEqual(pursuitPosition(phase, 1000, area), { x: 500, y: 300 });
  assert.deepEqual(pursuitPosition(phase, 2000, area), { x: 600, y: 300 });
  assert.deepEqual(pursuitPosition(phase, 3000, area), { x: 500, y: 300 });
  assert.deepEqual(pursuitPosition(phase, 6000, area), { x: 200, y: 300 });
  assert.deepEqual(pursuitPosition({ ...phase, axis: "y" }, 1000, area), { x: 400, y: 400 });
  assert.deepEqual(reversalTimes(phase, 200), [2000, 6000]);
});

/* A pointer that follows the target `delay` ms late, optionally running
 * on `carry` px past each turning point for 200 ms. */
function follow(phase, { delay = 0, carry = 0, offset = 0 } = {}) {
  const samples = [];
  const turns = reversalTimes(phase, area.amplitude);
  for (let t = 0; t <= phase.duration; t += 8) {
    const target = pursuitPosition(phase, t, area);
    const p = { ...pursuitPosition(phase, Math.max(0, t - delay), area) };
    p.y += offset;
    turns.forEach((turn, index) => {
      /* Run on past the turning point and come back. */
      const sign = index % 2 === 0 ? 1 : -1;
      if (carry && t > turn && t < turn + 200) {
        p[phase.axis] = area.centre[phase.axis] + sign * (area.amplitude + carry * Math.sin(((t - turn) / 200) * Math.PI));
      }
    });
    samples.push({ t, p, target });
  }
  return samples;
}

test("a pointer right on the target scores full time and no error", () => {
  const phase = PURSUIT_PHASES[2];
  const result = measurePursuitPhase(phase, follow(phase), area);
  assert.equal(result.onTarget, 1);
  assert.equal(result.rmsError, 0);
  assert.equal(result.lag, 0);
  assert.ok(Math.abs(result.speedRatio - 1) < 0.01);
  assert.equal(result.overshoot, 0);
});

test("lag, overshoot and a constant offset are measured apart", () => {
  const phase = PURSUIT_PHASES[2];
  const late = measurePursuitPhase(phase, follow(phase, { delay: 120 }), area);
  assert.ok(Math.abs(late.lag - 120) <= 8, `lag ${late.lag}`);
  assert.ok(late.onTarget < 0.5);

  const carried = measurePursuitPhase(phase, follow(phase, { carry: 40 }), area);
  assert.ok(Math.abs(carried.overshoot - 40) < 1, `overshoot ${carried.overshoot}`);

  const offset = measurePursuitPhase(phase, follow(phase, { offset: 25 }), area);
  assert.equal(offset.onTarget, 0);
  assert.ok(Math.abs(offset.rmsError - 25) < 1e-9);
  assert.equal(offset.lag, 0);
});

test("flags slow catch-up and run-on past reversals", () => {
  const phases = PURSUIT_PHASES.map((phase) => measurePursuitPhase(phase,
    follow(phase, phase.variant === "reactive" ? { delay: 200, carry: 50 } : {}), area));
  const result = analysePursuit(phases);
  assert.deepEqual(Object.keys(result.pursuit), ["precise", "reactive"]);
  assert.equal(result.notes.length, 2);
  assert.match(result.notes[0], /trailed the fast target/);
  assert.match(result.notes[1], /ran on past fast reversals/);
  assert.match(formatPursuit(result.pursuit)[0], /^Precise: on target 100%, RMS error 0.0 px, lag 0 ms/);
});
