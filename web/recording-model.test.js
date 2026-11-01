import test from "node:test";
import assert from "node:assert/strict";

import { CAPTURE, encodeFrame, parseCaptureBatch, captureDecoder } from "./protocol.js";
import { buildSimulatorTuningTargets } from "./simulator-model.js";
import { createTuningProfile } from "./profile.js";
import { SimulatedCapture } from "./simulated-capture.js";
import {
  addRecords, compareScoreSets, createRecordingStats, deriveTruth, exportRecording, importRecording, resyncs,
} from "./recording-model.js";
import { captureToFixtures, decodeCaptureStream } from "../host/replay/capture-convert.js";
import { validateFixture } from "../host/replay/fixture.js";

/* Records seconds of simulated capture and returns the stream as received. */
async function simulate(ticks, change) {
  const targets = new Map(buildSimulatorTuningTargets().map((target) => [target.id, target]));
  const profiles = [createTuningProfile(targets)];
  const frames = [];
  const capture = new SimulatedCapture({ profile: () => createTuningProfile(targets), send: (frame) => frames.push(frame) });
  capture.describe(0);
  capture.control(0);
  for (let tick = 0; tick < ticks; tick += 1) {
    if (tick === change) {
      targets.get(13).parameters[0].current = 4;
      profiles.push(createTuningProfile(targets));
    }
    capture.tick(8);
  }
  capture.control(CAPTURE.NONE);
  await new Promise((resolve) => setTimeout(resolve, 20));
  const stream = new Uint8Array(frames.flatMap(({ type, payload }) => [...encodeFrame(type, payload)]));
  return { stream, frames, profiles, capture };
}

test("simulated capture drops during a link stall, resyncs, and converts with a profile per setting", async () => {
  const { stream, frames, profiles, capture } = await simulate(1250, 500);
  const stats = createRecordingStats();
  const decoder = captureDecoder();
  for (const frame of frames.filter(({ type }) => type === 0x92)) addRecords(stats, parseCaptureBatch(frame.payload, decoder));
  /* The stall 5 s in drops records; firmware counts exactly the gap. */
  assert.ok(capture.dropped > 0);
  assert.equal(stats.dropped, capture.dropped);
  assert.equal(stats.settingsChanges, 1);
  /* Start, the resync after the settings change, and the one after the drop. */
  assert.equal(resyncs(stats), 2);
  const { fixtures, warnings } = captureToFixtures(decodeCaptureStream(stream), profiles, { id: "sim" });
  assert.equal(fixtures.length, 3);
  assert.deepEqual(fixtures.map(({ processor }) => processor.profile === profiles[1]), [false, true, true]);
  assert.match(warnings.join("\n"), /records dropped/);
});

test("derived truth labels strokes from the recording and validates", async () => {
  const { stream, profiles } = await simulate(500);
  const [fixture] = captureToFixtures(decodeCaptureStream(stream), profiles).fixtures;
  const derived = deriveTruth(fixture);
  validateFixture(derived);
  const strokes = derived.truth.strokes;
  const activities = new Set(strokes.map(({ activity }) => activity));
  assert.deepEqual([...activities].sort(), ["aim", "flick", "slow-aim"]);
  /* Only the steady slow aims carry a speed: about 13 mm over 0.7 s. */
  for (const stroke of strokes.filter(({ speedMmPerS }) => speedMmPerS !== undefined)) {
    assert.equal(stroke.activity, "slow-aim");
    assert.ok(stroke.speedMmPerS > 14 && stroke.speedMmPerS < 26, String(stroke.speedMmPerS));
  }
  /* The path ends where the counts do. */
  const motion = derived.events.filter(([type]) => type === "motion");
  const first = strokes[0];
  let time = 0;
  const total = [0, 0];
  for (const event of derived.events) {
    time += event[1];
    if (event[0] === "motion" && time >= first.startUs && time < first.endUs) {
      total[0] += event[2];
      total[1] += event[3];
    }
  }
  assert.ok(motion.length > 0);
  assert.deepEqual(first.path.points.at(-1), total);
  /* Existing truth wins. */
  const labelled = { ...fixture, truth: { strokes: [] } };
  assert.equal(deriveTruth(labelled), labelled);
});

test("score comparisons report change and direction per metric", () => {
  const metrics = { flickLagMs: { higherIsBetter: false, unit: "ms" }, axisLockPercent: { higherIsBetter: true, unit: "%" },
    unintendedOutput: { higherIsBetter: false, unit: "" } };
  const rows = compareScoreSets([
    { scores: { flickLagMs: { value: 12, strokes: 3 }, axisLockPercent: { value: 80, strokes: 2 } } },
    { scores: { flickLagMs: { value: 14.5, strokes: 3 }, axisLockPercent: { value: 90, strokes: 2 } } },
  ], metrics);
  assert.deepEqual(rows.map(({ metric, change, better }) => [metric, change, better]), [
    ["flickLagMs", [0, 2.5], [null, false]],
    ["axisLockPercent", [0, 10], [null, true]],
    ["unintendedOutput", [null, null], [null, null]],
  ]);
  assert.equal(rows[2].truth, null);
});

test("a recording survives export and import byte for byte", async () => {
  const { stream, profiles } = await simulate(100);
  const recording = { id: "rec-1", label: "slow", pipeline: "cursor", createdAt: "2026-11-01T00:00:00Z", profiles, stream,
    stats: createRecordingStats() };
  const back = importRecording(JSON.parse(JSON.stringify(exportRecording(recording))));
  assert.deepEqual(back.stream, stream);
  assert.deepEqual(back.profiles, profiles);
  assert.throws(() => importRecording({ schema: "other" }), /expected a zmk-pointing-tools\/recording v1 file/);
});
