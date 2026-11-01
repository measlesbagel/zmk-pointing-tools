import test from "node:test";
import assert from "node:assert/strict";

import { captureToFixtures } from "./capture.js";
import { encodeRunnerInput } from "./fixture.js";
import { CAPTURE, parseCaptureBatch } from "../../web/protocol.js";

const description = {
  pipelineId: 0,
  stableId: "right-cursor",
  stages: [
    { strategy: "orthogonal-orientation", stableId: "o",
      fixed: { "swap-xy": 0, "invert-x": 1, "invert-y": 0 } },
    { strategy: "resolution-normalize-mm", stableId: "n", fixed: {} },
    { strategy: "cursor-transfer", stableId: "t", fixed: {} },
    { strategy: "cursor-quantizer", stableId: "q", fixed: {} },
  ],
};
const profile = {
  schema: "zmk-pointing-tools/tuning-profile",
  version: 1,
  targets: [{ stableId: "t", parameters: [{ key: "scale-multiplier", value: 3 },
    { key: "scale-divisor", value: 2 }] }],
};

let sequence = 0;
const entered = () => ({ sequence: sequence++, kind: CAPTURE.RECORD_ENTERED });
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
  /* Times count from the key press's millisecond; the flush keeps its
   * millisecond; a clipped span restarts the speed estimate on replay. */
  assert.deepEqual(fixture.events, [["keypress", 0], ["motion", 10250, 3, 0, 1000],
    ["flush", 0], ["motion", 2749, -1, 0, 40000]]);
  assert.match(encodeRunnerInput(fixture), /^F\nE\nK 0\nM 10250 3 0 1000\nF 10250\nM 12999/m);
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

test("only cursor pipelines convert", () => {
  const scroll = { ...description, stages: [{ strategy: "axis-intent", stableId: "a", fixed: {} }] };
  assert.throws(() => captureToFixtures({ description: scroll, records: [] }, profile),
    /isn't a cursor stage/);
});

test("parses capture batches with consecutive sequences", () => {
  const payload = Uint8Array.of(7, 0, 0, 0, 3,
    CAPTURE.RECORD_SOURCE, 1, 2, 0, 0x20, 0x03,
    CAPTURE.RECORD_FRAME, 0x80, 10, 0, 0, 0, 0xe7, 0x03, 0xff, 0xff, 0xff, 0xff, 2, 0, 0, 0,
    0x30, 0x75, 0, 0,
    CAPTURE.RECORD_LEFT);
  assert.deepEqual(parseCaptureBatch(payload), [
    { sequence: 7, kind: CAPTURE.RECORD_SOURCE, link: 1, sourceId: 2, cpi: 800 },
    { sequence: 8, kind: CAPTURE.RECORD_FRAME, flags: 0x80, timeMs: 10, timeUs: 999, x: -1,
      y: 2, spanUs: 30000 },
    { sequence: 9, kind: CAPTURE.RECORD_LEFT },
  ]);
  assert.throws(() => parseCaptureBatch(Uint8Array.of(0, 0, 0, 0, 1, 99)), /Unknown capture record/);
});
