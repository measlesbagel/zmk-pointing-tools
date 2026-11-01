/* SPDX-License-Identifier: MIT */

/* Turns a firmware input capture (docs/protocol.md, "Input capture") into
 * fixture v2 traces of cursor, scroll, or text pipelines that replay
 * through the stage-chain runner. capture.js wraps
 * it as a command; the tuner loads it as is from the Pages build's replay/
 * module, so it has no Node imports. host/wasm/build.sh rewrites the
 * protocol import below for that layout, where replay/ sits next to the
 * tuner's protocol.js. */

import { FIXTURE_SCHEMA, eventTimesUs, validateFixture, validateProfile } from "./fixture.js";
import {
  CAPTURE,
  FrameDecoder,
  MESSAGE,
  captureSettingsHash,
  captureDecoder,
  parseCaptureBatch,
  parseCaptureDescription,
  parseCaptureStatus,
  parseDescribe,
} from "../../web/protocol.js";

/* Firmware stage strategies and the chain runner's stage types. */
const STAGE_TYPES = {
  "orthogonal-orientation": "orientation",
  "resolution-normalize-mm": "normalize",
  "coherent-displacement": "gate",
  "cursor-acceleration": "acceleration",
  "cursor-transfer": "transfer",
  "cursor-quantizer": "quantizer",
  "cursor-coalescer": "coalescer",
  "axis-intent": "axis-intent",
  "axis-constraint": "axis-constraint",
  "scroll-batcher": "scroll-batcher",
  "text-nav": "text-nav",
};
/* A pipeline's fixture kind and mode follow from its last stage. */
const CHAIN_MODES = {
  "scroll-batcher": { kind: "scroll-pipeline", mode: "scroll" },
  "text-nav": { kind: "text-pipeline", mode: "text" },
};
const CURSOR_MODE = { kind: "cursor-pipeline", mode: "cursor" };
/* Motion this soon after a key press counts as typing shake in an
 * unlabelled scroll or text trace (the synthetic benchmark's shake lasts
 * 50 to 60 ms per key). */
const TYPING_SHAKE_US = 100000;
const LABEL_AXES = ["horizontal", "vertical"];

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
  const batches = captureDecoder();
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
      records.push(...parseCaptureBatch(frame.payload, batches));
    }
  }
  if (description === null) throw new Error("stream has no capture description");
  return { description, statuses, records, skipped: batches.skipped };
}

/* The profile whose settings hash is the one the device ran a trace with,
 * or undefined. */
function matchingProfile(description, profiles, settingsHash) {
  const stageIds = description.stages.map((stage) => stage.stableId);
  return profiles.find((profile) => captureSettingsHash(profile, stageIds) === settingsHash);
}

