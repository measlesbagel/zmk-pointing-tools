import test from "node:test";
import assert from "node:assert/strict";

import { compare } from "./bench.js";

test("the benchmark counts a guarded score as worse whatever its value", () => {
  const guard = "homing output is 0.34× the recorded settings'";
  /* Lower jitter would be better, but the guard says it can't be trusted. */
  assert.deepEqual(compare("homingJitterPercent", { value: 5, strokes: 6, guard },
    { value: 18, strokes: 6 }), { change: "worse", note: guard });
  assert.deepEqual(compare("homingJitterPercent", { value: 5, strokes: 6 },
    { value: 18, strokes: 6 }), { delta: -13, change: "better" });
});
