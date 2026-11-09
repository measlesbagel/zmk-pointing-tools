import test from "node:test";
import assert from "node:assert/strict";

import { RAIL_COURSES, analyseRail, finished, formatRail, locate, measureRail } from "./rail.js";

const down = RAIL_COURSES[0];
const corner = RAIL_COURSES[4];

test("locates a point on the nearest leg and knows the end", () => {
  assert.deepEqual(locate(down, { x: 5, y: 300 }), { leg: 0, along: 300, across: 5, distance: 5 });
  const onSecond = locate(corner, { x: 300, y: 690 });
  assert.equal(onSecond.leg, 1);
  assert.equal(onSecond.along, 300);
  assert.equal(onSecond.across, -10);
  assert.ok(!finished(corner, { x: 600, y: 700 }));
  assert.ok(finished(corner, { x: 690, y: 700 }));
});

/* Wheel events of `step` px along a course, with `leak` px on the other
 * axis every other event, pausing `pause` ms at a corner. */
function play(course, { step = 50, leak = 0, pause = 0, gap = 16 } = {}) {
  const events = [];
  let x = 0;
  let y = 0;
  let t = 0;
  course.legs.forEach((leg, index) => {
    if (index > 0) t += pause;
    for (let travelled = 0; travelled < leg.length; travelled += step) {
      const wobble = events.length % 2 ? leak : -leak;
      const dx = leg.dx * step + (leg.dx ? 0 : wobble);
      const dy = leg.dy * step + (leg.dy ? 0 : wobble);
      x += dx;
      y += dy;
      events.push({ t, dx, dy, x, y });
      t += gap;
    }
  });
  return { course, events, endedAt: t - gap };
}

test("clean straight rails have no leakage, exits or wrong locks", () => {
  const result = measureRail(play(down));
  assert.equal(result.reached, true);
  assert.equal(result.leakage, 0);
  assert.equal(result.wrongLocks, 0);
  assert.equal(result.exits, 0);
});

test("leakage, the corner switch and steering are measured", () => {
  const leaky = measureRail(play(down, { leak: 10 }));
  assert.ok(Math.abs(leaky.leakage - 0.2) < 1e-9);
  assert.ok(leaky.steering.y > 0);

  const slowTurn = measureRail(play(corner, { pause: 600 }));
  assert.equal(slowTurn.corner, true);
  assert.ok(slowTurn.switchDelay >= 600, `switch ${slowTurn.switchDelay}`);
});

test("flags leakage and slow corners", () => {
  const runs = RAIL_COURSES.map((course) => play(course, course.legs.length > 1 ? { pause: 500 } : { leak: 10 }));
  const result = analyseRail(runs);
  assert.equal(result.notes.length, 2);
  assert.match(result.notes[0], /leaked onto the other axis/);
  assert.match(result.notes[1], /400 ms or more to switch/);
  const lines = formatRail(result.rail);
  assert.match(lines[0], /^Straight rails: reached 100%, leakage 20.0%/);
  assert.match(lines[2], /^Steering: vertical [\d.]+, horizontal [\d.]+ per second/);
});

test("a step that jumps past the corner still starts the switch clock", () => {
  /* 60 px steps go from 660 straight to 720 on the first leg. */
  const result = measureRail(play(corner, { step: 60, pause: 300 }));
  assert.ok(Number.isFinite(result.switchDelay));
});
