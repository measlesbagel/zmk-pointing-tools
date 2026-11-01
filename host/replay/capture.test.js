import test from "node:test";
import assert from "node:assert/strict";

import { readdirSync, readFileSync } from "node:fs";

import { captureToFixtures, decodeCaptureStream } from "./capture.js";
import { deriveTruthStrokes } from "./capture-convert.js";
import { encodeRunnerInput } from "./fixture.js";
import { CAPTURE, MESSAGE, captureDecoder, captureSettingsHash, encodeFrame, parseCaptureBatch }
  from "../../web/protocol.js";

const stages = [
  { strategy: "orthogonal-orientation", stableId: "o",
    fixed: { "swap-xy": 0, "invert-x": 1, "invert-y": 0 } },
  { strategy: "resolution-normalize-mm", stableId: "n", fixed: {} },
  { strategy: "cursor-transfer", stableId: "t", fixed: {} },
  { strategy: "cursor-quantizer", stableId: "q", fixed: {} },
];
const profile = {
  schema: "zmk-pointing-tools/tuning-profile",
  version: 1,
  targets: [{ stableId: "t", parameters: [{ key: "scale-multiplier", value: 3 },
    { key: "scale-divisor", value: 2 }] }],
};
const description = { pipelineId: 0, stableId: "right-cursor", stages,
  settingsHash: captureSettingsHash(profile, stages.map((stage) => stage.stableId)) };

let sequence = 0;
const entered = (settingsHash = description.settingsHash) =>
  ({ sequence: sequence++, kind: CAPTURE.RECORD_ENTERED, settingsHash });
const source = (cpi = 800, link = CAPTURE.LINK_BLE) =>
  ({ sequence: sequence++, kind: CAPTURE.RECORD_SOURCE, link, sourceId: 2, cpi });
const frame = (timeMs, timeUs, x, flags = 0, spanUs = 1000) =>
  ({ sequence: sequence++, kind: CAPTURE.RECORD_FRAME, flags, timeMs, timeUs, x, y: 0, spanUs });
const timed = (kind, timeMs) => ({ sequence: sequence++, kind, timeMs });

test("a recording becomes a trace with recorded timing and settings", () => {
  sequence = 0;
  const records = [entered(), timed(CAPTURE.RECORD_KEYPRESS, 4990), source(),
    frame(5000, 250, 3), timed(CAPTURE.RECORD_FLUSH, 5000), frame(5002, 999, -1, 0x80, 30000)];
  const { fixtures, warnings } = captureToFixtures({ description, records }, profile);
  assert.deepEqual(warnings, []);
  const [fixture] = fixtures;
  assert.deepEqual(fixture.source, { device: "source-2", location: "central", cpi: 800,
    span: "sensor" });
  assert.equal(fixture.processor.link, "ble");
  assert.deepEqual(fixture.processor.stages[0].settings, { "swap-xy": 0, "invert-x": 1,
    "invert-y": 0 });
  assert.deepEqual(fixture.processor.stages[2].settings, { "scale-multiplier": 3,
    "scale-divisor": 2 });
  /* Without a keypress guard the key press can't matter, so it is left
   * out; the flush keeps its millisecond; a clipped span restarts the
   * speed estimate on replay. */
  assert.deepEqual(fixture.events, [["motion", 250, 3, 0, 1000],
    ["flush", 0], ["motion", 2749, -1, 0, 40000]]);
  assert.match(encodeRunnerInput(fixture), /^F\nE\nM 250 3 0 1000\nF 250\nM 2999/m);
});

test("drops, settings changes, and pipeline switches end a trace", () => {
  sequence = 0;
  const records = [entered(), source(), frame(10, 0, 1), frame(11, 0, 1)];
  sequence += 2; /* two records dropped */
  records.push(frame(12, 0, 1), entered(), source(), frame(20, 0, 1),
    { sequence: sequence++, kind: CAPTURE.RECORD_SETTINGS_CHANGED }, frame(21, 0, 1),
    { sequence: sequence++, kind: CAPTURE.RECORD_LEFT }, entered(), source(400), frame(40, 0, 2),
    source(800), frame(41, 0, 2));
  const { fixtures, warnings } = captureToFixtures({ description, records }, profile,
    { id: "split" });
  assert.deepEqual(fixtures.map((fixture) => fixture.id), ["split-1", "split-2", "split-3"]);
  assert.deepEqual(fixtures.map((fixture) => fixture.events.length), [2, 1, 1]);
  assert.equal(fixtures[2].source.cpi, 400);
  assert.deepEqual(warnings, ["2 records dropped before 6",
    "trace 2 ends early: settings changed while recording",
    "trace 3 ends early: the source, resolution, or report link changed"]);
});

