#!/usr/bin/env node
/* SPDX-License-Identifier: MIT */

/* Turns a firmware input capture (docs/protocol.md, "Input capture") into
 * fixture v2 traces that replay through the cursor runner.
 *
 *   node host/replay/capture.js --stream capture.bin --profile profile.json \
 *     --out trace.json [--id my-trace] [--device right-ball] [--provenance "…"]
 *
 * The stream is the device's protocol bytes as received: a capture
 * description (0x8d) and the capture batches (0x92) of one recording; other
 * frames are ignored. The profile is the tuner's export
 * (zmk-pointing-tools/tuning-profile v1) of the settings the device ran
 * while recording. A recording becomes one trace per stretch the pipeline
 * ran unchanged; see captureToFixtures. */

import { readFileSync, writeFileSync } from "node:fs";
import { resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { parseArgs } from "node:util";

import { FIXTURE_SCHEMA, validateFixture, validateProfile } from "./fixture.js";
import {
  CAPTURE,
  FrameDecoder,
  MESSAGE,
  parseCaptureBatch,
  parseCaptureDescription,
} from "../../web/protocol.js";

/* Firmware stage strategies and the cursor runner's stage types. */
const STAGE_TYPES = {
  "orthogonal-orientation": "orientation",
  "resolution-normalize-mm": "normalize",
  "coherent-displacement": "gate",
  "cursor-acceleration": "acceleration",
  "cursor-transfer": "transfer",
  "cursor-quantizer": "quantizer",
  "cursor-coalescer": "coalescer",
};

const SIGNAL_FLAG_TRANSPORTED = 1 << 1;
const SIGNAL_FLAG_SPAN_CLIPPED = 1 << 7;
/* The acceleration stage restarts its speed estimate on a clipped span or
 * one this long (ZPT_CURSOR_ACCELERATION_RESTART_MS); a fixture has no
 * flags, so a clipped span is written as this span. */
const RESTART_SPAN_US = 40000;
/* The keypress guard's window is a 16-bit millisecond count, so an older
 * key press can't affect the recording. */
const KEYPRESS_HORIZON_MS = 0xffff;

/* The description and records of the one recording in a protocol stream. */
export function decodeCaptureStream(bytes) {
  const decoder = new FrameDecoder();
  let description = null;
  const records = [];
  for (const frame of decoder.push(bytes)) {
    if (frame.type === MESSAGE.CAPTURE_DESCRIPTION_RESPONSE) {
      description = parseCaptureDescription(frame.payload);
    } else if (frame.type === MESSAGE.CAPTURE_BATCH) {
      records.push(...parseCaptureBatch(frame.payload));
    }
  }
  if (description === null) throw new Error("stream has no capture description");
  return { description, records };
}

function cursorStages(description, profile) {
  const targets = new Map(profile.targets.map((target) => [target.stableId, target]));
  return description.stages.map((stage) => {
    const type = STAGE_TYPES[stage.strategy];
    if (type === undefined) {
      throw new Error(`${description.stableId}: stage ${stage.stableId} (${stage.strategy}) ` +
        "isn't a cursor stage; only cursor pipelines replay so far");
    }
    const tuned = targets.get(stage.stableId)?.parameters ?? [];
    const settings = {
      ...Object.fromEntries(tuned.map(({ key, value }) => [key, value])),
      ...stage.fixed,
    };
    return Object.keys(settings).length === 0
      ? { type, stableId: stage.stableId }
      : { type, stableId: stage.stableId, settings };
  });
}

/* Split records into stretches that replay from reset stages: each starts
 * at an entered record and ends at a left record, a settings change, a
 * dropped record, or a change of source, resolution, or link, after which
 * nothing replays until the pipeline is entered again. */
function segments(records, warnings) {
  const result = [];
  let current = null;
  let keypressMs = null;
  let expected = records[0]?.sequence ?? 0;
  const end = (reason) => {
    if (current !== null) {
      if (reason !== null) warnings.push(`trace ${result.length + 1} ends early: ${reason}`);
      if (current.events.length > 0) result.push(current);
    }
    current = null;
  };
  for (const record of records) {
    if (record.sequence !== expected) {
      warnings.push(`${record.sequence - expected} records dropped before ${record.sequence}`);
      end(null);
    }
    expected = record.sequence + 1;
    switch (record.kind) {
      case CAPTURE.RECORD_ENTERED:
        end(current === null ? null : "the pipeline was entered again");
        current = { source: null, events: [], keypressMs };
        break;
      case CAPTURE.RECORD_LEFT:
        end(null);
        break;
      case CAPTURE.RECORD_SETTINGS_CHANGED:
        end("settings changed while recording");
        break;
      case CAPTURE.RECORD_KEYPRESS:
        keypressMs = record.timeMs;
        current?.events.push(record);
        break;
      case CAPTURE.RECORD_SOURCE:
        if (current?.source !== null && current?.source !== undefined) {
          end("the source, resolution, or report link changed");
        } else if (current !== null) {
          current.source = record;
        }
        break;
      default:
        if (current !== null) {
          if (record.kind === CAPTURE.RECORD_FRAME && current.source === null) {
            throw new Error(`frame ${record.sequence} before its source record`);
          }
          current.events.push(record);
        }
    }
  }
  end(null);
  return result;
}

/* Fixture events from records. Times count from the millisecond of the
 * first event, or of the last key press before it while the guard could
 * still remember it, so each frame keeps the millisecond its stages saw. */
function fixtureEvents(segment, warnings) {
  const isKeypress = (event) => event.kind === CAPTURE.RECORD_KEYPRESS;
  const startMs = (segment.events.find((event) => !isKeypress(event)) ?? segment.events[0]).timeMs;
  const recent = (timeMs) => ((startMs - timeMs) | 0) <= KEYPRESS_HORIZON_MS;
  const events = segment.events.filter((event) => !isKeypress(event) || recent(event.timeMs));
  if (segment.keypressMs !== null && recent(segment.keypressMs) &&
      !events.some((event) => isKeypress(event) && event.timeMs === segment.keypressMs)) {
    events.unshift({ kind: CAPTURE.RECORD_KEYPRESS, timeMs: segment.keypressMs });
  }
  const firstMs = events[0].timeMs;
  const baseMs = events.reduce((base, event) =>
    ((event.timeMs - base) | 0) < 0 ? event.timeMs : base, firstMs);
  let previous = 0;
  let reordered = 0;
  let spans = false;
  const out = events.map((event) => {
    let time = ((event.timeMs - baseMs) >>> 0) * 1000 +
      (event.kind === CAPTURE.RECORD_FRAME ? event.timeUs : 0);
    if (time < previous) {
      /* Flushes and key presses carry only their millisecond, so within a
       * frame's millisecond they move to its time, which the stages can't
       * tell apart. An event recorded after a later millisecond can't go
       * back in time on replay. */
      if (Math.floor(time / 1000) !== Math.floor(previous / 1000)) reordered += 1;
      time = previous;
    }
    const delta = time - previous;
    previous = time;
    if (event.kind === CAPTURE.RECORD_FRAME) {
      const span = event.flags & SIGNAL_FLAG_SPAN_CLIPPED
        ? Math.max(event.spanUs, RESTART_SPAN_US) : event.spanUs;
      spans ||= span !== 0;
      return ["motion", delta, event.x, event.y, span];
    }
    return [event.kind === CAPTURE.RECORD_FLUSH ? "flush" : "keypress", delta];
  });
  if (reordered > 0) {
    warnings.push(`${reordered} events arrived after a later one and replay at its time`);
  }
  return { events: out, spans };
}

/* Fixture v2 traces from a decoded capture and the settings it ran with.
 * Returns { fixtures, warnings }. */
export function captureToFixtures({ description, records }, profile, options = {}) {
  validateProfile(profile);
  const stages = cursorStages(description, profile);
  const warnings = [];
  const parts = segments(records, warnings);
  if (parts.length === 0) warnings.push("the recording has no replayable frames");
  const id = options.id ?? `capture-${description.stableId}`;
  const fixtures = parts.map((segment, index) => {
    const { events, spans } = fixtureEvents(segment, warnings);
    const frames = segment.events.filter((event) => event.kind === CAPTURE.RECORD_FRAME);
    const transported = frames.some((frame) => frame.flags & SIGNAL_FLAG_TRANSPORTED);
    const fixture = {
      schema: FIXTURE_SCHEMA,
      version: 2,
      id: parts.length === 1 ? id : `${id}-${index + 1}`,
      description: options.description ??
        `Input capture of pipeline ${description.stableId}`,
      metadata: {
        mode: "cursor",
        synthetic: false,
        provenance: options.provenance ?? "firmware input capture (host/replay/capture.js)",
        capture: {
          pipeline: description.stableId,
          sourceId: segment.source.sourceId,
          firstSequence: segment.events[0].sequence ?? null,
        },
      },
      source: {
        device: options.device ?? `source-${segment.source.sourceId}`,
        location: transported ? "peripheral" : "central",
        cpi: segment.source.cpi,
        span: spans ? "sensor" : "none",
      },
      processor: {
        kind: "cursor-pipeline",
        link: segment.source.link === CAPTURE.LINK_BLE ? "ble" : "usb",
        stages,
        profile,
      },
      events,
    };
    return validateFixture(fixture, fixture.id);
  });
  return { fixtures, warnings };
}

function main() {
  const { values } = parseArgs({
    options: {
      stream: { type: "string" },
      profile: { type: "string" },
      out: { type: "string" },
      id: { type: "string" },
      device: { type: "string" },
      provenance: { type: "string" },
    },
  });
  if (!values.stream || !values.profile || !values.out) {
    console.error("usage: capture.js --stream <bytes> --profile <profile.json> --out <trace.json>");
    process.exit(2);
  }
  const capture = decodeCaptureStream(new Uint8Array(readFileSync(values.stream)));
  const profile = JSON.parse(readFileSync(values.profile, "utf8"));
  const { fixtures, warnings } = captureToFixtures(capture, profile, values);
  for (const warning of warnings) console.warn(`warning: ${warning}`);
  fixtures.forEach((fixture, index) => {
    const path = fixtures.length === 1 ? values.out
      : values.out.replace(/(\.json)?$/, `-${index + 1}.json`);
    writeFileSync(path, `${JSON.stringify(fixture, null, 2)}\n`);
    console.log(`${path}: ${fixture.events.length} events`);
  });
}

if (process.argv[1] && resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  main();
}