function chainStages(description, profile) {
  const targets = new Map(profile.targets.map((target) => [target.stableId, target]));
  return description.stages.map((stage) => {
    const type = STAGE_TYPES[stage.strategy];
    if (type === undefined) {
      throw new Error(`${description.stableId}: stage ${stage.stableId} (${stage.strategy}) ` +
        "doesn't replay; only cursor, scroll, and text pipeline stages do");
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

/* Intervals of [start, end) left after removing the cuts. */
function subtract(intervals, cuts) {
  return cuts.reduce((left, [cutStart, cutEnd]) => left.flatMap(([start, end]) =>
    [[start, Math.min(end, cutStart)], [Math.max(start, cutEnd), end]]
      .filter(([from, to]) => to > from)), intervals);
}

function validateLabels(labels) {
  for (const [index, label] of labels.entries()) {
    if (!Number.isSafeInteger(label?.startMs) || !Number.isSafeInteger(label.endMs) ||
        label.endMs <= label.startMs || !["scroll", "text"].includes(label.activity) ||
        !LABEL_AXES.includes(label.axis)) {
      throw new Error(`label ${index} needs startMs < endMs (device uptime), activity scroll ` +
        "or text, and axis horizontal or vertical");
    }
  }
  const sorted = [...labels].sort((a, b) => a.startMs - b.startMs);
  if (sorted.some((label, index) => index > 0 && label.startMs < sorted[index - 1].endMs)) {
    throw new Error("labels overlap");
  }
}

/* Ground truth for a scroll or text trace: labelled stretches score
 * against their axis; motion right after a key press is typing shake the
 * user didn't mean; the rest is unlabelled, and only the metrics that need
 * no truth apply to it. The strokes don't overlap. */
function truthStrokes(fixture, startMs, mode, labels) {
  const times = eventTimesUs(fixture);
  const endUs = times.at(-1) + 1;
  const toUs = (ms) => Math.min(Math.max(((ms - startMs) | 0) * 1000, 0), endUs);
  const labelled = labels.filter((label) => label.activity === mode)
    .map((label) => ({ ...label, startUs: toUs(label.startMs), endUs: toUs(label.endMs) }))
    .filter((label) => label.endUs > label.startUs);
  const cuts = labelled.map((label) => [label.startUs, label.endUs]);
  const keys = fixture.events.flatMap((event, index) =>
    event[0] === "keypress" ? [[times[index], Math.min(times[index] + TYPING_SHAKE_US, endUs)]]
      : []);
  const typing = subtract(keys.reduce((merged, [start, end]) => {
    const last = merged.at(-1);
    if (last && start <= last[1]) last[1] = Math.max(last[1], end);
    else merged.push([start, end]);
    return merged;
  }, []), cuts);
  const moved = ([start, end]) => fixture.events.some((event, index) =>
    event[0] === "motion" && times[index] >= start && times[index] < end);
  const unlabelled = subtract([[0, endUs]], [...cuts, ...typing]).filter(moved);
  return [
    ...labelled.map((label, index) => ({ id: `label-${index + 1}`, startUs: label.startUs,
      endUs: label.endUs, activity: mode, intended: true, axis: label.axis })),
    ...typing.filter(moved).map(([start, end], index) => ({ id: `typing-${index + 1}`,
      startUs: start, endUs: end, activity: "post-typing", intended: false })),
    ...unlabelled.map(([start, end], index) => ({ id: `unlabelled-${index + 1}`,
      startUs: start, endUs: end, activity: mode, intended: true })),
  ].sort((a, b) => a.startUs - b.startUs);
}

/* Fixture v2 traces from a decoded capture and the settings it ran with:
 * one profile, or several when settings changed while recording. A trace
 * whose settings hash no profile matches is skipped with a warning.
 * options.labels, for scroll and text pipelines, lists stretches whose
 * intended axis the user or an agent named: { startMs, endMs, activity:
 * "scroll" | "text", axis: "horizontal" | "vertical" } in device uptime.
 * Returns { fixtures, warnings }. */
export function captureToFixtures(capture, profiles, options = {}) {
  const { description, records } = capture;
  profiles = Array.isArray(profiles) ? profiles : [profiles];
  for (const profile of profiles) {
    validateProfile(profile);
    chainStages(description, profile);
  }
  const labels = options.labels ?? [];
  validateLabels(labels);
  const { kind, mode } = CHAIN_MODES[description.stages.at(-1)?.strategy] ?? CURSOR_MODE;
  /* The stages that take a keypress guard share one window. */
  const windowMs = Math.max(0, ...description.stages.map((stage) =>
    stage.fixed["suppress-after-keypress-ms"] ?? 0));
  const warnings = [];
  if (capture.skipped > 0) {
    warnings.push(`${capture.skipped} delta frames had no frame to follow and were skipped`);
  }
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
        mode,
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
        kind,
        /* Only a cursor coalescer reads the link. */
        ...(kind !== "cursor-pipeline" && segment.source.link === CAPTURE.LINK_NONE ? {}
          : { link: segment.source.link === CAPTURE.LINK_BLE ? "ble" : "usb" }),
        stages: chainStages(description, profile),
        profile,
      },
      events,
    };
    if (mode !== "cursor") {
      fixture.truth = { strokes: truthStrokes(fixture, startMs, mode, labels) };
    }
    return validateFixture(fixture, fixture.id);
  });
  return { fixtures, warnings };
}
