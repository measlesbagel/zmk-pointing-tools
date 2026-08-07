import test from "node:test";
import assert from "node:assert/strict";

import {
  FIXTURE_SCHEMA,
  encodeRunnerInput,
  importCapture,
  validateFixture,
} from "./fixture.js";

const textFixture = {
  schema: FIXTURE_SCHEMA,
  version: 1,
  id: "text-test",
  processor: {
    kind: "text-navigation",
    settings: {
      horizontalThreshold: 75,
      verticalThreshold: 75,
      idleTimeoutMs: 40,
      activationDistance: 35,
      engageRatioPercent: 150,
    },
  },
  events: [["motion", 0, 20, -2], ["motion", 8, 30, 1]],
};

test("validates fixtures and encodes deterministic runner input", () => {
  assert.equal(validateFixture(textFixture), textFixture);
  assert.equal(encodeRunnerInput(textFixture), "C 75 75 40 35 150\nM 0 20 -2\nM 8 30 1\n");
  assert.throws(
    () => validateFixture({ ...textFixture, version: 2 }),
    /expected zmk-pointing-tools\/trace-fixture v1/,
  );
});

test("imports one stream and normalizes uptime to frame deltas", () => {
  const capture = {
    exportedAt: "2026-01-01T00:00:00.000Z",
    streams: [{ key: "0:0", label: "Left raw" }],
    samples: [
      { key: "1:0", timestamp: 90, x: 9, y: 9 },
      { key: "0:0", timestamp: 100, x: 1, y: -2 },
      { deviceId: 0, stage: 0, timestamp: 108, x: 3, y: -4 },
    ],
  };
  const imported = importCapture(capture, { ...textFixture, expect: {} }, {
    stream: "0:0",
    start: 0,
    count: undefined,
    source: "capture.json",
  });
  assert.deepEqual(imported.events, [["motion", 0, 1, -2], ["motion", 8, 3, -4]]);
  assert.equal(imported.metadata.capture.frameCount, 2);
  assert.equal("expect" in imported, false);
});
