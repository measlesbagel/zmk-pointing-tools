#!/usr/bin/env node
/* SPDX-License-Identifier: MIT */

/* Turns a firmware input capture (docs/protocol.md, "Input capture") into
 * fixture v2 traces that replay through the cursor runner.
 *
 *   node host/replay/capture.js --stream capture.bin --profile a.json \
 *     [--profile b.json …] --out trace.json [--id my-trace] [--device right-ball] \
 *     [--provenance "…"]
 *
 * The stream is the device's protocol bytes as received: a capture
 * description (0x8d) and the capture batches (0x92) of one recording; other
 * frames are ignored. Each profile is a tuner export
 * (zmk-pointing-tools/tuning-profile v1) of settings the device ran while
 * recording, such as the two sides of an A/B comparison. A recording
 * becomes one trace per stretch the pipeline ran unchanged, each with the
 * profile whose settings hash its entered record carries; see
 * captureToFixtures. */

import { readFileSync, writeFileSync } from "node:fs";
import { resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { parseArgs } from "node:util";

import { FIXTURE_SCHEMA, validateFixture, validateProfile } from "./fixture.js";
import {
  CAPTURE,
  FrameDecoder,
  MESSAGE,
  captureSettingsHash,
  parseCaptureBatch,
  parseCaptureDescription,
  parseCaptureStatus,
  parseDescribe,
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

/* The description, capture statuses, and records of the one recording in
 * a protocol stream. */
export function decodeCaptureStream(bytes) {
  const decoder = new FrameDecoder();
  let description = null;
  const statuses = [];
  const records = [];
  for (const frame of decoder.push(bytes)) {
    if (frame.type === MESSAGE.DESCRIBE_RESPONSE) {
      /* A stream that includes the handshake must be from v8 firmware. */
      parseDescribe(frame.payload);
    } else if (frame.type === MESSAGE.CAPTURE_DESCRIPTION_RESPONSE) {
      description = parseCaptureDescription(frame.payload);
    } else if (frame.type === MESSAGE.CAPTURE_STATUS_RESPONSE) {
      statuses.push(parseCaptureStatus(frame.payload));
    } else if (frame.type === MESSAGE.CAPTURE_BATCH) {
      records.push(...parseCaptureBatch(frame.payload));
    }
  }
  if (description === null) throw new Error("stream has no capture description");
  return { description, statuses, records };
}

/* The profile whose settings hash is the one the device ran a trace with,
 * or undefined. */
function matchingProfile(description, profiles, settingsHash) {
  const stageIds = description.stages.map((stage) => stage.stableId);
  return profiles.find((profile) => captureSettingsHash(profile, stageIds) === settingsHash);
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
 * nothing replays until the next entered record. After a drop, firmware
 * resets the pipeline and sends one at the next rest (docs/protocol.md,
 * "Drops"). Each stretch starts with its own key press record. */
function segments(records, warnings) {
  const result = [];
  let current = null;
  let expected = records[0]?.sequence ?? 0;
  if (expected !== 0) {
    warnings.push(`the recording starts at record ${expected}; earlier records were dropped`);
  }
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
        current = { source: null, events: [], settingsHash: record.settingsHash };
        break;
      case CAPTURE.RECORD_LEFT:
        end(null);
        break;
      case CAPTURE.RECORD_SETTINGS_CHANGED:
        end("settings changed while recording");
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

/* Fixture events from records. Times count from the earliest event, a key
 * press the guard still remembers included, so each frame keeps its
 * microsecond. An event recorded after a later one moves to that one's time
 * and carries how many milliseconds earlier its stages saw it, so replay
 * hands every stage the millisecond firmware did. */
function fixtureEvents(segment, windowMs) {
  const isKeypress = (event) => event.kind === CAPTURE.RECORD_KEYPRESS;
  const startMs = (segment.events.find((event) => !isKeypress(event)) ?? segment.events[0]).timeMs;
  /* A key press older than the guard's window can't suppress anything. */
  const events = segment.events.filter((event) =>
    !isKeypress(event) || ((startMs - event.timeMs) | 0) < windowMs);
  if (events.length === 0) return null;
  const baseMs = events.reduce((base, event) =>
    ((event.timeMs - base) | 0) < 0 ? event.timeMs : base, events[0].timeMs);
  let previous = 0;
  let spans = false;
  const out = events.map((event) => {
    const ownMs = (event.timeMs - baseMs) >>> 0;
    const own = ownMs * 1000 + (event.kind === CAPTURE.RECORD_FRAME ? event.timeUs : 0);
    const time = Math.max(own, previous);
    const late = Math.floor(time / 1000) - ownMs;
    const delta = time - previous;
    previous = time;
    if (event.kind === CAPTURE.RECORD_FRAME) {
      const span = event.flags & SIGNAL_FLAG_SPAN_CLIPPED
        ? Math.max(event.spanUs, RESTART_SPAN_US) : event.spanUs;
      spans ||= span !== 0;
      return late > 0 ? ["motion", delta, event.x, event.y, span, late]
        : ["motion", delta, event.x, event.y, span];
    }
    const type = event.kind === CAPTURE.RECORD_FLUSH ? "flush" : "keypress";
    return late > 0 ? [type, delta, late] : [type, delta];
  });
  return { events: out, spans, startMs: baseMs };
}

/* Fixture v2 traces from a decoded capture and the settings it ran with:
 * one profile, or several when settings changed while recording. A trace
 * whose settings hash no profile matches is skipped with a warning.
 * Returns { fixtures, warnings }. */
export function captureToFixtures(capture, profiles, options = {}) {
  const { description, records } = capture;
  profiles = Array.isArray(profiles) ? profiles : [profiles];
  for (const profile of profiles) {
    validateProfile(profile);
    cursorStages(description, profile);
  }
  const windowMs = description.stages.find((stage) => stage.strategy === "coherent-displacement")
    ?.fixed["suppress-after-keypress-ms"] ?? 0;
  const warnings = [];
  /* A stretch without frames (only flushes or key presses) replays nothing. */
  const parts = segments(records, warnings).filter((segment) => segment.source !== null)
    .map((segment) => ({ segment, converted: fixtureEvents(segment, windowMs),
      profile: matchingProfile(description, profiles, segment.settingsHash) }))
    .filter(({ converted }) => converted !== null)
    .filter(({ segment, profile }) => {
      if (profile === undefined) {
        warnings.push(`trace from record ${segment.events[0].sequence} skipped: no profile ` +
          `holds the settings it ran with (settings hash ${segment.settingsHash?.toString(16)}); ` +
          "export a profile from the keyboard for each setting the recording ran");
      }
      return profile !== undefined;
    });
  if (parts.length === 0) warnings.push("the recording has no replayable frames");
  const id = options.id ?? `capture-${description.stableId}`;
  const fixtures = parts.map(({ segment, converted, profile }, index) => {
    const { events, spans, startMs } = converted;
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
          settingsHash: segment.settingsHash,
          /* Device uptime in milliseconds at fixture time zero. */
          startMs,
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
        stages: cursorStages(description, profile),
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
      profile: { type: "string", multiple: true },
      out: { type: "string" },
      id: { type: "string" },
      device: { type: "string" },
      provenance: { type: "string" },
    },
  });
  if (!values.stream || !values.profile || !values.out) {
    console.error("usage: capture.js --stream <bytes> --profile <profile.json> [--profile …] " +
      "--out <trace.json>");
    process.exit(2);
  }
  const capture = decodeCaptureStream(new Uint8Array(readFileSync(values.stream)));
  const profiles = values.profile.map((path) => JSON.parse(readFileSync(path, "utf8")));
  const { fixtures, warnings } = captureToFixtures(capture, profiles, values);
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
