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
    () => validateFixture({ ...textFixture, version: 3 }),
    /expected zmk-pointing-tools\/trace-fixture v1 or v2/,
  );
});

const cursorFixture = {
  schema: FIXTURE_SCHEMA,
  version: 2,
  id: "cursor-v2",
  source: { device: "right-ball", location: "central", cpi: 684, span: "sensor" },
  processor: {
    kind: "cursor-pipeline",
    link: "ble",
    stages: [
      { type: "normalize", stableId: "n" },
      { type: "transfer", stableId: "t",
        settings: { "scale-multiplier": 1, "scale-divisor": 1 } },
      { type: "quantizer", stableId: "q" },
    ],
    profile: { schema: "zmk-pointing-tools/tuning-profile", version: 1, targets: [
      { stableId: "t", parameters: [{ key: "scale-multiplier", value: 2 }] },
      { stableId: "elsewhere", parameters: [{ key: "scale-divisor", value: 9 }] },
    ] },
  },
  events: [["motion", 1500, 1, -1, 1200], ["keypress", 250], ["motion", 2250, 2, 0]],
};

test("encodes v2 cursor fixtures with microsecond times, spans, and profiles", () => {
  assert.equal(validateFixture(cursorFixture), cursorFixture);
  const override = { schema: "zmk-pointing-tools/tuning-profile", version: 1, targets: [
    { stableId: "t", parameters: [{ key: "scale-divisor", value: 3 }] }] };
  /* Inline settings, then the embedded profile, then the override. */
  assert.equal(encodeRunnerInput(cursorFixture, override), [
    "R 684", "L ble", "S normalize n", "S transfer t", "P scale-multiplier 2",
    "P scale-divisor 3", "S quantizer q", "E",
    "M 1500 1 -1 1200", "K 1750", "M 4000 2 0 0", ""].join("\n"));
  /* Without a sensor span on the source, spans stay out of the pipeline. */
  const noSpan = { ...cursorFixture, source: { ...cursorFixture.source, span: "none" } };
  assert.match(encodeRunnerInput(noSpan), /\nM 1500 1 -1 0\n/);
});

test("v2 non-cursor fixtures send whole-millisecond arrival times", () => {
  const scroll = { ...composedScrollFixture, version: 2,
    source: { device: "left-ball", location: "peripheral", cpi: 114, span: "none" },
    processor: { ...composedScrollFixture.processor,
      settings: { ...composedScrollFixture.processor.settings, cpi: undefined } },
    events: [["motion", 1999, 10, 1], ["motion", 2, 3, 0]] };
  assert.equal(encodeRunnerInput(validateFixture(scroll)),
    "C 114 3445 16 120 0 0 300 180 581 64 1\nM 1 10 1\nM 2 3 0\n");
  assert.throws(() => validateFixture({ ...scroll, truth: { strokes: [
    { id: "s", activity: "scroll", intended: true, startUs: 5, endUs: 5 }] } }), /endUs/);
});