test("a drop ends a trace and the resync at the next rest starts a full one", () => {
  sequence = 0;
  const records = [entered(), source()];
  for (let i = 0; i < 10; i += 1) records.push(frame(100 + i, 0, 1));
  sequence += 3; /* three records dropped */
  /* Frames the pipeline ran before it rested and resynced don't replay. */
  records.push(frame(113, 0, 1), frame(114, 0, 1), entered(), source());
  for (let i = 0; i < 1000; i += 1) records.push(frame(200 + i, 0, 1));
  const { fixtures, warnings } = captureToFixtures({ description, records }, profile);
  assert.deepEqual(fixtures.map((fixture) => fixture.events.length), [10, 1000]);
  assert.equal(fixtures[1].metadata.capture.startMs, 200);
  assert.deepEqual(warnings, ["3 records dropped before 15"]);
});

test("each trace takes the profile its settings hash names", () => {
  sequence = 0;
  const previewed = structuredClone(profile);
  previewed.targets[0].parameters[0].value = 4;
  const previewedHash = captureSettingsHash(previewed, stages.map((stage) => stage.stableId));
  /* An A/B preview mid-recording: firmware resyncs at the next rest with
   * the new settings' hash on the entered record. */
  const records = [entered(), source(), frame(10, 0, 1), frame(11, 0, 1),
    { sequence: sequence++, kind: CAPTURE.RECORD_SETTINGS_CHANGED }, frame(12, 0, 1),
    entered(previewedHash), source(), frame(60, 0, 1)];
  const both = captureToFixtures({ description, records }, [previewed, profile]);
  assert.deepEqual(both.fixtures.map((fixture) => fixture.processor.profile), [profile, previewed]);
  assert.equal(both.fixtures[1].processor.stages[2].settings["scale-multiplier"], 4);
  assert.deepEqual(both.fixtures.map((fixture) => fixture.events.length), [2, 1]);
  /* A trace no profile matches is skipped, the rest still convert. */
  const one = captureToFixtures({ description, records }, profile);
  assert.deepEqual(one.fixtures.map((fixture) => fixture.events.length), [2]);
  assert.match(one.warnings.at(-1), new RegExp(`trace from record 8 skipped: .*` +
    `settings hash ${previewedHash.toString(16)}`));
});

test("events recorded after a later one carry the millisecond their stages saw", () => {
  sequence = 0;
  const gated = { ...description, stages: [...stages.slice(0, 2),
    { strategy: "coherent-displacement", stableId: "g",
      fixed: { "suppress-after-keypress-ms": 40 } }, ...stages.slice(2)] };
  gated.settingsHash = captureSettingsHash(profile, gated.stages.map((stage) => stage.stableId));
  /* The first key press is past the guard's window when motion starts. */
  const records = [entered(), timed(CAPTURE.RECORD_KEYPRESS, 900), source(),
    timed(CAPTURE.RECORD_KEYPRESS, 980), frame(1000, 500, 1),
    timed(CAPTURE.RECORD_FLUSH, 1003), frame(1001, 700, 2), frame(1004, 100, 3)];
  const { fixtures, warnings } = captureToFixtures({ description: gated, records }, profile);
  assert.deepEqual(warnings, []);
  assert.deepEqual(fixtures[0].events, [["keypress", 0], ["motion", 20500, 1, 0, 1000],
    ["flush", 2500], ["motion", 0, 2, 0, 1000, 2], ["motion", 1100, 3, 0, 1000]]);
  assert.match(encodeRunnerInput(fixtures[0]), /\nM 23000 2 0 1000 2\n/);
});

test("warns when the recording's first records were dropped", () => {
  sequence = 5;
  const { warnings } = captureToFixtures({ description, records: [entered(), source(),
    frame(10, 0, 1)] }, profile);
  assert.deepEqual(warnings, ["the recording starts at record 5; earlier records were dropped"]);
});

