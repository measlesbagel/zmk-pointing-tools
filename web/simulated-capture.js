/* SPDX-License-Identifier: MIT */

/* A simulated input-capture device for the tuner's simulator: one
 * replayable cursor pipeline built from the simulator's tuning targets,
 * answering capture requests with the protocol payloads firmware sends
 * (docs/protocol.md, "Input capture"). While recording it moves a simulated
 * ball in strokes and rests, stores records in a burst buffer the size of
 * firmware's default, and sends them in batches at a fixed link rate, so
 * the buffer fills during long strokes, records that don't fit are dropped
 * and counted, and the pipeline resyncs with an entered record at the next
 * rest, as firmware does. Frames are always sent whole. */

import { CAPTURE, MESSAGE, captureSettingsHash } from "./protocol.js";

const PIPELINE = "cursor";
const STAGES = [
  { strategy: "orthogonal-orientation", stableId: "cursor-orientation",
    fixed: { "swap-xy": 0, "invert-x": 0, "invert-y": 0 } },
  { strategy: "resolution-normalize-mm", stableId: "cursor-normalize", fixed: {} },
  { strategy: "cursor-acceleration", stableId: "cursor-acceleration", fixed: {} },
  { strategy: "cursor-transfer", stableId: "cursor-transfer",
    fixed: { "scale-multiplier": 1, "scale-divisor": 1 } },
  { strategy: "cursor-quantizer", stableId: "cursor-quantizer", fixed: {} },
  { strategy: "cursor-coalescer", stableId: "cursor-coalescer", fixed: {} },
];
const CPI = 800;
const SOURCE_ID = 1;
const BUFFER_SIZE = 4096;
const BATCH_BYTES = 114;
const BATCHES_PER_TICK = 2;
const RESYNC_REST_MS = 40;
const SIZES = { [CAPTURE.RECORD_FRAME]: 20, [CAPTURE.RECORD_SOURCE]: 6,
  [CAPTURE.RECORD_ENTERED]: 5, [CAPTURE.RECORD_SETTINGS_CHANGED]: 1 };
/* Buffer use counts a frame at a delta frame's typical size, as firmware
 * stores most frames; batches still carry whole frames. */
const STORED = { ...SIZES, [CAPTURE.RECORD_FRAME]: 6 };
/* 5 s into every 8 s the simulated link stalls for 2.5 s, as a busy radio
 * can, so the buffer fills and drops and resyncs show. */
const STALL_EVERY_MS = 8_000;
const STALL_FROM_MS = 5_000;
const STALL_MS = 2_500;

const encoder = new TextEncoder();

function bytes(parts) {
  const out = [];
  for (const part of parts) {
    if (typeof part === "string") {
      const text = encoder.encode(part);
      out.push(text.length, ...text);
    } else {
      out.push(...part);
    }
  }
  return Uint8Array.from(out);
}

const u32 = (value) => { const b = new Uint8Array(4); new DataView(b.buffer).setUint32(0, value >>> 0, true); return [...b]; };
const i32 = (value) => { const b = new Uint8Array(4); new DataView(b.buffer).setInt32(0, value, true); return [...b]; };
const u16 = (value) => [value & 0xff, (value >> 8) & 0xff];

function encodeRecord(record) {
  switch (record.kind) {
  case CAPTURE.RECORD_FRAME:
    return [record.kind, record.flags, ...u32(record.timeMs), ...u16(record.timeUs),
      ...i32(record.x), ...i32(record.y), ...u32(record.spanUs)];
  case CAPTURE.RECORD_SOURCE:
    return [record.kind, CAPTURE.LINK_USB, ...u16(SOURCE_ID), ...u16(CPI)];
  case CAPTURE.RECORD_ENTERED:
    return [record.kind, ...u32(record.settingsHash)];
  default:
    return [record.kind];
  }
}

/* A small deterministic generator, so the simulator records the same
 * motion every time. */
function random(seed) {
  let state = seed >>> 0;
  return () => {
    state = (Math.imul(state, 1664525) + 1013904223) >>> 0;
    return state / 2 ** 32;
  };
}

/* Strokes of a few sizes and speeds, from slow aims to flicks, with rests
 * between. Each yields per-millisecond ball travel in counts. */
function* motion(next) {
  for (let stroke = 0; ; stroke += 1) {
    const durationMs = [180, 420, 700, 260, 900][stroke % 5];
    const distanceMm = [6, 18, 13, 85, 70][stroke % 5] * (0.85 + 0.3 * next());
    const angle = next() * 2 * Math.PI;
    const counts = (distanceMm * CPI) / 25.4;
    /* The third stroke is a steady slow aim: short ramps around a
     * constant speed. The rest follow a minimum-jerk profile. */
    const steady = stroke % 5 === 2;
    const ramp = 60 / durationMs;
    const position = (t) => (steady
      ? (t < ramp ? t * t / (2 * ramp) : t > 1 - ramp ? 1 - ramp - (1 - t) ** 2 / (2 * ramp) : t - ramp / 2) / (1 - ramp)
      : 10 * t ** 3 - 15 * t ** 4 + 6 * t ** 5);
    let previous = 0;
    for (let ms = 1; ms <= durationMs; ms += 1) {
      const travelled = counts * position(ms / durationMs);
      const step = travelled - previous;
      previous = travelled;
      yield [step * Math.cos(angle), step * Math.sin(angle)];
    }
    const restMs = 250 + Math.floor(next() * 250);
    for (let ms = 0; ms < restMs; ms += 1) yield null;
  }
}

