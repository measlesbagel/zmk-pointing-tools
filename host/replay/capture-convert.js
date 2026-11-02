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

/* A pipeline's mode, cursor, scroll, or text, from its description. The
 * tuner picks a guided test's pipeline with it. */
export const pipelineMode = (description) =>
  (CHAIN_MODES[description.stages.at(-1)?.strategy] ?? CURSOR_MODE).mode;
/* Intended axes of scroll and text labels; "both" (a two-axis scroll) means
 * the user meant to move on both axes at once. */
const LABEL_AXES = { scroll: ["horizontal", "vertical", "both"], text: ["horizontal", "vertical"] };
const POINTER_ACCELERATION = ["off", "on", "unknown"];
/* Browser pointer samples kept on either side of a cursor label, so the
 * screen scale fit can slide the two clocks against each other. */
const POINTER_MARGIN_US = 150000;
/* A rest this long inside a scroll or text label splits it into one
 * stroke per gesture, so each gesture's lock and output delays count. */
const LABEL_REST_US = 100000;
const TRUTH_MODES = ["labels-only", "derive"];
/* Derived truth (docs/trace-benchmark.md, "Recorded traces: ground
 * truth"). A rest this long ends a stroke: the acceleration stage's
 * restart gap, which is also when firmware resyncs a recording. */
const STROKE_GAP_US = 40000;
/* Motion that starts this soon after a key press is typing shake unless
 * it is a reach: a straight stroke of at least REACH_MIN_TRAVEL_MM. */
const TYPING_GAP_US = 150000;
const REACH_MIN_TRAVEL_MM = 1;
const MIN_FRAMES = 5;
const MIN_TRAVEL_MM = 0.3;
/* Net travel under this share of the path travelled: motion that goes
 * nowhere. */
const MIN_STRAIGHTNESS = 0.5;
const PATH_INTERVAL_US = 2000;
const SMOOTH_HALF_WINDOW = 2;
const STEADY_CV = 0.15;
const STEADY_WINDOW_US = 50000;
const SLOW_MM_PER_S = 50;
/* A peak averaged over FLICK_WINDOW_US reads a little under the true peak,
 * so this sits below the 250 mm/s the benchmark calls a flick. */
const FLICK_MM_PER_S = 200;
const FLICK_WINDOW_US = 10000;
const FLICK_EDGE = 0.1;
const FLICK_MAX_US = 300000;

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

const isPoint = (point) => Number.isFinite(point?.x) && Number.isFinite(point?.y);

/* Labels say what the user meant in a stretch: for scroll and text, the
 * intended axis; for a cursor trial, its target and where it started, in
 * the screen's CSS pixels. test and trial name the guided test and trial
 * the label came from. */
function validateLabels(labels) {
  for (const [index, label] of labels.entries()) {
    if (!Number.isSafeInteger(label?.startMs) || !Number.isSafeInteger(label.endMs) ||
        label.endMs <= label.startMs || !["cursor", "scroll", "text"].includes(label.activity)) {
      throw new Error(`label ${index} needs startMs < endMs (device uptime) and activity ` +
        "cursor, scroll, or text");
    }
    if (label.activity === "cursor" ? !(isPoint(label.target) && label.target.size > 0 &&
        isPoint(label.start) && label.axis === undefined)
      : !LABEL_AXES[label.activity].includes(label.axis)) {
      throw new Error(`label ${index}: a cursor label needs target { x, y, size } and start ` +
        "{ x, y }; a scroll label needs axis horizontal, vertical, or both; a text label " +
        "horizontal or vertical");
    }
    if ((label.test !== undefined && typeof label.test !== "string") ||
        (label.trial !== undefined && !Number.isSafeInteger(label.trial))) {
      throw new Error(`label ${index}: test is a name and trial an integer`);
    }
  }
  const sorted = [...labels].sort((a, b) => a.startMs - b.startMs);
  if (sorted.some((label, index) => index > 0 && label.startMs < sorted[index - 1].endMs)) {
    throw new Error("labels overlap");
  }
}

/* The browser's pointer path during a guided test: samples [ms, x, y] in
 * device uptime and the labels' CSS pixels, and whether the computer's own
 * pointer acceleration was off, as the user answered. */
function validatePointer(pointer) {
  if (pointer === undefined) return;
  if (!Array.isArray(pointer?.samples) || !pointer.samples.every((sample) =>
    Array.isArray(sample) && sample.length === 3 && sample.every(Number.isFinite)) ||
      !POINTER_ACCELERATION.includes(pointer.acceleration)) {
    throw new Error("options.pointer needs samples [ms, x, y] and acceleration " +
      `${POINTER_ACCELERATION.join(", ")}`);
  }
}

