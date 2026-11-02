import test from "node:test";
import assert from "node:assert/strict";

import { CAPTURE, encodeFrame, parseCaptureBatch, captureDecoder } from "./protocol.js";
import { buildSimulatorTuningTargets } from "./simulator-model.js";
import { createTuningProfile } from "./profile.js";
import { SimulatedCapture } from "./simulated-capture.js";
import {
  addRecords, compareScoreSets, createRecordingStats, describeTruth, deviceGuide, exportRecording, importRecording,
  metricTruth, resyncs, streamStats,
} from "./recording-model.js";
import { captureToFixtures, decodeCaptureStream } from "../host/replay/capture-convert.js";

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

test("truth sources are counted per metric over the strokes that scored it", () => {
  const traces = [
    { id: "a", truth: { strokes: [{ id: "derived-1", source: "derived" }, { id: "typing-2", source: "typing" }] } },
    /* Untagged strokes count as derived, never as labelled. */
    { id: "b", truth: { strokes: [{ id: "s1", activity: "scroll", axis: "vertical" }, { id: "label-2", source: "label" }] } },
  ];
  const result = (strokes) => ({ fixtureResults: [{ id: "a", strokes: strokes.a }, { id: "b", strokes: strokes.b }] });
  const truth = metricTruth(traces, [
    result({ a: [{ id: "derived-1", metrics: { outputErrorPercent: 3 } }, { id: "typing-2", metrics: { unintendedOutput: 1 } }],
      b: [{ id: "s1", metrics: { axisLockPercent: 90, unintendedOutput: 0 } }, { id: "label-2", metrics: { axisLockPercent: 70 } }] }),
    /* The same stroke scored by a second set counts once. */
    result({ a: [{ id: "derived-1", metrics: { outputErrorPercent: 4 } }], b: [] }),
  ]);
  assert.deepEqual(truth, {
    outputErrorPercent: { derived: 1 },
    unintendedOutput: { typing: 1, derived: 1 },
    axisLockPercent: { derived: 1, label: 1 },
  });
  assert.equal(describeTruth(truth.unintendedOutput), "derived 1, typing heuristic 1");
  assert.equal(describeTruth(truth.axisLockPercent), "labels 1, derived 1");
  assert.match(describeTruth(undefined), /no stroke/);
});

test("score comparisons count a vanished metric or other strokes as worse, never better", () => {
  const metrics = { flickLagMs: { higherIsBetter: false, unit: "ms" }, axisLockPercent: { higherIsBetter: true, unit: "%" },
    outputErrorPercent: { higherIsBetter: false, unit: "%" }, speedErrorPercent: { higherIsBetter: false, unit: "%" },
    unintendedOutput: { higherIsBetter: false, unit: "" } };
  const rows = compareScoreSets([
    { scores: { flickLagMs: { value: 12, strokes: 3 }, axisLockPercent: { value: 80, strokes: 2 },
      outputErrorPercent: { value: 9, strokes: 10 }, speedErrorPercent: { value: 5, strokes: 4 } } },
    /* outputErrorPercent drops by losing strokes; speedErrorPercent vanishes. */
    { scores: { flickLagMs: { value: 14.5, strokes: 3 }, axisLockPercent: { value: 90, strokes: 2 },
      outputErrorPercent: { value: 2, strokes: 6 } } },
    { scores: { flickLagMs: { value: 12, strokes: 3 }, speedErrorPercent: { value: 1, strokes: 4 },
      unintendedOutput: { value: 3, strokes: 1 } } },
  ], metrics, { flickLagMs: { derived: 3 } });
  assert.deepEqual(rows.map(({ metric, change, better, note }) => [metric, change, better, note]), [
    ["flickLagMs", [null, 2.5, 0], [null, false, null], [null, null, null]],
    ["axisLockPercent", [null, 10, null], [null, true, false], [null, null, "missing"]],
    ["outputErrorPercent", [null, -7, null], [null, false, false], [null, "strokes 10→6", "missing"]],
    ["speedErrorPercent", [null, null, -4], [null, false, true], [null, "missing", null]],
    /* Nothing to compare against: no verdict. */
    ["unintendedOutput", [null, null, null], [null, null, null], [null, null, null]],
  ]);
  assert.deepEqual([rows[0].truth, rows[1].truth], [{ derived: 3 }, null]);
});

