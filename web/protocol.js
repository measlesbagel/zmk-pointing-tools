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
  CAPTURE_CONTROL_REQUEST: 0x10,
  CAPTURE_DESCRIBE_REQUEST: 0x11,
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
  CAPTURE_STATUS_RESPONSE: 0x8c,
  CAPTURE_DESCRIPTION_RESPONSE: 0x8d,
  STATE_SAMPLE: 0x91,
  CAPTURE_BATCH: 0x92,
});

/* v7 added pointing-device discovery and preview messages; v8 input
 * capture and the capture drop counter in acknowledgements. */
export const PROTOCOL_VERSION = 8;
export const STATE_SCHEMA_VERSION = 2;

/* Target kinds carried in tuning targets and state samples. Pipeline stages
 * are the only kind current firmware registers. */
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
  /* Device list flags. Local devices are always reachable; a peripheral's
   * are reachable when its half answered the last request sent to it. */
  FLAG_REACHABLE: 0x01,
  FLAG_SETTABLE: 0x02,
  /* Device description flags. Stale marks a value the central last knew
   * because the owning half didn't answer. */
  DESCRIPTION_SETTABLE: 0x01,
  DESCRIPTION_STALE: 0x02,
  /* Device results only: the owning split half didn't answer. */
  STATUS_UNREACHABLE: 5,
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
  if (payload.length !== 8) throw new Error("Invalid acknowledgement length");
  const view = new DataView(payload.buffer, payload.byteOffset, payload.byteLength);
  return {
    stateDropped: view.getUint32(0, true),
    captureDropped: view.getUint32(4, true),
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
      reachable: Boolean(flags & DEVICE.FLAG_REACHABLE),
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
  const flags = payload[offset++];
  const settable = Boolean(flags & DEVICE.DESCRIPTION_SETTABLE);
  const stale = Boolean(flags & DEVICE.DESCRIPTION_STALE);

  if (!settable) {
    return { stableId, devicetreePath, currentCpi, defaultCpi, settable, stale };
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
    return { stableId, devicetreePath, currentCpi, defaultCpi, settable, stale, discrete: true, values };
  }
  requireBytes(payload, offset, 6, "device capability range");
  const range = {
    min: view.getUint16(offset, true),
    max: view.getUint16(offset + 2, true),
    step: view.getUint16(offset + 4, true),
  };
  return { stableId, devicetreePath, currentCpi, defaultCpi, settable, stale, discrete: false, range };
}

export function encodeDevicePreview(deviceId, cpi) {
  return encodeFrame(MESSAGE.DEVICE_PREVIEW_REQUEST, Uint8Array.of(deviceId, cpi & 0xff, (cpi >> 8) & 0xff));
}

/* --- Input capture (protocol v8) ------------------------------------- */

export const CAPTURE = Object.freeze({
  SCHEMA_VERSION: 2,
  NONE: 0xff,
  RECORD_FRAME: 1,
  RECORD_SOURCE: 2,
  RECORD_KEYPRESS: 3,
  RECORD_ENTERED: 4,
  RECORD_LEFT: 5,
  RECORD_SETTINGS_CHANGED: 6,
  RECORD_FLUSH: 7,
  LINK_USB: 0,
  LINK_BLE: 1,
  LINK_NONE: 0xff,
  FLAG_REPLAYABLE: 0x01,
  /* Wire kinds 0x80 to 0x87: a frame as a delta from the previous one. */
  DELTA_FRAME: 0x80,
  DELTA_FLAGS: 0x04,
  SPAN_PREVIOUS: 0,
  SPAN_ELAPSED: 1,
  SPAN_ELAPSED_PLUS: 2,
  SPAN_ABSOLUTE: 3,
});

/* Start recording a pipeline, or stop with CAPTURE.NONE; no argument only
 * queries the status. */
export function encodeCaptureControl(pipelineId) {
  return encodeFrame(MESSAGE.CAPTURE_CONTROL_REQUEST,
    pipelineId === undefined ? new Uint8Array() : Uint8Array.of(pipelineId));
}

export function encodeCaptureDescribe(pipelineId) {
  return encodeFrame(MESSAGE.CAPTURE_DESCRIBE_REQUEST, Uint8Array.of(pipelineId));
}

