import test from "node:test";
import assert from "node:assert/strict";

import {
  FrameDecoder,
  MESSAGE,
  encodeTuningSet,
  encodeTuningSetMany,
  encodeFrame,
  parseAck,
  parseDescribe,
  parseSample,
  parseTuningDescription,
  parseTuningHelp,
  parseTuningParameterMetadata,
  parseTuningResult,
  parseTuningTargetMetadata,
  parseTuningTargets,
} from "./protocol.js";

test("decodes fragmented and adjacent frames", () => {
  const first = encodeFrame(MESSAGE.DESCRIBE_REQUEST);
  const second = encodeFrame(MESSAGE.TELEMETRY_CONTROL, Uint8Array.of(1));
  const bytes = new Uint8Array(first.length + second.length + 1);
  bytes[0] = 0xff;
  bytes.set(first, 1);
  bytes.set(second, first.length + 1);

  const decoder = new FrameDecoder();
  assert.deepEqual(decoder.push(bytes.slice(0, 4)), []);
  assert.deepEqual(
    decoder.push(bytes.slice(4)).map(({ type, payload }) => [type, [...payload]]),
    [
      [MESSAGE.DESCRIBE_REQUEST, []],
      [MESSAGE.TELEMETRY_CONTROL, [1]],
    ],
  );
});

test("parses stream descriptions", () => {
  const label = new TextEncoder().encode("Right raw");
  const payload = Uint8Array.of(1, 1, 7, 0, label.length, ...label);
  assert.deepEqual(parseDescribe(payload), {
    version: 1,
    streams: [{ deviceId: 7, stage: 0, label: "Right raw", key: "7:0" }],
  });
});

test("parses acknowledgements and signed samples", () => {
  const ack = new Uint8Array(5);
  new DataView(ack.buffer).setUint32(1, 42, true);
  ack[0] = 1;
  assert.deepEqual(parseAck(ack), { enabled: true, dropped: 42 });

  const sample = new Uint8Array(26);
  const view = new DataView(sample.buffer);
  sample.set([3, 1]);
  view.setUint32(2, 1000, true);
  view.setUint32(6, 9, true);
  view.setInt32(10, -12, true);
  view.setInt32(14, 34, true);
  view.setInt32(18, -2, true);
  view.setInt32(22, 5, true);
  assert.deepEqual(parseSample(sample), {
    deviceId: 3,
    stage: 1,
    key: "3:1",
    timestamp: 1000,
    sequence: 9,
    x: -12,
    y: 34,
    wheel: -2,
    hWheel: 5,
  });
});

test("parses runtime tuning discovery and parameter descriptions", () => {
  const targetLabel = new TextEncoder().encode("Left scroll");
  const targetPayload = Uint8Array.of(1, 3, 1, targetLabel.length, ...targetLabel);
  assert.deepEqual(parseTuningTargets(targetPayload), [
    { id: 3, kind: 1, label: "Left scroll", parameters: [] },
  ]);

  const label = new TextEncoder().encode("Physical keypress guard");
  const unit = new TextEncoder().encode("ms");
  const description = new Uint8Array(2 + 24 + label.length + unit.length);
  const view = new DataView(description.buffer);
  description.set([3, 1, 9, 0]);
  view.setInt32(4, 0, true);
  view.setInt32(8, 500, true);
  view.setInt32(12, 1, true);
  view.setInt32(16, 75, true);
  view.setInt32(20, 60, true);
  description.set([label.length, unit.length], 24);
  description.set(label, 26);
  description.set(unit, 26 + label.length);

  assert.deepEqual(parseTuningDescription(description), {
    targetId: 3,
    parameters: [{
      id: 9,
      type: 0,
      minimum: 0,
      maximum: 500,
      step: 1,
      compiled: 75,
      current: 60,
      label: "Physical keypress guard",
      unit: "ms",
    }],
  });
});

test("encodes tuning previews and parses results", () => {
  const frame = encodeTuningSet(2, 9, -12);
  assert.deepEqual([...frame.slice(0, 7)], [0x5a, 0x50, MESSAGE.TUNING_SET_REQUEST, 6, 0, 2, 9]);
  assert.equal(new DataView(frame.buffer).getInt32(7, true), -12);

  const result = new Uint8Array(8);
  result.set([MESSAGE.TUNING_SET_REQUEST, 0, 2, 9]);
  new DataView(result.buffer).setInt32(4, 60, true);
  assert.deepEqual(parseTuningResult(result), {
    requestType: MESSAGE.TUNING_SET_REQUEST,
    status: 0,
    targetId: 2,
    parameterId: 9,
    value: 60,
  });
});

test("parses on-demand tuning help", () => {
  const description = new TextEncoder().encode("Rejects typing vibration.");
  const payload = Uint8Array.of(2, 9, description.length, 0, ...description);
  assert.deepEqual(parseTuningHelp(payload), {
    targetId: 2,
    parameterId: 9,
    description: "Rejects typing vibration.",
  });
});

test("parses stable tuning metadata", () => {
  const stableId = new TextEncoder().encode("left-scroll");
  const path = new TextEncoder().encode("/zpt_left_scroll");
  const target = Uint8Array.of(2, stableId.length, path.length, 0, ...stableId, ...path);
  assert.deepEqual(parseTuningTargetMetadata(target), {
    targetId: 2,
    stableId: "left-scroll",
    devicetreePath: "/zpt_left_scroll",
  });

  const key = new TextEncoder().encode("scale-multiplier");
  const property = new TextEncoder().encode("scale-multiplier");
  const parameter = Uint8Array.of(2, 1, key.length, property.length, ...key, ...property);
  assert.deepEqual(parseTuningParameterMetadata(parameter), {
    targetId: 2,
    parameterId: 1,
    key: "scale-multiplier",
    devicetreeProperty: "scale-multiplier",
  });
});

test("encodes atomic tuning batches", () => {
  const frame = encodeTuningSetMany(2, [
    { parameterId: 7, value: 300 },
    { parameterId: 8, value: 180 },
  ]);
  assert.deepEqual([...frame.slice(0, 7)], [0x5a, 0x50, MESSAGE.TUNING_SET_MANY_REQUEST, 12, 0, 2, 2]);
  const view = new DataView(frame.buffer);
  assert.equal(frame[7], 7);
  assert.equal(view.getInt32(8, true), 300);
  assert.equal(frame[12], 8);
  assert.equal(view.getInt32(13, true), 180);
});