/* Motion events of a trace with absolute times. */
function motionFrames(fixture) {
  const times = eventTimesUs(fixture);
  return fixture.events.flatMap((event, index) => (event[0] === "motion"
    ? [{ tUs: times[index], x: event[2], y: event[3], spanUs: event[4] ?? 0 }] : []));
}

/* Positions every PATH_INTERVAL_US from startUs to endUs, interpolated
 * between frames, then smoothed by a short moving average whose ends stay
 * put, so the path keeps the stroke's net travel. */
function strokePath(frames, startUs, endUs) {
  const knots = [{ tUs: startUs, x: 0, y: 0 }];
  for (const frame of frames) {
    const last = knots.at(-1);
    knots.push({ tUs: frame.tUs, x: last.x + frame.x, y: last.y + frame.y });
  }
  const raw = [];
  let index = 0;
  for (let tUs = startUs; ; tUs += PATH_INTERVAL_US) {
    const t = Math.min(tUs, endUs);
    while (index < knots.length - 2 && knots[index + 1].tUs <= t) index += 1;
    const a = knots[index];
    const b = knots[Math.min(index + 1, knots.length - 1)];
    const f = b.tUs > a.tUs ? Math.min(Math.max((t - a.tUs) / (b.tUs - a.tUs), 0), 1) : 1;
    raw.push([a.x + (b.x - a.x) * f, a.y + (b.y - a.y) * f]);
    if (t >= endUs) break;
  }
  if (raw.length === 1) raw.push(raw[0]);
  return raw.map((point, i) => {
    if (i === 0 || i === raw.length - 1) return point;
    const half = Math.min(SMOOTH_HALF_WINDOW, i, raw.length - 1 - i);
    const window = raw.slice(i - half, i + half + 1);
    return [0, 1].map((axis) => window.reduce((sum, p) => sum + p[axis], 0) / window.length);
  }).map(([x, y]) => [Math.round(x * 1000) / 1000, Math.round(y * 1000) / 1000]);
}

const stepLengths = (points) => points.slice(1).map((point, i) =>
  Math.hypot(point[0] - points[i][0], point[1] - points[i][1]));
const sum = (values) => values.reduce((total, value) => total + value, 0);

/* Whether a path's speed, averaged over STEADY_WINDOW_US so sparse slow
 * frames don't read as changes, stays within STEADY_CV of its mean,
 * leaving out the first and last fifth where the ball speeds up and slows
 * down. */
function steady(points) {
  const steps = windowedSpeeds(points, 1, STEADY_WINDOW_US);
  const middle = steps.slice(Math.floor(steps.length / 5), Math.ceil((steps.length * 4) / 5));
  if (middle.length < 5) return false;
  const mean = sum(middle) / middle.length;
  if (mean === 0) return false;
  const sd = Math.sqrt(sum(middle.map((step) => (step - mean) ** 2)) / middle.length);
  return sd / mean <= STEADY_CV;
}

/* Speed along a path in mm/s, averaged over windowUs around each point. */
function windowedSpeeds(points, mmPerCount, windowUs = FLICK_WINDOW_US) {
  const steps = stepLengths(points);
  const half = Math.round(windowUs / PATH_INTERVAL_US / 2);
  return steps.map((_, i) => {
    const window = steps.slice(Math.max(0, i - half), i + half + 1);
    return (sum(window) / window.length) * mmPerCount * (1e6 / PATH_INTERVAL_US);
  });
}

/* One derived stroke from a group of frames (see docs/trace-benchmark.md,
 * "Recorded traces: ground truth"), or null when it is too small to call. */