/* Derived truth against the benchmark's real truth: strip it from every
 * benchmark fixture, derive it from the motion, and compare stroke by
 * stroke. A derived stroke matches the real one it overlaps most. */
test("derived truth finds the benchmark's strokes and speeds", () => {
  const directory = new URL("../bench/fixtures/", import.meta.url);
  const counts = { flick: [0, 0], tremor: [0, 0], typing: [0, 0], steady: [0, 0] };
  for (const set of ["tune", "holdout"]) {
    for (const name of readdirSync(new URL(set, directory))) {
      const fixture = JSON.parse(readFileSync(new URL(`${set}/${name}`, directory), "utf8"));
      const derived = deriveTruthStrokes({ ...fixture, truth: undefined });
      for (const real of fixture.truth.strokes) {
        const overlap = (stroke) => Math.max(0, Math.min(real.endUs, stroke.endUs) -
          Math.max(real.startUs, stroke.startUs));
        const match = derived.reduce((best, stroke) =>
          (overlap(stroke) > (best ? overlap(best) : 0) ? stroke : best), null);
        const tally = (kind, ok) => {
          counts[kind][0] += ok ? 1 : 0;
          counts[kind][1] += 1;
        };
        if (real.activity === "flick") tally("flick", match?.activity === "flick");
        if (real.activity === "tremor") tally("tremor", match?.intended === false);
        if (real.activity === "post-typing" && !real.intended) {
          tally("typing", match?.intended === false);
        }
        if (real.activity === "slow-aim") {
          tally("steady", match?.speedMmPerS !== undefined &&
            Math.abs(match.speedMmPerS - real.speedMmPerS) <= 0.06 * real.speedMmPerS);
        }
      }
    }
  }
  /* Every flick, rest tremor, and typing burst, and every slow stroke's
   * speed within 6 %. */
  assert.deepEqual(counts, { flick: [12, 12], tremor: [4, 4], typing: [8, 8],
    steady: [22, 22] });
});

