import test from "node:test";
import assert from "node:assert/strict";

import { STATE, TARGET_KIND } from "./protocol.js";
import { buildSimulatorEvent, buildSimulatorState, SIMULATED_STAGES } from "./simulator-model.js";

test("simulator models the smoke keymap's stage targets only", () => {
  assert.deepEqual(SIMULATED_STAGES.map(({ id }) => id), [0, 1, 2, 3, 4, 5, 6, 7, 8, 9]);
  assert.deepEqual(SIMULATED_STAGES.map(({ label }) => label), [
    "resolution-normalize",
    "motion-gate",
    "cursor-transfer",
    "cursor-quantizer",
    "scroll-resolution-normalize",
    "scroll-axis-intent",
    "scroll-constraint",
    "scroll-batcher",
    "text-resolution-normalize",
    "text-nav",
  ]);
  for (const event of [
    buildSimulatorEvent(1, 1, 0, 0, 1, [1, 0]),
    buildSimulatorEvent(5, 1, STATE.FLAG_INTENT_CHANGED, 8, 2, [3, 2]),
    buildSimulatorEvent(7, 2, STATE.FLAG_OUTPUT, 16, 3, [4, 3]),
  ]) {
    assert.equal(event.targetKind, TARGET_KIND.PIPELINE_STAGE);
  }
});

test("simulator status enumerates the stage targets at the off level", () => {
  const status = buildSimulatorState();
  assert.equal(status.schemaVersion, 2);
  assert.equal(status.levels.size, SIMULATED_STAGES.length);
  assert.equal(status.labels.size, SIMULATED_STAGES.length);
  for (const { id, label } of SIMULATED_STAGES) {
    assert.equal(status.levels.get(id), STATE.OFF);
    assert.equal(status.labels.get(id), label);
  }
});