function deriveStroke(frames, mode, keyTimes, mmPerCount, floorUs) {
  /* The first frame's span reaches back over the rest before it, so the
   * stroke starts one typical frame interval earlier instead, but not
   * inside a label (floorUs). */
  const gaps = frames.slice(1).map((frame, i) => frame.tUs - frames[i].tUs).sort((a, b) => a - b);
  const leadUs = gaps.length ? gaps[Math.floor(gaps.length / 2)] : 1000;
  const startUs = Math.max(floorUs, frames[0].tUs - Math.min(frames[0].spanUs || leadUs, leadUs));
  const endUs = frames.at(-1).tUs + 1;
  const base = { startUs, endUs, source: "derived" };
  const points = strokePath(frames, startUs, endUs);
  const travel = sum(stepLengths(points));
  const straight = Math.hypot(...points.at(-1)) >= MIN_STRAIGHTNESS * travel;
  /* Typing: strokes split at key presses, so this one follows its key.
   * Short or crooked motion soon after it is shake; a straight stroke that
   * goes somewhere is the hand reaching for the ball. */
  const reach = straight && frames.length >= MIN_FRAMES &&
    travel * mmPerCount >= REACH_MIN_TRAVEL_MM;
  if (!reach && keyTimes.some((key) => key <= frames[0].tUs &&
    startUs - key < TYPING_GAP_US)) {
    return { ...base, source: "typing", activity: "post-typing", intended: false };
  }
  if (frames.length < MIN_FRAMES || travel * mmPerCount < MIN_TRAVEL_MM) return null;
  /* Motion that goes nowhere, such as tremor, isn't meant. */
  if (!straight) return { ...base, activity: "tremor", intended: false };
  if (mode !== "cursor") return { ...base, activity: mode, intended: true, path: {
    intervalUs: PATH_INTERVAL_US, points } };
  const speeds = windowedSpeeds(points, mmPerCount);
  const peak = Math.max(...speeds);
  if (peak >= FLICK_MM_PER_S) {
    /* A flick spans the steps faster than a tenth of its peak. */
    const first = speeds.findIndex((speed) => speed >= peak * FLICK_EDGE);
    const last = speeds.findLastIndex((speed) => speed >= peak * FLICK_EDGE);
    const flickStart = startUs + first * PATH_INTERVAL_US;
    const flickEnd = Math.min(endUs, startUs + (last + 1) * PATH_INTERVAL_US);
    if (flickEnd - flickStart <= FLICK_MAX_US) {
      const inside = frames.filter((frame) => frame.tUs > flickStart && frame.tUs <= flickEnd);
      return { ...base, startUs: flickStart, endUs: flickEnd, activity: "flick", intended: true,
        path: { intervalUs: PATH_INTERVAL_US, points: strokePath(inside, flickStart, flickEnd) } };
    }
  }
  const isSteady = steady(points);
  const speed = (travel * mmPerCount * 1e6) / (endUs - startUs);
  return { ...base, activity: isSteady && speed <= SLOW_MM_PER_S ? "slow-aim" : "aim",
    intended: true, ...(isSteady ? { speedMmPerS: Math.round(speed * 10) / 10 } : {}),
    path: { intervalUs: PATH_INTERVAL_US, points } };
}

/* Shake right after a key press runs into the reach that follows when the
 * hand leaves the keys within STROKE_GAP_US. The reach starts where the
 * stroke last stands furthest back along its net direction; frames before
 * that are the shake. */
function splitShake(frames) {
  const positions = [[0, 0]];
  for (const frame of frames) {
    const [x, y] = positions.at(-1);
    positions.push([x + frame.x, y + frame.y]);
  }
  const [nx, ny] = positions.at(-1);
  const along = positions.map(([x, y]) => x * nx + y * ny);
  const onset = along.lastIndexOf(Math.min(...along));
  return onset > 0 && onset < frames.length ? [frames.slice(0, onset), frames.slice(onset)]
    : [frames];
}

const traceMode = (fixture) => ({ "scroll-pipeline": "scroll", "composed-scroll": "scroll",
  "text-pipeline": "text", "composed-text": "text" }[fixture.processor.kind] ?? "cursor");

/* Ground truth derived from a trace's own motion for the stretches outside
 * cuts ([startUs, endUs) intervals that labels already cover). */
export function deriveTruthStrokes(fixture, cuts = []) {
  const mode = traceMode(fixture);
  const times = eventTimesUs(fixture);
  const keyTimes = fixture.events.flatMap((event, index) =>
    (event[0] === "keypress" ? [times[index]] : []));
  const cpi = fixture.version === 1 ? fixture.processor.settings.cpi : fixture.source.cpi;
  const inCut = (tUs) => cuts.some(([start, end]) => tUs >= start && tUs < end);
  /* A label boundary or a key press also ends a stroke. */
  const groups = [];
  for (const frame of motionFrames(fixture)) {
    if (inCut(frame.tUs)) {
      groups.push(null);
      continue;
    }
    const current = groups.at(-1);
    const last = current?.at(-1).tUs;
    if (current && frame.tUs - last < STROKE_GAP_US &&
      !keyTimes.some((key) => key > last && key <= frame.tUs)) current.push(frame);
    else groups.push([frame]);
  }
  const afterKey = (frames) => keyTimes.some((key) => key <= frames[0].tUs &&
    frames[0].tUs - key < TYPING_GAP_US);
  /* A stroke ends where the next one starts, and never inside a label. */
  let floorUs = 0;
  return groups.filter(Boolean)
    .flatMap((frames) => (afterKey(frames) ? splitShake(frames) : [frames]))
    .map((frames) => {
      const floor = Math.max(floorUs, 0,
        ...cuts.map(([, end]) => end).filter((end) => end <= frames[0].tUs));
      floorUs = frames.at(-1).tUs + 1;
      return deriveStroke(frames, mode, keyTimes, 25.4 / cpi, floor);
    })
    .filter(Boolean);
}