export function parseCaptureStatus(payload) {
  requireBytes(payload, 0, 1, "capture status");
  /* Schema 1 streams (no burst buffer) aren't read: record again with
   * current firmware. */
  if (payload[0] !== CAPTURE.SCHEMA_VERSION) {
    throw new Error(`Unsupported capture schema ${payload[0]}; expected ${CAPTURE.SCHEMA_VERSION}`);
  }
  requireBytes(payload, 0, 15, "capture status");
  const view = new DataView(payload.buffer, payload.byteOffset, payload.byteLength);
  const decoder = new TextDecoder();
  const pipelines = [];
  let offset = 15;
  for (let index = 0; index < payload[14]; index += 1) {
    requireBytes(payload, offset, 9, "capture pipeline entry");
    const [id, active, stageCount, flags] = payload.slice(offset, offset + 4);
    const settingsHash = view.getUint32(offset + 4, true);
    const labelLength = payload[offset + 8];
    offset += 9;
    requireBytes(payload, offset, labelLength, "capture pipeline label");
    pipelines.push({ id, active: active !== 0, stageCount,
      replayable: Boolean(flags & CAPTURE.FLAG_REPLAYABLE), settingsHash,
      label: decoder.decode(payload.slice(offset, offset + labelLength)) });
    offset += labelLength;
  }
  return {
    schemaVersion: payload[0],
    recording: payload[1] === CAPTURE.NONE ? null : payload[1],
    dropped: view.getUint32(2, true),
    /* Bytes of encoded records waiting in the burst buffer, and its size. */
    bufferUsed: view.getUint32(6, true),
    bufferSize: view.getUint32(10, true),
    pipelines,
  };
}

export function parseCaptureDescription(payload) {
  const view = new DataView(payload.buffer, payload.byteOffset, payload.byteLength);
  const decoder = new TextDecoder();
  let offset = 0;
  const byte = (context) => {
    requireBytes(payload, offset, 1, context);
    return payload[offset++];
  };
  const string = (context) => {
    const length = byte(context);
    requireBytes(payload, offset, length, context);
    const text = decoder.decode(payload.slice(offset, offset + length));
    offset += length;
    return text;
  };
  const pipelineId = byte("capture description");
  const stableId = string("capture pipeline id");
  requireBytes(payload, offset, 4, "capture settings hash");
  const settingsHash = view.getUint32(offset, true);
  offset += 4;
  const stages = [];
  for (let count = byte("capture stage count"); stages.length < count;) {
    const strategy = string("capture stage strategy");
    const stageId = string("capture stage id");
    const fixed = {};
    for (let settings = byte("capture stage settings"); settings > 0; settings -= 1) {
      const key = string("capture stage setting");
      requireBytes(payload, offset, 4, "capture stage setting value");
      fixed[key] = view.getInt32(offset, true);
      offset += 4;
    }
    stages.push({ strategy, stableId: stageId, fixed });
  }
  return { pipelineId, stableId, settingsHash, stages };
}

/* Decoding state carried from batch to batch of one recording: delta
 * frames build on the previous frame. skipped counts delta frames that had
 * no frame to follow and were left out. */
export function captureDecoder() {
  return { nextSequence: 0, previous: null, skipped: 0 };
}

/* A u32 varint: at most 5 bytes, the 5th holding the top 4 bits, so every
 * zigzag field decodes to an int32. */
function readVarint(payload, offset, context) {
  let value = 0;
  for (let shift = 0; shift < 35; shift += 7) {
    requireBytes(payload, offset, 1, context);
    const byte = payload[offset];
    offset += 1;
    if (shift === 28 && byte > 0x0f) break;
    value += (byte & 0x7f) * 2 ** shift;
    if ((byte & 0x80) === 0) return [value, offset];
  }
  throw new Error(`Malformed varint in ${context}`);
}

const unzigzag = (value) => (value % 2 === 0 ? value / 2 : -(value + 1) / 2);

/* Records of one capture batch, each with its sequence number. state, from
 * captureDecoder(), carries the previous frame across the batches of a
 * recording; a sequence gap or an entered record clears it, since firmware
 * sends the next frame whole after either. A delta frame with no frame to
 * follow is left out and counted in state.skipped, so the converter sees a
 * sequence gap and decoding picks up again at the next whole frame. */
