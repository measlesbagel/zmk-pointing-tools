export const MESSAGE = Object.freeze({
  DESCRIBE_REQUEST: 0x01,
  TELEMETRY_CONTROL: 0x02,
  PING: 0x03,
  DESCRIBE_RESPONSE: 0x81,
  ACK: 0x82,
  SAMPLE: 0x90,
});

const MAGIC_0 = 0x5a;
const MAGIC_1 = 0x50;

export function encodeFrame(type, payload = new Uint8Array()) {
  const frame = new Uint8Array(5 + payload.length);
  frame.set([MAGIC_0, MAGIC_1, type, payload.length & 0xff, payload.length >> 8]);
  frame.set(payload, 5);
  return frame;
}

export class FrameDecoder {
  #buffer = new Uint8Array();

  push(chunk) {
    const merged = new Uint8Array(this.#buffer.length + chunk.length);
    merged.set(this.#buffer);
    merged.set(chunk, this.#buffer.length);
    const frames = [];
    let offset = 0;

    while (offset < merged.length) {
      while (
        offset + 1 < merged.length &&
        (merged[offset] !== MAGIC_0 || merged[offset + 1] !== MAGIC_1)
      ) {
        offset += 1;
      }
      if (merged.length - offset < 5) break;

      const length = merged[offset + 3] | (merged[offset + 4] << 8);
      if (merged.length - offset < 5 + length) break;
      frames.push({
        type: merged[offset + 2],
        payload: merged.slice(offset + 5, offset + 5 + length),
      });
      offset += 5 + length;
    }

    this.#buffer = merged.slice(offset);
    return frames;
  }
}

export function parseDescribe(payload) {
  const decoder = new TextDecoder();
  let offset = 0;
  const version = payload[offset++];
  const count = payload[offset++];
  const streams = [];

  for (let index = 0; index < count; index += 1) {
    const deviceId = payload[offset++];
    const stage = payload[offset++];
    const length = payload[offset++];
    if (offset + length > payload.length) throw new Error("Truncated stream descriptor");
    const label = decoder.decode(payload.slice(offset, offset + length));
    offset += length;
    streams.push({ deviceId, stage, label, key: `${deviceId}:${stage}` });
  }
  return { version, streams };
}

export function parseAck(payload) {
  if (payload.length !== 5) throw new Error("Invalid acknowledgement length");
  const view = new DataView(payload.buffer, payload.byteOffset, payload.byteLength);
  return { enabled: Boolean(payload[0]), dropped: view.getUint32(1, true) };
}

export function parseSample(payload) {
  if (payload.length !== 26) throw new Error("Invalid trace sample length");
  const view = new DataView(payload.buffer, payload.byteOffset, payload.byteLength);
  const deviceId = payload[0];
  const stage = payload[1];
  return {
    deviceId,
    stage,
    key: `${deviceId}:${stage}`,
    timestamp: view.getUint32(2, true),
    sequence: view.getUint32(6, true),
    x: view.getInt32(10, true),
    y: view.getInt32(14, true),
    wheel: view.getInt32(18, true),
    hWheel: view.getInt32(22, true),
  };
}