/* Ground truth for a trace from labels and, with derive, from its own
 * motion elsewhere. Returns the strokes, sorted and numbered, or none. */
function traceTruth(fixture, labelled, derive) {
  const strokes = [...labelled];
  if (derive) {
    strokes.push(...deriveTruthStrokes(fixture,
      labelled.map((label) => [label.startUs, label.endUs])));
  }
  if (strokes.length === 0) return undefined;
  strokes.sort((a, b) => a.startUs - b.startUs);
  const derived = strokes.some((stroke) => stroke.source !== "label");
  return { strokes: strokes.map((stroke, index) => ({ id: `${stroke.source}-${index + 1}`,
    ...stroke })), ...(derived ? { derived: true } : {}) };
}

/* Frames split where they rest for LABEL_REST_US or longer. */
function gestures(frames) {
  const groups = [];
  for (const frame of frames) {
    const last = groups.at(-1);
    if (last && frame.tUs - last.at(-1).tUs < LABEL_REST_US) last.push(frame);
    else groups.push([frame]);
  }
  return groups;
}

/* Labelled strokes of one trace, clamped to it, with the ball path inside
 * them and, for cursor trials, the browser's pointer path around them;
 * scroll and text labels split into a stroke per gesture. A cursor trial
 * the trace doesn't hold whole is dropped: its output would be measured
 * against the whole distance. Only the recording's first trace, which
 * nothing was lost before (fresh), may start inside a trial, since the
 * trial then began at rest. Warnings name labels that don't fit. */
function labelledStrokes(fixture, startMs, mode, labels, pointer, warnings, fresh) {
  const times = eventTimesUs(fixture);
  const endUs = times.at(-1) + 1;
  const frames = motionFrames(fixture);
  /* Device milliseconds to trace microseconds, across a uptime wrap. */
  const traceUs = (ms) => (((Math.floor(ms) - startMs) | 0) + (ms - Math.floor(ms))) * 1000;
  const result = [];
  for (const [index, label] of labels.entries()) {
    const from = ((label.startMs - startMs) | 0) * 1000;
    const to = ((label.endMs - startMs) | 0) * 1000;
    if (to <= 0 || from >= endUs) continue;
    if (label.activity !== mode) {
      warnings.push(`label ${index} (${label.activity}) is ignored on ${fixture.id}, a ${mode} ` +
        "trace");
      label.used = true;
      continue;
    }
    label.used = true;
    const startUs = Math.max(from, 0);
    const stopUs = Math.min(to, endUs);
    if (mode === "cursor" && (stopUs !== to || (startUs !== from && !fresh))) {
      warnings.push(`label ${index} (trial ${label.trial ?? "?"}) is dropped: ${fixture.id}, ` +
        `which runs from ${startMs} ms for ${Math.ceil(endUs / 1000)} ms, holds only part of it`);
      continue;
    }
    if (mode !== "cursor" && (startUs !== from || stopUs !== to)) {
      warnings.push(`label ${index} is clamped to ${fixture.id}, which runs from ` +
        `${startMs} ms for ${Math.ceil(endUs / 1000)} ms`);
    }
    const inside = frames.filter((frame) => frame.tUs >= startUs && frame.tUs < stopUs);
    if (inside.length === 0) {
      warnings.push(`label ${index} covers no motion in ${fixture.id}`);
      continue;
    }
    if (mode !== "cursor") {
      const groups = gestures(inside);
      groups.forEach((group, part) => {
        const from_ = part === 0 ? startUs : group[0].tUs;
        const to_ = part === groups.length - 1 ? stopUs : groups[part + 1][0].tUs;
        result.push({ startUs: from_, endUs: to_, source: "label", activity: mode, intended: true,
          ...(label.test !== undefined ? { test: label.test } : {}), axis: label.axis,
          path: { intervalUs: PATH_INTERVAL_US, points: strokePath(group, from_, to_) } });
      });
      continue;
    }
    const stroke = { startUs, endUs: stopUs, source: "label",
      activity: mode === "cursor" ? "target" : mode, intended: true,
      ...(label.test !== undefined ? { test: label.test } : {}),
      ...(label.trial !== undefined ? { trial: label.trial } : {}),
      target: { x: label.target.x, y: label.target.y, size: label.target.size },
      start: { x: label.start.x, y: label.start.y },
      path: { intervalUs: PATH_INTERVAL_US, points: strokePath(inside, startUs, stopUs) } };
    if (mode === "cursor" && pointer) {
      const samples = pointer.samples.map(([ms, x, y]) => [Math.round(traceUs(ms)), x, y])
        .filter(([tUs]) => tUs >= startUs - POINTER_MARGIN_US && tUs < stopUs + POINTER_MARGIN_US);
      if (samples.length) stroke.pointer = samples;
      else warnings.push(`label ${index} has no browser pointer samples`);
    }
    result.push(stroke);
  }
  return result;
}

