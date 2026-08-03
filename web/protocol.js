export const MESSAGE = Object.freeze({
  DESCRIBE_REQUEST: 0x01,
  TELEMETRY_CONTROL: 0x02,
  PING: 0x03,
  TUNING_TARGETS_REQUEST: 0x04,
  TUNING_DESCRIBE_REQUEST: 0x05,
  TUNING_SET_REQUEST: 0x06,
  TUNING_RESET_REQUEST: 0x07,
  TUNING_HELP_REQUEST: 0x08,
  TUNING_TARGET_METADATA_REQUEST: 0x09,
  TUNING_PARAMETER_METADATA_REQUEST: 0x0a,
  TUNING_SET_MANY_REQUEST: 0x0b,
  DESCRIBE_RESPONSE: 0x81,
  ACK: 0x82,
  TUNING_TARGETS_RESPONSE: 0x83,
  TUNING_DESCRIBE_RESPONSE: 0x84,
  TUNING_RESULT: 0x85,
  TUNING_HELP_RESPONSE: 0x86,
  TUNING_TARGET_METADATA_RESPONSE: 0x87,
  TUNING_PARAMETER_METADATA_RESPONSE: 0x88,
  SAMPLE: 0x90,
});

export const TUNING = Object.freeze({
  ALL_TARGETS: 0xff,
  INTEGER: 0,
  BOOLEAN: 1,
  STATUS_OK: 0,
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

function requireBytes(payload, offset, count, context) {
  if (offset + count > payload.length) throw new Error(`Truncated ${context}`);
}

export function parseTuningTargets(payload) {
  requireBytes(payload, 0, 1, "tuning target list");
  const decoder = new TextDecoder();
  const count = payload[0];
  const targets = [];
  let offset = 1;

  for (let index = 0; index < count; index += 1) {
    requireBytes(payload, offset, 3, "tuning target descriptor");
    const id = payload[offset++];
    const kind = payload[offset++];
    const length = payload[offset++];
    requireBytes(payload, offset, length, "tuning target label");
    const label = decoder.decode(payload.slice(offset, offset + length));
    offset += length;
    targets.push({ id, kind, label, parameters: [] });
  }
  return targets;
}

export function parseTuningDescription(payload) {
  requireBytes(payload, 0, 2, "tuning description");
  const decoder = new TextDecoder();
  const view = new DataView(payload.buffer, payload.byteOffset, payload.byteLength);
  const targetId = payload[0];
  const count = payload[1];
  const parameters = [];
  let offset = 2;

  for (let index = 0; index < count; index += 1) {
    requireBytes(payload, offset, 24, "tuning parameter descriptor");
    const id = payload[offset++];
    const type = payload[offset++];
    const minimum = view.getInt32(offset, true); offset += 4;
    const maximum = view.getInt32(offset, true); offset += 4;
    const step = view.getInt32(offset, true); offset += 4;
    const compiled = view.getInt32(offset, true); offset += 4;
    const current = view.getInt32(offset, true); offset += 4;
    const labelLength = payload[offset++];
    const unitLength = payload[offset++];
    requireBytes(payload, offset, labelLength + unitLength, "tuning parameter strings");
    const label = decoder.decode(payload.slice(offset, offset + labelLength));
    offset += labelLength;
    const unit = decoder.decode(payload.slice(offset, offset + unitLength));
    offset += unitLength;
    parameters.push({ id, type, minimum, maximum, step, compiled, current, label, unit });
  }
  return { targetId, parameters };
}

export function parseTuningResult(payload) {
  if (payload.length !== 8) throw new Error("Invalid tuning result length");
  const view = new DataView(payload.buffer, payload.byteOffset, payload.byteLength);
  return {
    requestType: payload[0],
    status: payload[1],
    targetId: payload[2],
    parameterId: payload[3],
    value: view.getInt32(4, true),
  };
}

export function parseTuningHelp(payload) {
  requireBytes(payload, 0, 4, "tuning help");
  const length = payload[2] | (payload[3] << 8);
  requireBytes(payload, 4, length, "tuning help text");
  return {
    targetId: payload[0],
    parameterId: payload[1],
    description: new TextDecoder().decode(payload.slice(4, 4 + length)),
  };
}

export function parseTuningTargetMetadata(payload) {
  requireBytes(payload, 0, 4, "tuning target metadata");
  const stableIdLength = payload[1];
  const pathLength = payload[2] | (payload[3] << 8);
  requireBytes(payload, 4, stableIdLength + pathLength, "tuning target metadata strings");
  const decoder = new TextDecoder();
  return {
    targetId: payload[0],
    stableId: decoder.decode(payload.slice(4, 4 + stableIdLength)),
    devicetreePath: decoder.decode(payload.slice(4 + stableIdLength, 4 + stableIdLength + pathLength)),
  };
}

export function parseTuningParameterMetadata(payload) {
  requireBytes(payload, 0, 4, "tuning parameter metadata");
  const keyLength = payload[2];
  const propertyLength = payload[3];
  requireBytes(payload, 4, keyLength + propertyLength, "tuning parameter metadata strings");
  const decoder = new TextDecoder();
  return {
    targetId: payload[0],
    parameterId: payload[1],
    key: decoder.decode(payload.slice(4, 4 + keyLength)),
    devicetreeProperty: decoder.decode(payload.slice(4 + keyLength, 4 + keyLength + propertyLength)),
  };
}

export function encodeTuningSet(targetId, parameterId, value) {
  const payload = new Uint8Array(6);
  payload.set([targetId, parameterId]);
  new DataView(payload.buffer).setInt32(2, value, true);
  return encodeFrame(MESSAGE.TUNING_SET_REQUEST, payload);
}

export function encodeTuningSetMany(targetId, values) {
  if (values.length === 0 || values.length > 20) {
    throw new Error("A tuning batch must contain between 1 and 20 values");
  }
  const payload = new Uint8Array(2 + values.length * 5);
  const view = new DataView(payload.buffer);
  payload.set([targetId, values.length]);
  let offset = 2;
  for (const { parameterId, value } of values) {
    payload[offset++] = parameterId;
    view.setInt32(offset, value, true);
    offset += 4;
  }
  return encodeFrame(MESSAGE.TUNING_SET_MANY_REQUEST, payload);
}