export class SimulatedCapture {
  /* profile() returns the simulator's current tuning profile; send(frame)
   * delivers a { type, payload } frame to the tuner. */
  constructor({ profile, send }) {
    this.profile = profile;
    this.send = send;
    this.recording = null;
    this.dropped = 0;
    this.nowMs = 10_000;
  }

  get stageIds() {
    return STAGES.map(({ stableId }) => stableId);
  }

  settingsHash() {
    return captureSettingsHash(this.profile(), this.stageIds);
  }

  #status() {
    const label = encoder.encode(PIPELINE);
    const used = this.queue?.reduce((total, record) => total + STORED[record.kind], 0) ?? 0;
    return bytes([[CAPTURE.SCHEMA_VERSION, this.recording === null ? CAPTURE.NONE : 0],
      u32(this.dropped), u32(used), u32(BUFFER_SIZE), [1, 0, 1, STAGES.length,
        CAPTURE.FLAG_REPLAYABLE], u32(this.settingsHash()), [label.length], label]);
  }

  #reply(type, payload) {
    /* Answers arrive asynchronously, as from a device. */
    setTimeout(() => this.send({ type, payload }), 5);
  }

  /* A capture control request: undefined queries, CAPTURE.NONE stops. */
  control(pipelineId) {
    if (pipelineId === CAPTURE.NONE) {
      if (this.recording !== null) this.#drain(Infinity);
      this.recording = null;
    } else if (pipelineId === 0) {
      this.recording = 0;
      this.sequence = 0;
      this.queue = [];
      this.motion = motion(random(0x2b1d));
      this.pendingSettings = undefined;
      this.lastHash = this.settingsHash();
      this.restMs = Infinity;
      this.needEntered = true;
      this.startMs = this.nowMs;
    }
    this.#reply(MESSAGE.CAPTURE_STATUS_RESPONSE, this.#status());
  }

  describe(pipelineId) {
    if (pipelineId !== 0) {
      this.#reply(MESSAGE.TUNING_RESULT, Uint8Array.of(MESSAGE.CAPTURE_DESCRIBE_REQUEST, 1, 0xff, 0xff, 0, 0, 0, 0));
      return;
    }
    const stages = STAGES.flatMap(({ strategy, stableId, fixed }) => [strategy, stableId,
      [Object.keys(fixed).length], ...Object.entries(fixed).flatMap(([key, value]) => [key, i32(value)])]);
    this.#reply(MESSAGE.CAPTURE_DESCRIPTION_RESPONSE,
      bytes([[0], PIPELINE, u32(this.settingsHash()), [STAGES.length], ...stages]));
  }

  #record(record, used) {
    const sequence = this.sequence++;
    if (used + STORED[record.kind] > BUFFER_SIZE) {
      this.dropped += 1;
      this.needEntered = true;
      return used;
    }
    this.queue.push({ ...record, sequence });
    return used + STORED[record.kind];
  }

  /* Advances the simulated clock by ms milliseconds of motion. */
  tick(ms = 8) {
    if (this.recording === null) return;
    let used = this.queue.reduce((total, record) => total + STORED[record.kind], 0);
    const hash = this.settingsHash();
    if (hash !== this.lastHash) {
      this.lastHash = hash;
      used = this.#record({ kind: CAPTURE.RECORD_SETTINGS_CHANGED }, used);
      this.needEntered = true;
    }
    for (let step = 0; step < ms; step += 1) {
      this.nowMs += 1;
      const delta = this.motion.next().value;
      if (delta === null) {
        this.restMs += 1;
        continue;
      }
      if (this.needEntered && this.restMs >= RESYNC_REST_MS) {
        /* A start, or a resync at the first frame after a rest. */
        used = this.#record({ kind: CAPTURE.RECORD_ENTERED, settingsHash: hash }, used);
        used = this.#record({ kind: CAPTURE.RECORD_SOURCE }, used);
        this.needEntered = false;
        this.carry = [0, 0];
        this.lastFrameMs = this.nowMs - 1;
      }
      this.restMs = 0;
      this.carry[0] += delta[0];
      this.carry[1] += delta[1];
      const x = Math.trunc(this.carry[0]);
      const y = Math.trunc(this.carry[1]);
      if (x === 0 && y === 0) continue;
      this.carry[0] -= x;
      this.carry[1] -= y;
      /* The sensor's sample span: the counts built up since the last
       * frame it reported. */
      used = this.#record({ kind: CAPTURE.RECORD_FRAME, flags: 0x01, timeMs: this.nowMs,
        timeUs: 300, x, y, spanUs: Math.min(this.nowMs - this.lastFrameMs, 40) * 1000 }, used);
      this.lastFrameMs = this.nowMs;
    }
    const phase = (this.nowMs - this.startMs) % STALL_EVERY_MS;
    if (phase < STALL_FROM_MS || phase >= STALL_FROM_MS + STALL_MS) this.#drain(BATCHES_PER_TICK);
  }

  /* Sends up to count batches of consecutive records. */
  #drain(count) {
    for (let sent = 0; sent < count && this.queue.length; sent += 1) {
      const records = [];
      let size = 0;
      while (this.queue.length && records.length < 255) {
        const record = this.queue[0];
        const length = SIZES[record.kind];
        if (size + length > BATCH_BYTES) break;
        if (records.length && record.sequence !== records.at(-1).sequence + 1) break;
        records.push(this.queue.shift());
        size += length;
      }
      this.send({ type: MESSAGE.CAPTURE_BATCH,
        payload: bytes([u32(records[0].sequence), [records.length], ...records.map(encodeRecord)]) });
    }
  }
}
