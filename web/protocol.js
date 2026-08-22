export const MESSAGE = Object.freeze({
  DESCRIBE_REQUEST: 0x01,
  PING: 0x03,
  TUNING_TARGETS_REQUEST: 0x04,
  TUNING_DESCRIBE_REQUEST: 0x05,
  TUNING_SET_REQUEST: 0x06,
  TUNING_RESET_REQUEST: 0x07,
  TUNING_HELP_REQUEST: 0x08,
  TUNING_TARGET_METADATA_REQUEST: 0x09,
  TUNING_PARAMETER_METADATA_REQUEST: 0x0a,
  TUNING_SET_MANY_REQUEST: 0x0b,
  STATE_CONTROL_REQUEST: 0x0c,
  DEVICE_LIST_REQUEST: 0x0d,
  DEVICE_DESCRIBE_REQUEST: 0x0e,
  DEVICE_PREVIEW_REQUEST: 0x0f,
  DEVICE_LIST_REQUEST: 0x0d,
  DEVICE_DESCRIBE_REQUEST: 0x0e,
  DEVICE_PREVIEW_REQUEST: 0x0f,
  DESCRIBE_RESPONSE: 0x81,
  ACK: 0x82,
  TUNING_TARGETS_RESPONSE: 0x83,
  TUNING_DESCRIBE_RESPONSE: 0x84,
  TUNING_RESULT: 0x85,
  TUNING_HELP_RESPONSE: 0x86,
  TUNING_TARGET_METADATA_RESPONSE: 0x87,
  TUNING_PARAMETER_METADATA_RESPONSE: 0x88,
  STATE_STATUS_RESPONSE: 0x89,
  DEVICE_LIST_RESPONSE: 0x8a,
  DEVICE_DESCRIPTION_RESPONSE: 0x8b,
  DEVICE_LIST_RESPONSE: 0x8a,
  DEVICE_DESCRIPTION_RESPONSE: 0x8b,
  STATE_SAMPLE: 0x91,
});

/* v7 added pointing-device discovery and preview messages. */
export const PROTOCOL_VERSION = 7;
export const STATE_SCHEMA_VERSION = 2;

/* Target kinds carried in state samples. Pipeline stages are the only
 * producers in the current firmware; the tuning registry is empty. */
export const TARGET_KIND = Object.freeze({
  PIPELINE_STAGE: 4,
});

export const TUNING = Object.freeze({
  ALL_TARGETS: 0xff,
  INTEGER: 0,
  BOOLEAN: 1,
  STATUS_OK: 0,
});

export const DEVICE = Object.freeze({
  LOCATION_LOCAL: 0,
  FLAG_LOCAL_CONNECTED: 0x01,
  FLAG_SETTABLE: 0x02,
});

