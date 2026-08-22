import test from "node:test";
import assert from "node:assert/strict";

import {
  FrameDecoder,
  MESSAGE,
  encodeDevicePreview,
  encodeStateControl,
  encodeTuningSet,
  encodeTuningSetMany,
  encodeFrame,
  parseAck,
  parseDescribe,
  parseDeviceDescription,
  parseDeviceList,
  parseStateSample,
  parseStateStatus,
  parseTuningDescription,
  parseTuningHelp,
  parseTuningParameterMetadata,
  parseTuningResult,
  parseTuningTargetMetadata,
  parseTuningTargets,
} from "./protocol.js";

test("decodes fragmented and adjacent frames", () => {
  const first = encodeFrame(MESSAGE.DESCRIBE_REQUEST);
  const second = encodeFrame(MESSAGE.PING);
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
      [MESSAGE.PING, []],
    ],
  );
});

test("parses the version-only description", () => {
  assert.deepEqual(parseDescribe(Uint8Array.of(7)), { version: 7 });
  assert.throws(
    () => parseDescribe(Uint8Array.of(6)),
    /does not match tuner protocol/,
  );
});

test("parses acknowledgements", () => {
  const ack = new Uint8Array(4);
  new DataView(ack.buffer).setUint32(0, 42, true);
  assert.deepEqual(parseAck(ack), { stateDropped: 42 });
  assert.throws(() => parseAck(new Uint8Array(5)), /Invalid acknowledgement length/);

});

test("parses and controls semantic processor state", () => {
  const firstLabel = new TextEncoder().encode("left-scroll");
  const secondLabel = new TextEncoder().encode("coherent-displacement");
  const status = new Uint8Array(8 + 3 + firstLabel.length + 3 + secondLabel.length);
  const statusView = new DataView(status.buffer);
  status[0] = 2;
  statusView.setUint32(1, 7, true);
  statusView.setUint16(5, 64, true);
  status[7] = 2;
  let offset = 8;
  status[offset++] = 0;
  status[offset++] = 1;
  status[offset++] = firstLabel.length;
  status.set(firstLabel, offset);
  offset += firstLabel.length;
  status[offset++] = 1;
  status[offset++] = 2;
  status[offset++] = secondLabel.length;
  status.set(secondLabel, offset);
  const parsedStatus = parseStateStatus(status);
  assert.equal(parsedStatus.schemaVersion, 2);
  assert.equal(parsedStatus.dropped, 7);
  assert.equal(parsedStatus.queueCapacity, 64);
  assert.deepEqual([...parsedStatus.levels], [[0, 1], [1, 2]]);
  assert.deepEqual([...parsedStatus.labels], [[0, "left-scroll"], [1, "coherent-displacement"]]);

  const payload = new Uint8Array(54);
  const view = new DataView(payload.buffer);
  payload.set([1, 1, 1, 3]);
  view.setUint16(4, 0x23, true);
  view.setUint32(6, 1234, true);
  view.setUint32(10, 99, true);
  view.setInt32(14, -5, true);
  view.setInt32(18, 8, true);
  assert.deepEqual(parseStateSample(payload), {
    targetId: 1,
    targetKind: 1,
    event: 1,
    intent: 3,
    flags: 0x23,
    timestamp: 1234,
    sequence: 99,
    values: [-5, 8, 0, 0, 0, 0, 0, 0, 0, 0],
  });

  assert.deepEqual([...encodeStateControl(1, 2)], [0x5a, 0x50, MESSAGE.STATE_CONTROL_REQUEST, 2, 0, 1, 2]);
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

test("parses pointing device lists", () => {
  const label = new TextEncoder().encode("test-local-trackball");
  const payload = Uint8Array.of(
    1,
    0,
    0, // local
    DEVICE_FLAGS_SETTABLE_AND_CONNECTED,
    label.length,
    ...label,
  );
  assert.deepEqual(parseDeviceList(payload), [
    { id: 0, location: 0, connected: true, settable: true, label: "test-local-trackball" },
  ]);

  assert.throws(() => parseDeviceList(Uint8Array.of(2, 0)), /Truncated device list entry/);
});

const DEVICE_FLAGS_SETTABLE_AND_CONNECTED = 0x03;

test("parses pointing device descriptions in every capability form", () => {
  const stableId = new TextEncoder().encode("test-local-trackball");
  const path = new TextEncoder().encode("/trackball-local");
  const head = [
    stableId.length, ...stableId,
    path.length, 0, ...path,
    0x20, 0x03, // current 800
    0x20, 0x03, // default 800
    1,          // settable
  ];
  const discrete = Uint8Array.of(...head, 4, 200 & 0xff, 200 >> 8, 400 & 0xff, 400 >> 8, 800 & 0xff, 800 >> 8, 1600 & 0xff, 1600 >> 8);
  assert.deepEqual(parseDeviceDescription(discrete), {
    stableId: "test-local-trackball",
    devicetreePath: "/trackball-local",
    currentCpi: 800,
    defaultCpi: 800,
    settable: true,
    discrete: true,
    values: [200, 400, 800, 1600],
  });

  const range = Uint8Array.of(...head, 0, 100 & 0xff, 100 >> 8, 0xb0, 0x04, 100 & 0xff, 100 >> 8);
  assert.deepEqual(parseDeviceDescription(range), {
    stableId: "test-local-trackball",
    devicetreePath: "/trackball-local",
    currentCpi: 800,
    defaultCpi: 800,
    settable: true,
    discrete: false,
    range: { min: 100, max: 1200, step: 100 },
  });

  const readOnly = Uint8Array.of(stableId.length, ...stableId, path.length, 0, ...path, 0xb0, 0x04, 0xb0, 0x04, 0);
  assert.deepEqual(parseDeviceDescription(readOnly), {
    stableId: "test-local-trackball",
    devicetreePath: "/trackball-local",
    currentCpi: 1200,
    defaultCpi: 1200,
    settable: false,
  });

  assert.throws(() => parseDeviceDescription(Uint8Array.of(3, ...stableId.slice(0, 2))), /Truncated device stable id/);
});

test("encodes device previews little-endian", () => {
  const frame = encodeDevicePreview(1, 800);
  assert.deepEqual([...frame.slice(0, 8)], [0x5a, 0x50, MESSAGE.DEVICE_PREVIEW_REQUEST, 3, 0, 1, 0x20, 0x03]);
});
