import test from "node:test";
import assert from "node:assert/strict";

import { STATE, TARGET_KIND } from "./protocol.js";
import { analyzeCalibration, parseStageFrame } from "./calibration-model.js";
import {
  buildSimulatorEvent,
  buildSimulatorFrame,
  buildSimulatorState,
  SIMULATED_STAGES,
} from "./simulator-model.js";

test("simulator models the smoke keymap's stage targets only", () => {
  assert.deepEqual(SIMULATED_STAGES.map(({ id }) => id), [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11]);
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
    "text-axis-intent",
    "text-constraint",
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

test("simulated normalize frames calibrate like a real recording", () => {
  assert.equal(buildSimulatorFrame("motion-gate", 0), undefined);
  const frames = [];
  for (let tick = 0; tick < 900; tick += 1) {
    const values = buildSimulatorFrame("scroll-resolution-normalize", tick);
    if (values) {
      frames.push(parseStageFrame(buildSimulatorEvent(4, STATE.EVENT_FRAME, 0, tick * 8, tick, values)));
    }
  }
  const analysis = analyzeCalibration(frames, { mode: "scroll", desiredOutput: 10 });
  assert.equal(analysis.summary.gestureCount, 6);
  assert.equal(analysis.summary.axis, "y");
  assert.equal(analysis.summary.missingFrames, 0);
  assert.ok(analysis.recommendation.parameters[0].value > 300);
});
