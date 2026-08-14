import test from "node:test";
import assert from "node:assert/strict";

import {
  createRun,
  exportRun,
  finishRun,
  recordClick,
  recordPointer,
  recordWheel,
} from "./playground-model.js";

test("records repeatable activity metrics and exports only associated telemetry", () => {
  const run = createRun("cursor-targets", { nextSequence: 10, tuningProfile: { version: 1 } }, 1000);
  recordWheel(run, -2, 4);
  let previous = recordPointer(run, undefined, { x: 1, y: 1 });
  previous = recordPointer(run, previous, { x: 4, y: 5 });
  recordClick(run, false);
  recordClick(run, true);
  finishRun(run, true, 1250);

  const exported = exportRun(run, {
    stateEvents: [{ sequence: 8 }, { sequence: 12 }],
    queueCapacity: 64,
  });
  assert.equal(previous.x, 4);
  assert.equal(exported.durationMs, 250);
  assert.equal(exported.metrics.pointerDistance, 5);
  assert.equal(exported.metrics.clicks, 2);
  assert.equal(exported.metrics.errors, 1);
  assert.deepEqual(exported.telemetry.stateEvents.map(({ sequence }) => sequence), [12]);
  assert.equal("startedMs" in exported, false);
});