test("only stages the chain runner knows convert", () => {
  const other = { ...description, stages: [{ strategy: "sensor-pll", stableId: "a", fixed: {} }] };
  assert.throws(() => captureToFixtures({ description: other, records: [] }, profile),
    /doesn't replay/);
});

test("a scroll pipeline converts, with labelled, typing, and unlabelled truth", () => {
  const guard = { "suppress-after-keypress-ms": 40 };
  const scrollStages = [
    { strategy: "resolution-normalize-mm", stableId: "n", fixed: {} },
    { strategy: "axis-intent", stableId: "i", fixed: { policy: 1, ...guard } },
    { strategy: "axis-constraint", stableId: "c", fixed: guard },
    { strategy: "scroll-batcher", stableId: "b", fixed: guard },
  ];
  const parameters = (entries) => entries.map(([key, value]) => ({ key, value }));
  const scrollProfile = { schema: "zmk-pointing-tools/tuning-profile", version: 1, targets: [
    { stableId: "i", parameters: parameters([["engage-ratio", 200], ["release-ratio", 150],
      ["activation-distance", 400], ["window", 100], ["idle-timeout", 30]]) },
    { stableId: "c", parameters: parameters([["discard-unclassified", 0],
      ["idle-timeout", 30], ["fold-interval", 20]]) },
    { stableId: "b", parameters: parameters([["steps-per-meter", 3000],
      ["report-interval", 20]]) },
  ] };
  const scroll = { pipelineId: 1, stableId: "left-scroll", stages: scrollStages,
    settingsHash: captureSettingsHash(scrollProfile, scrollStages.map((stage) => stage.stableId)) };
  sequence = 0;
  const records = [entered(scroll.settingsHash), timed(CAPTURE.RECORD_KEYPRESS, 4990),
    source(800, CAPTURE.LINK_NONE), frame(5000, 0, 1), frame(5010, 0, 1), frame(5020, 0, 1),
    frame(5060, 0, 1), timed(CAPTURE.RECORD_KEYPRESS, 5100), frame(5200, 0, 1)];
  const labels = [{ startMs: 5000, endMs: 5050, activity: "scroll", axis: "vertical" },
    { startMs: 4000, endMs: 4100, activity: "scroll", axis: "horizontal" },
    { startMs: 5052, endMs: 5058, activity: "scroll", axis: "horizontal" },
    { startMs: 5150, endMs: 5400, activity: "scroll", axis: "horizontal" }];
  const { fixtures, warnings } = captureToFixtures({ description: scroll, records },
    scrollProfile, { labels });
  assert.deepEqual(warnings, ["label 2 covers no motion in capture-left-scroll",
    "label 3 is clamped to capture-left-scroll, which runs from 4990 ms for 211 ms",
    "label 1 covers no trace and is dropped"]);
  const [fixture] = fixtures;
  assert.equal(fixture.processor.kind, "scroll-pipeline");
  assert.equal(fixture.processor.link, undefined);
  assert.equal(fixture.metadata.mode, "scroll");
  assert.deepEqual(fixture.processor.stages[1].settings, { "engage-ratio": 200,
    "release-ratio": 150, "activation-distance": 400, window: 100, "idle-timeout": 30,
    policy: 1, "suppress-after-keypress-ms": 40 });
  /* The guard window comes from the scroll stages, so the key press before
   * the first frame stays. */
  assert.deepEqual(fixture.events.map((event) => event.slice(0, 2)), [["keypress", 0],
    ["motion", 10000], ["motion", 10000], ["motion", 10000], ["motion", 40000],
    ["keypress", 40000], ["motion", 100000]]);
  assert.match(encodeRunnerInput(fixture), /^L usb\nS normalize n\nS axis-intent i\n/m);
  /* By default only labels are truth; the last runs past the trace's last
   * event, so it is clamped there. */
  assert.deepEqual(fixture.truth.strokes.map(({ id, startUs, endUs, axis, intended }) =>
    [id, startUs, endUs, axis, intended]), [["label-1", 10000, 60000, "vertical", true],
    ["label-2", 160000, 210001, "horizontal", true]]);
  assert.equal(fixture.truth.derived, undefined);
  /* With derive, motion outside the labels is typing: the frame between
   * them comes within 150 ms of a key press. */
  const derived = captureToFixtures({ description: scroll, records }, scrollProfile,
    { labels, truth: "derive" }).fixtures[0].truth;
  assert.equal(derived.derived, true);
  assert.deepEqual(derived.strokes.map(({ id, activity, intended, source }) =>
    [id, activity, intended, source]), [["label-1", "scroll", true, "label"],
    ["typing-2", "post-typing", false, "typing"], ["label-3", "scroll", true, "label"]]);
  assert.throws(() => captureToFixtures({ description: scroll, records }, scrollProfile,
    { labels: [{ startMs: 1, endMs: 1, activity: "scroll", axis: "vertical" }] }), /label 0/);
});

test("parses capture batches with consecutive sequences", () => {
  const payload = Uint8Array.of(7, 0, 0, 0, 3,
    CAPTURE.RECORD_SOURCE, 1, 2, 0, 0x20, 0x03,
    CAPTURE.RECORD_FRAME, 0x80, 10, 0, 0, 0, 0xe7, 0x03, 0xff, 0xff, 0xff, 0xff, 2, 0, 0, 0,
    0x30, 0x75, 0, 0,
    CAPTURE.RECORD_LEFT);
  assert.deepEqual(parseCaptureBatch(payload, captureDecoder()), [
    { sequence: 7, kind: CAPTURE.RECORD_SOURCE, link: 1, sourceId: 2, cpi: 800 },
    { sequence: 8, kind: CAPTURE.RECORD_FRAME, flags: 0x80, timeMs: 10, timeUs: 999, x: -1,
      y: 2, spanUs: 30000 },
    { sequence: 9, kind: CAPTURE.RECORD_LEFT },
  ]);
  assert.throws(() => parseCaptureBatch(Uint8Array.of(0, 0, 0, 0, 1, 99), captureDecoder()),
    /Unknown capture record/);
});

test("streams from firmware before protocol v8 are refused", () => {
  assert.throws(() => decodeCaptureStream(encodeFrame(MESSAGE.DESCRIBE_RESPONSE, Uint8Array.of(7))),
    /Firmware protocol 7 does not match tuner protocol 8/);
});

test("decodes delta frames against the previous frame, across batches", () => {
  const state = captureDecoder();
  state.nextSequence = 7;
  const first = Uint8Array.of(7, 0, 0, 0, 2,
    /* Whole: flags 0, 10 ms + 900 us, x 5, y -3, span 1000 us. */
    CAPTURE.RECORD_FRAME, 0, 10, 0, 0, 0, 0x84, 0x03, 5, 0, 0, 0, 0xfd, 0xff, 0xff, 0xff,
    0xe8, 0x03, 0, 0,
    /* +1250 us, x -2, y 1, span = elapsed. */
    0x81, 0xc4, 0x13, 0x03, 0x02);
  const second = Uint8Array.of(9, 0, 0, 0, 2,
    /* -1 us, x 200, y 0, flags 0x80, span 30000 given whole. */
    0x87, 0x01, 0x90, 0x03, 0x00, 0x80, 0xb0, 0xea, 0x01,
    /* +1000 us, no motion, span 3 us short of elapsed, flags kept. */
    0x82, 0xd0, 0x0f, 0x00, 0x00, 0x05);
  const frame = (sequence, timeMs, timeUs, x, y, flags, spanUs) =>
    ({ sequence, kind: CAPTURE.RECORD_FRAME, flags, timeMs, timeUs, x, y, spanUs });
  assert.deepEqual([...parseCaptureBatch(first, state), ...parseCaptureBatch(second, state)], [
    frame(7, 10, 900, 5, -3, 0, 1000), frame(8, 12, 150, -2, 1, 0, 1250),
    frame(9, 12, 149, 200, 0, 0x80, 30000), frame(10, 13, 149, 0, 0, 0x80, 997)]);
  /* After a gap, firmware sends the next frame whole. A delta with nothing
   * to follow is skipped, which the converter reads as a gap, and decoding
   * picks up again at the next whole frame. */
  const whole = [CAPTURE.RECORD_FRAME, 0, 50, 0, 0, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0,
    0xe8, 0x03, 0, 0];
  assert.deepEqual(parseCaptureBatch(Uint8Array.of(20, 0, 0, 0, 3,
    0x80, 0, 0, 0, ...whole, 0x80, 0xd0, 0x0f, 0x02, 0x02), state), [
    frame(21, 50, 0, 1, 1, 0, 1000), frame(22, 51, 0, 1, 1, 0, 1000)]);
  assert.equal(state.skipped, 1);
  /* An entered record starts a trace, which starts with a whole frame. */
  assert.deepEqual(parseCaptureBatch(Uint8Array.of(23, 0, 0, 0, 2,
    CAPTURE.RECORD_ENTERED, 0, 0, 0, 0, 0x80, 0, 0, 0), state), [
    { sequence: 23, kind: CAPTURE.RECORD_ENTERED, settingsHash: 0 }]);
  assert.equal(state.skipped, 2);
});

test("delta frame fields are int32: a fifth varint byte holds only the top 4 bits", () => {
  const batch = (...bytes) => {
    const state = captureDecoder();
    return parseCaptureBatch(Uint8Array.of(0, 0, 0, 0, 2, CAPTURE.RECORD_FRAME, 0, 0, 0, 0, 0,
      0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, ...bytes), state)[1];
  };
  /* zigzag 0xffffffff is INT32_MIN, 0xfffffffe INT32_MAX. */
  const max = [0xff, 0xff, 0xff, 0xff, 0x0f];
  assert.deepEqual([batch(0x80, 0, ...max, 0xfe, 0xff, 0xff, 0xff, 0x0f)].map(({ x, y }) => [x, y]),
    [[-(2 ** 31), 2 ** 31 - 1]]);
  assert.throws(() => batch(0x80, 0, 0xff, 0xff, 0xff, 0xff, 0x10, 0), /Malformed varint/);
  assert.throws(() => batch(0x80, 0, 0x80, 0x80, 0x80, 0x80, 0x80, 0), /Malformed varint/);
});

test("the converter the tuner loads has no Node imports", async () => {
  const { readFile } = await import("node:fs/promises");
  const source = await readFile(new URL("./capture-convert.js", import.meta.url), "utf8");
  assert.doesNotMatch(source, /from "node:/);
});
