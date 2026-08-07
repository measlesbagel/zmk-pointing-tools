import test from "node:test";
import assert from "node:assert/strict";

import { compareMetrics, parseTextMetrics } from "./metrics.js";

test("summarizes text runner output and reports snapshot differences", () => {
  const fixture = { id: "text", events: [["motion", 0, 10, 0], ["motion", 8, 20, 0]] };
  const output = [
    "D\t0\t0\t10\t0\t-1",
    "D\t8\t2\t0\t0\t1",
    "O\t8\t1",
  ].join("\n");
  const metrics = parseTextMetrics(output, fixture);
  assert.equal(metrics.inputFrames, 2);
  assert.deepEqual(metrics.steps, { left: 0, right: 1, up: 0, down: 0, total: 1 });
  assert.deepEqual(compareMetrics({ outputFrames: 2 }, metrics), [
    "outputFrames: expected 2, got 1",
  ]);
});
