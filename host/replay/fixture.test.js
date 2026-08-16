import test from "node:test";
import assert from "node:assert/strict";

import {
  FIXTURE_SCHEMA,
  encodeRunnerInput,
  validateFixture,
} from "./fixture.js";

const textFixture = {
  schema: FIXTURE_SCHEMA,
  version: 1,
  id: "text-test",
  processor: {
    kind: "composed-text",
    settings: {
      cpi: 700,
      engageRatioPercent: 150,
      releaseRatioPercent: 90,
      activationDistanceMicrometers: 1270,
      intentWindowMs: 32,
      idleTimeoutMs: 40,
      discardUnclassified: true,
      horizontalThresholdMicrometers: 2721,
      verticalThresholdMicrometers: 2721,
      mapperIdleTimeoutMs: 40,
    },
  },
  events: [["motion", 0, 20, -2], ["motion", 8, 30, 1]],
};

const composedScrollFixture = {
  schema: FIXTURE_SCHEMA,
  version: 1,
  id: "composed-scroll-test",
  processor: {
    kind: "composed-scroll",
    policy: "adaptive",
    settings: {
      cpi: 700, stepsPerMeter: 3445, reportIntervalMs: 16, idleTimeoutMs: 120,
      suppressAfterKeypressMs: 0, discardUnclassified: false, engageRatioPercent: 300,
      releaseRatioPercent: 180, activationDistanceMicrometers: 581, intentWindowMs: 64,
    },
  },
  events: [["motion", 0, 10, 1]],
};

test("accepts composed fixture kinds with legacy settings shapes", () => {
  assert.equal(validateFixture(composedScrollFixture), composedScrollFixture);
  assert.equal(encodeRunnerInput(composedScrollFixture),
    "C 700 3445 16 120 0 0 300 180 581 64 1\nM 0 10 1\n");
  assert.throws(() => validateFixture({ ...composedScrollFixture, processor: {
    ...composedScrollFixture.processor, policy: "diagonal" } }), /unknown axis policy/);
});

test("validates fixtures and encodes deterministic runner input", () => {
  assert.equal(validateFixture(textFixture), textFixture);
  assert.equal(encodeRunnerInput(textFixture),
    "C 700 150 90 1270 32 40 1 2721 2721 40\nM 0 20 -2\nM 8 30 1\n");
  assert.throws(
    () => validateFixture({ ...textFixture, version: 2 }),
    /expected zmk-pointing-tools\/trace-fixture v1/,
  );
});
