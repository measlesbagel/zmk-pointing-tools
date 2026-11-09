import test from "node:test";
import assert from "node:assert/strict";

import { analyseStillHands, formatStillHands, measureClick, measureRest } from "./still-hands.js";

function click({ slip = 0, resumeAt = 250 } = {}) {
  const down = { t: 0, x: 100, y: 100 };
  const up = { t: 90, x: 100 + slip, y: 100 };
  const after = [{ t: 150, x: up.x + 1, y: 100 }, { t: 90 + resumeAt, x: up.x + 6, y: 100 }, { t: 600, x: up.x + 40, y: 100 }];
  return { down, held: slip ? [{ t: 40, x: 100 + slip / 2, y: 100 }] : [], up, after };
}

test("measures slip while held and the wait before moving again", () => {
  assert.deepEqual(measureClick(click()), { drift: 0, landedOff: 0, resume: 250 });
  const slipped = measureClick(click({ slip: 4, resumeAt: 500 }));
  assert.equal(slipped.drift, 4);
  assert.equal(slipped.resume, 500);
  assert.ok(Number.isNaN(measureClick({ ...click(), after: [] }).resume));
  assert.deepEqual(measureRest([{ t: 0, x: 0, y: 0 }, { t: 1, x: 3, y: 4 }, { t: 2, x: 0, y: 0 }]), { path: 10, net: 0 });
  assert.deepEqual(measureRest([]), { path: 0, net: 0 });
});

test("shows the trade-off and flags each side of it", () => {
  const steady = analyseStillHands(Array.from({ length: 8 }, () => click()), [{ t: 0, x: 5, y: 5 }]);
  assert.deepEqual(steady.notes, []);
  assert.deepEqual(formatStillHands(steady.stillHands), [
    "Clicks: 8, slipped on 0, median slip 0.0 px, median time to move again 250 ms",
    "Rest: moved 0.0 px in all, 0.0 px from where it started",
  ]);

  const slippy = analyseStillHands([...Array.from({ length: 3 }, () => click({ slip: 3 })), ...Array.from({ length: 5 }, () => click())], []);
  assert.match(slippy.notes[0], /slipped during a quarter/);

  const sluggish = analyseStillHands(Array.from({ length: 8 }, () => click({ resumeAt: 600 })),
    [{ t: 0, x: 0, y: 0 }, { t: 1, x: 3, y: 0 }]);
  assert.equal(sluggish.notes.length, 2);
  assert.match(sluggish.notes[0], /450 ms or more/);
  assert.match(sluggish.notes[1], /hands off the ball/);
});