export function parseCaptureBatch(payload, state) {
  requireBytes(payload, 0, 5, "capture batch");
  const view = new DataView(payload.buffer, payload.byteOffset, payload.byteLength);
  const first = view.getUint32(0, true);
  if (first !== state.nextSequence) state.previous = null;
  const records = [];
  let offset = 5;
  for (let index = 0; index < payload[4]; index += 1) {
    requireBytes(payload, offset, 1, "capture record");
    const kind = payload[offset];
    const sequence = (first + index) >>> 0;
    if (kind === CAPTURE.RECORD_FRAME) {
      requireBytes(payload, offset, 20, "capture frame");
      records.push({ sequence, kind, flags: payload[offset + 1],
        timeMs: view.getUint32(offset + 2, true), timeUs: view.getUint16(offset + 6, true),
        x: view.getInt32(offset + 8, true), y: view.getInt32(offset + 12, true),
        spanUs: view.getUint32(offset + 16, true) });
      offset += 20;
    } else if ((kind & 0xf8) === CAPTURE.DELTA_FRAME) {
      const previous = state.previous;
      let elapsed;
      let x;
      let y;
      [elapsed, offset] = readVarint(payload, offset + 1, "capture delta frame");
      [x, offset] = readVarint(payload, offset, "capture delta frame");
      [y, offset] = readVarint(payload, offset, "capture delta frame");
      elapsed = unzigzag(elapsed);
      let flags = previous?.flags;
      if (kind & CAPTURE.DELTA_FLAGS) {
        requireBytes(payload, offset, 1, "capture delta frame");
        flags = payload[offset];
        offset += 1;
      }
      let spanUs;
      const mode = kind & 0x03;
      if (mode === CAPTURE.SPAN_PREVIOUS) spanUs = previous?.spanUs;
      else if (mode === CAPTURE.SPAN_ELAPSED) spanUs = elapsed;
      else {
        let value;
        [value, offset] = readVarint(payload, offset, "capture delta frame");
        spanUs = mode === CAPTURE.SPAN_ABSOLUTE ? value : elapsed + unzigzag(value);
      }
      if (previous === null) {
        state.skipped += 1;
        continue;
      }
      const us = previous.timeMs * 1000 + previous.timeUs + elapsed;
      records.push({ sequence, kind: CAPTURE.RECORD_FRAME, flags,
        timeMs: Math.floor(us / 1000), timeUs: us % 1000, x: unzigzag(x), y: unzigzag(y),
        spanUs });
    } else if (kind === CAPTURE.RECORD_SOURCE) {
      requireBytes(payload, offset, 6, "capture source");
      records.push({ sequence, kind, link: payload[offset + 1],
        sourceId: view.getUint16(offset + 2, true), cpi: view.getUint16(offset + 4, true) });
      offset += 6;
    } else if (kind === CAPTURE.RECORD_KEYPRESS || kind === CAPTURE.RECORD_FLUSH) {
      requireBytes(payload, offset, 5, "capture timed record");
      records.push({ sequence, kind, timeMs: view.getUint32(offset + 1, true) });
      offset += 5;
    } else if (kind === CAPTURE.RECORD_ENTERED) {
      requireBytes(payload, offset, 5, "capture entered record");
      records.push({ sequence, kind, settingsHash: view.getUint32(offset + 1, true) });
      offset += 5;
      state.previous = null;
    } else if (kind === CAPTURE.RECORD_LEFT || kind === CAPTURE.RECORD_SETTINGS_CHANGED) {
      records.push({ sequence, kind });
      offset += 1;
    } else {
      throw new Error(`Unknown capture record kind ${kind}`);
    }
    const record = records[records.length - 1];
    if (record.kind === CAPTURE.RECORD_FRAME) state.previous = record;
  }
  if (offset !== payload.length) throw new Error("Capture batch has trailing bytes");
  state.nextSequence = (first + payload[4]) >>> 0;
  return records;
}

/* A pipeline's settings hash (docs/protocol.md, "Capture pipeline
 * description") from a tuner profile export: the sum, modulo 2^32, of
 * FNV-1a over stage stable id, NUL, key, NUL, and the value as 4
 * little-endian bytes, for every parameter of the stages listed. */
export function captureSettingsHash(profile, stageIds) {
  const encoder = new TextEncoder();
  let hash = 0;
  for (const target of profile.targets) {
    if (!stageIds.includes(target.stableId)) continue;
    for (const { key, value } of target.parameters) {
      const value32 = new Uint8Array(4);
      new DataView(value32.buffer).setInt32(0, value, true);
      let fnv = 0x811c9dc5;
      for (const byte of [...encoder.encode(target.stableId), 0, ...encoder.encode(key), 0,
        ...value32]) {
        fnv = Math.imul(fnv ^ byte, 0x01000193) >>> 0;
      }
      hash = (hash + fnv) >>> 0;
    }
  }
  return hash;
}