test("a recording survives export and import byte for byte", async () => {
  const { stream, profiles } = await simulate(100);
  const recording = { id: "rec-1", label: "slow", pipeline: "cursor", createdAt: "2026-11-01T00:00:00Z", profiles, stream,
    stats: createRecordingStats() };
  const back = importRecording(JSON.parse(JSON.stringify(exportRecording(recording))));
  assert.deepEqual(back.stream, stream);
  assert.deepEqual(back.profiles, profiles);
  assert.equal(back.createdAt, "2026-11-01T00:00:00.000Z");
  /* Counts come from the stream, not the file. */
  assert.deepEqual(back.stats, streamStats(stream));
  assert.ok(back.stats.frames > 0);
  assert.throws(() => importRecording({ schema: "other" }), /expected a zmk-pointing-tools\/recording v1 file/);
});

test("an imported recording keeps only checked schema fields, so a hostile file renders as data", async () => {
  const { stream, profiles } = await simulate(100);
  const file = {
    ...exportRecording({ id: "rec-1", label: "x", pipeline: "cursor", profiles, stream, stats: createRecordingStats() }),
    id: "<img src=x onerror=alert(1)>", label: '<b>“quoted”</b>"', createdAt: "not a date", durationMs: "<script>",
    stats: { frames: "<img src=x onerror=alert(2)>", dropped: {}, entered: [] }, firmwareDropped: "9",
    transport: { evil: true }, warnings: ["ok", 7, { html: "<b>" }], extra: "<svg onload=alert(3)>",
  };
  const back = importRecording(JSON.parse(JSON.stringify(file)));
  assert.match(back.id, /^rec-[a-z0-9]+-[a-z0-9]+$/);
  assert.equal(back.label, "<b>quoted</b>");
  assert.ok(!Number.isNaN(Date.parse(back.createdAt)));
  assert.equal(typeof back.durationMs, "number");
  assert.equal(typeof back.stats.frames, "number");
  assert.deepEqual([back.firmwareDropped, back.transport, back.warnings, "extra" in back], [null, null, ["ok"], false]);
  assert.throws(() => importRecording({ ...file, profiles: ["<b>"] }), /tuning profiles/);
});

test("a guided test's labels and pointer path move to the device's clock and convert", async () => {
  const { stream, profiles, capture } = await simulate(450);
  /* The simulated device started at 10 000 ms uptime; this page's clock
   * read 500 ms then, so device time is page time + 9 500 ms. */
  const guide = { test: "cursor-precision", acceleration: "off", pointer: [[500, 0, 0], [3700.04, 30, 40]],
    labels: [{ activity: "cursor", trial: 0, startPerf: 500.4, endPerf: 3700.6,
      target: { x: 30, y: 40, size: 8 }, start: { x: 0, y: 0 } },
    { activity: "cursor", trial: 1, startPerf: 3700.6, endPerf: 3700.9,
      target: { x: 1, y: 1, size: 8 }, start: { x: 0, y: 0 } }] };
  const guided = deviceGuide(guide, { roundTripMs: 6.04, offsetMs: 9500 });
  /* The second label rounds to nothing and is dropped. */
  assert.deepEqual(guided.labels.map(({ startMs, endMs, test }) => [startMs, endMs, test]),
    [[10000, 13201, "cursor-precision"]]);
  assert.deepEqual(guided.pointer, { acceleration: "off", samples: [[10000, 0, 0], [13200, 30, 40]] });
  assert.equal(guided.clockRoundTripMs, 6);
  const { fixtures } = captureToFixtures(decodeCaptureStream(stream), profiles,
    { labels: guided.labels, pointer: guided.pointer });
  const strokes = fixtures.flatMap((fixture) => fixture.truth?.strokes ?? []);
  assert.ok(strokes.length > 0 && strokes.every((stroke) => stroke.source === "label" && stroke.target.size === 8));
  assert.ok(capture.nowMs > 13200);
  /* Exported and imported, the guided test comes back. */
  const recording = { id: "r", label: "g", pipeline: "cursor", createdAt: new Date().toISOString(), durationMs: 1,
    transport: "simulator", endReason: "done", firmwareDropped: 0, warnings: [], profiles, stats: {}, stream, guided };
  assert.deepEqual(importRecording(JSON.parse(JSON.stringify(exportRecording(recording)))).guided, guided);
  assert.equal(importRecording(JSON.parse(JSON.stringify(exportRecording({ ...recording, guided: null })))).guided, null);
});

test("an unranked metric reports its change but is never better or worse", () => {
  const sets = [{ scores: { bias: { value: -10, strokes: 6 } } }, { scores: { bias: { value: 5, strokes: 6 } } }];
  const [row] = compareScoreSets(sets, { bias: { higherIsBetter: false, unit: "%", ranked: false } });
  assert.deepEqual([row.change[1], row.better[1], row.ranked], [15, null, false]);
});