export const STATE = Object.freeze({
  ALL_TARGETS: 0xff,
  OFF: 0,
  DECISIONS: 1,
  VERBOSE: 2,
  EVENT_FRAME: 1,
  EVENT_FLUSH: 2,
  FLAG_INTENT_CHANGED: 1 << 0,
  FLAG_SUPPRESSED: 1 << 1,
  FLAG_DISCARDED: 1 << 2,
  FLAG_OUTPUT: 1 << 3,
  FLAG_QUALIFIED: 1 << 4,
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
  const version = payload[0];
  if (version !== PROTOCOL_VERSION) {
    throw new Error(`Firmware protocol ${version} does not match tuner protocol ${PROTOCOL_VERSION}`);
  }
  return { version };
}

export function parseAck(payload) {
  if (payload.length !== 4) throw new Error("Invalid acknowledgement length");
  const view = new DataView(payload.buffer, payload.byteOffset, payload.byteLength);
  return {
    stateDropped: view.getUint32(0, true),
  };
}

export function parseStateStatus(payload) {
  requireBytes(payload, 0, 8, "state telemetry status");
  const view = new DataView(payload.buffer, payload.byteOffset, payload.byteLength);
  if (payload[0] !== STATE_SCHEMA_VERSION) throw new Error("Unsupported state telemetry schema");
  const count = payload[7];
  const levels = new Map();
  const labels = new Map();
  const decoder = new TextDecoder();
  let offset = 8;
  for (let index = 0; index < count; index += 1) {
    requireBytes(payload, offset, 3, "state telemetry target entry");
    const targetId = payload[offset++];
    const level = payload[offset++];
    const labelLength = payload[offset++];
    requireBytes(payload, offset, labelLength, "state telemetry target label");
    levels.set(targetId, level);
    labels.set(targetId, decoder.decode(payload.slice(offset, offset + labelLength)));
    offset += labelLength;
  }
  return {
    schemaVersion: payload[0],
    dropped: view.getUint32(1, true),
    queueCapacity: view.getUint16(5, true),
    levels,
    labels,
  };
}

export function parseStateSample(payload) {
  if (payload.length !== 54) throw new Error("Invalid state sample length");
  const view = new DataView(payload.buffer, payload.byteOffset, payload.byteLength);
  const values = [];
  for (let offset = 14; offset < payload.length; offset += 4) values.push(view.getInt32(offset, true));
  return {
    targetId: payload[0],
    targetKind: payload[1],
    event: payload[2],
    intent: payload[3],
    flags: view.getUint16(4, true),
    timestamp: view.getUint32(6, true),
    sequence: view.getUint32(10, true),
    values,
  };
}

export function encodeStateControl(targetId, level) {
  return encodeFrame(MESSAGE.STATE_CONTROL_REQUEST, Uint8Array.of(targetId, level));
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

/* --- Pointing devices (protocol v7) ---------------------------------- */

export function parseDeviceList(payload) {
  requireBytes(payload, 0, 1, "device list");
  const decoder = new TextDecoder();
  const count = payload[0];
  const devices = [];
  let offset = 1;
  for (let index = 0; index < count; index += 1) {
    requireBytes(payload, offset, 4, "device list entry");
    const id = payload[offset++];
    const location = payload[offset++];
    const flags = payload[offset++];
    const labelLength = payload[offset++];
    requireBytes(payload, offset, labelLength, "device label");
    const label = decoder.decode(payload.slice(offset, offset + labelLength));
    offset += labelLength;
    devices.push({
      id,
      location,
      connected: Boolean(flags & DEVICE.FLAG_LOCAL_CONNECTED),
      settable: Boolean(flags & DEVICE.FLAG_SETTABLE),
      label,
    });
  }
  return devices;
}

export function parseDeviceDescription(payload) {
  requireBytes(payload, 0, 1, "device description");
  const decoder = new TextDecoder();
  const view = new DataView(payload.buffer, payload.byteOffset, payload.byteLength);
  const stableIdLength = payload[0];
  let offset = 1;
  requireBytes(payload, offset, stableIdLength, "device stable id");
  const stableId = decoder.decode(payload.slice(offset, offset + stableIdLength));
  offset += stableIdLength;
  requireBytes(payload, offset, 2, "device devicetree path length");
  const pathLength = view.getUint16(offset, true);
  offset += 2;
  requireBytes(payload, offset, pathLength, "device devicetree path");
  const devicetreePath = decoder.decode(payload.slice(offset, offset + pathLength));
  offset += pathLength;
  requireBytes(payload, offset, 5, "device resolution fields");
  const currentCpi = view.getUint16(offset, true);
  offset += 2;
  const defaultCpi = view.getUint16(offset, true);
  offset += 2;
  const settable = payload[offset++] !== 0;

  if (!settable) {
    return { stableId, devicetreePath, currentCpi, defaultCpi, settable };
  }

  requireBytes(payload, offset, 1, "device capability count");
  const valueCount = payload[offset++];
  if (valueCount > 0) {
    requireBytes(payload, offset, valueCount * 2, "device supported values");
    const values = [];
    for (let index = 0; index < valueCount; index += 1) {
      values.push(view.getUint16(offset, true));
      offset += 2;
    }
    return { stableId, devicetreePath, currentCpi, defaultCpi, settable, discrete: true, values };
  }
  requireBytes(payload, offset, 6, "device capability range");
  const range = {
    min: view.getUint16(offset, true),
    max: view.getUint16(offset + 2, true),
    step: view.getUint16(offset + 4, true),
  };
  return { stableId, devicetreePath, currentCpi, defaultCpi, settable, discrete: false, range };
}

export function encodeDevicePreview(deviceId, cpi) {
  return encodeFrame(MESSAGE.DEVICE_PREVIEW_REQUEST, Uint8Array.of(deviceId, cpi & 0xff, (cpi >> 8) & 0xff));
}
