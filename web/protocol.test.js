import test from "node:test";
import assert from "node:assert/strict";

import {
  FrameDecoder,
  MESSAGE,
  encodeFrame,
  parseAck,
  parseDescribe,
  parseSample,
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