/* Fixture v2 traces from a decoded capture and the settings it ran with:
 * one profile, or several when settings changed while recording. A trace
 * whose settings hash no profile matches is skipped with a warning.
 * options.labels lists stretches whose intent a guided test, the user, or
 * an agent named, in device uptime: { startMs, endMs, activity: "scroll" |
 * "text", axis: "horizontal" | "vertical" | "both" (scroll only) }, or for
 * a cursor trial { startMs, endMs, activity: "cursor", target: { x, y,
 * size }, start: { x, y } } in CSS pixels, each optionally with test and
 * trial. options.pointer is the browser's pointer path during a guided
 * cursor test: { samples: [[ms, x, y], ...], acceleration: "off" | "on" |
 * "unknown" }, in device uptime and the labels' pixels; cursor strokes keep
 * the samples around them, and truth.pointerAcceleration the answer.
 * options.truth is "labels-only" (the default: truth only from labels) or
 * "derive" (labels where they apply, and strokes derived from the motion
 * elsewhere). Every stroke's source is "label", "derived", or "typing"
 * (derived as typing shake), and truth.derived is set when any stroke
 * isn't from a label. Returns { fixtures, warnings }. */
export function captureToFixtures(capture, profiles, options = {}) {
  const { description, records } = capture;
  profiles = Array.isArray(profiles) ? profiles : [profiles];
  for (const profile of profiles) {
    validateProfile(profile);
    chainStages(description, profile);
  }
  const labels = (options.labels ?? []).map((label) => ({ ...label }));
  validateLabels(labels);
  validatePointer(options.pointer);
  const truthMode = options.truth ?? "labels-only";
  if (!TRUTH_MODES.includes(truthMode)) {
    throw new Error(`options.truth must be ${TRUTH_MODES.join(" or ")}`);
  }
  const { kind } = CHAIN_MODES[description.stages.at(-1)?.strategy] ?? CURSOR_MODE;
  const mode = pipelineMode(description);
  /* The stages that take a keypress guard share one window. */
  const windowMs = Math.max(0, ...description.stages.map((stage) =>
    stage.fixed["suppress-after-keypress-ms"] ?? 0));
  const warnings = [];
  if (capture.skipped > 0) {
    warnings.push(`${capture.skipped} delta frames had no frame to follow and were skipped`);
  }
  /* A stretch without frames (only flushes or key presses) replays nothing. */
  const allSegments = segments(records, warnings);
  const [firstSegment] = allSegments;
  const parts = allSegments.filter((segment) => segment.source !== null)
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
          /* The recording every trace of this capture shares. */
          recording: id,
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
    const truth = traceTruth(fixture,
      labelledStrokes(fixture, startMs, mode, labels, options.pointer, warnings,
        index === 0 && records[0]?.sequence === 0 && parts[0].segment === firstSegment),
      truthMode === "derive");
    if (truth && mode === "cursor" && options.pointer) {
      truth.pointerAcceleration = options.pointer.acceleration;
    }
    if (truth) fixture.truth = truth;
    return validateFixture(fixture, fixture.id);
  });
  for (const [index, label] of labels.entries()) {
    if (!label.used) warnings.push(`label ${index} covers no trace and is dropped`);
  }
  return { fixtures, warnings };
}
