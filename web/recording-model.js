/* SPDX-License-Identifier: MIT */

/* Recordings in the tuner (#124): what the panel and the agent tools keep
 * of a capture, where scored traces' ground truth comes from, and score
 * comparisons. No DOM and no replay imports, so it runs
 * in tests; recorder.js loads the replay module. */

import { CAPTURE, FrameDecoder, MESSAGE, captureDecoder, parseCaptureBatch } from "./protocol.js";

export const RECORDING_SCHEMA = "zmk-pointing-tools/recording";
export const RECORDING_VERSION = 1;

/* Where a recorded trace's ground-truth strokes come from. The converter
 * (host/replay/capture-convert.js, truth: "derive") owns the rules, in
 * docs/trace-benchmark.md, "Recorded traces: ground truth". */
export const TRUTH_SOURCES = {
  label: "strokes labelled in the recording",
  derived: "strokes the converter derived from the motion",
  typing: "motion right after a key press, counted as unintended",
};

const NO_TRUTH = "no stroke in these recordings has truth for this metric";

/* What a free recording's truth is: the converter's guess from the
 * motion, with no targets. */
export const FREE_TRUTH = "no targets: heuristic";

/* The clock offset after a capture status that carries the device's
 * uptime: the status with the shortest round trip so far places its uptime
 * halfway between the request (sentAt) and the answer (receivedAt), in
 * performance.now() time. A status without uptime (schema 2) changes
 * nothing. */
export function nextClock(clock, uptimeMs, sentAt, receivedAt) {
  const roundTripMs = receivedAt - sentAt;
  if (uptimeMs === null || uptimeMs === undefined || (clock && clock.roundTripMs <= roundTripMs)) return clock;
  return { roundTripMs, offsetMs: uptimeMs - (sentAt + receivedAt) / 2 };
}

/* A guided test's labels and pointer path, kept in this page's
 * performance.now() time while it runs, moved to device uptime with the
 * clock offset for the converter's options.labels and options.pointer. */
export function deviceGuide(guide, clock) {
  const device = (timeMs) => timeMs + clock.offsetMs;
  const labels = guide.labels.map(({ startPerf, endPerf, ...label }) =>
    ({ ...label, test: guide.test, startMs: Math.round(device(startPerf)), endMs: Math.round(device(endPerf)) }))
    .filter(({ startMs, endMs }) => endMs > startMs);
  return {
    test: guide.test,
    clockRoundTripMs: Math.round(clock.roundTripMs * 10) / 10,
    labels,
    ...(guide.pointer ? { pointer: { acceleration: guide.acceleration,
      samples: guide.pointer.map(([timeMs, x, y]) => [Math.round(device(timeMs) * 10) / 10,
        Math.round(x * 10) / 10, Math.round(y * 10) / 10]) } } : {}),
  };
}

/* Source of one truth stroke: a TRUTH_SOURCES key. The converter tags
 * each stroke; anything else counts as derived, never as labelled. */
export function truthSource(stroke) {
  return stroke.source === "label" || stroke.source === "typing" ? stroke.source : "derived";
}

/* For each metric, how many of the strokes scoring it in any set came
 * from each truth source, such as { derived: 12, typing: 3 }. sets are
 * score sets with fixtureResults; traces are the fixtures they scored. */
export function metricTruth(traces, sets) {
  const strokes = new Map(traces.flatMap((trace) =>
    (trace.truth?.strokes ?? []).map((stroke) => [`${trace.id}\n${stroke.id}`, stroke])));
  const seen = {};
  for (const { fixtureResults = [] } of sets) {
    for (const { id, strokes: scored } of fixtureResults) {
      for (const stroke of scored) {
        const truth = strokes.get(`${id}\n${stroke.id}`);
        if (!truth) continue;
        for (const metric of Object.keys(stroke.metrics)) {
          (seen[metric] ??= new Map()).set(`${id}\n${stroke.id}`, truthSource(truth));
        }
      }
    }
  }
  return Object.fromEntries(Object.entries(seen).map(([metric, sources]) => {
    const counts = {};
    for (const source of sources.values()) counts[source] = (counts[source] ?? 0) + 1;
    return [metric, counts];
  }));
}

/* "derived 12, typing heuristic 3" */
export function describeTruth(counts) {
  const names = { label: "labels", derived: "derived", typing: "typing heuristic" };
  const parts = Object.keys(names).filter((source) => counts?.[source]).map((source) => `${names[source]} ${counts[source]}`);
  return parts.length ? parts.join(", ") : NO_TRUTH;
}

export const SCORE_CAVEAT = "Replay shortlists; the keyboard decides. Guided cursor trials judge how consistently and smoothly a gain lands and homes near the gain you recorded with, and flag one that moves your landing too far, since you'd have aimed differently; they can't say which gain is best. Elsewhere the ideal output follows the curve and gain being scored, so a flatter curve or a lower gain can win while feeling worse. Confirm on the keyboard with a preview and a run.";

/* Counts of a recording's records, kept up to date as batches arrive:
 * dropped records from sequence gaps, and resyncs from entered records
 * after the first. */
export function createRecordingStats() {
  return { records: 0, frames: 0, keypresses: 0, dropped: 0, entered: 0, settingsChanges: 0,
    firstMs: null, lastMs: null, nextSequence: 0 };
}

export function addRecords(stats, records) {
  for (const record of records) {
    const gap = (record.sequence - stats.nextSequence) >>> 0;
    if (gap > 0 && gap < 0x80000000) stats.dropped += gap;
    stats.nextSequence = (record.sequence + 1) >>> 0;
    stats.records += 1;
    if (record.kind === CAPTURE.RECORD_FRAME) {
      stats.frames += 1;
      stats.firstMs ??= record.timeMs;
      stats.lastMs = record.timeMs;
    } else if (record.kind === CAPTURE.RECORD_KEYPRESS) stats.keypresses += 1;
    else if (record.kind === CAPTURE.RECORD_ENTERED) stats.entered += 1;
    else if (record.kind === CAPTURE.RECORD_SETTINGS_CHANGED) stats.settingsChanges += 1;
  }
  return stats;
}

export const resyncs = (stats) => Math.max(0, stats.entered - 1);

/* Rows of a score comparison: per metric, each set's mean and stroke
 * count, its change from the first set, and whether that change is better.
 * As in host/replay/bench.js, a metric that vanished or was scored over
 * different strokes is worse, never better: settings must not win by
 * making strokes unscorable. note says why. metrics is quality.js's
 * METRICS; truth is metricTruth's counts. */
export function compareScoreSets(sets, metrics, truth = {}) {
  const round = (value) => Math.round(value * 1000) / 1000;
  return Object.entries(metrics).map(([metric, { higherIsBetter, unit, ranked = true }]) => {
    const values = sets.map(({ scores }) => (Number.isFinite(scores[metric]?.value) ? scores[metric] : null));
    const base = values[0];
    const compared = values.map((entry, index) => {
      /* A guarded score (quality.js, the gain guard) is worse whatever its
       * value; on the current values it is only noted. */
      if (index === 0 || !base) return { change: null, better: null, note: entry?.guard ?? null };
      if (!entry) return { change: null, better: false, note: "missing" };
      const change = round(entry.value - base.value);
      if (entry.guard) return { change, better: false, note: entry.guard };
      if (entry.strokes !== base.strokes) {
        return { change, better: false, note: `strokes ${base.strokes}→${entry.strokes}` };
      }
      /* An unranked metric is reported, never better or worse. */
      return { change, better: change === 0 || !ranked ? null : (change > 0) === higherIsBetter, note: null };
    });
    return {
      metric, unit, higherIsBetter, ranked,
      truth: truth[metric] ?? null,
      values: values.map((entry) => (entry ? { value: round(entry.value), strokes: entry.strokes } : null)),
      change: compared.map(({ change }) => change),
      better: compared.map(({ better }) => better),
      note: compared.map(({ note }) => note),
    };
  });
}

function toBase64(bytes) {
  let text = "";
  for (let i = 0; i < bytes.length; i += 0x8000) text += String.fromCharCode(...bytes.subarray(i, i + 0x8000));
  return btoa(text);
}

function fromBase64(text) {
  const binary = atob(text);
  return Uint8Array.from(binary, (c) => c.charCodeAt(0));
}

/* A recording as a file: the capture stream exactly as received, which
 * host/replay/capture.js reads too, and the profiles it ran with. */
export function exportRecording(recording) {
  const { stream, ...rest } = recording;
  return { schema: RECORDING_SCHEMA, version: RECORDING_VERSION, ...rest, stream: toBase64(stream) };
}

export const newRecordingId = () => `rec-${Date.now().toString(36)}-${Math.random().toString(36).slice(2, 6)}`;

/* Counts of a capture stream's records, as recorded. */
export function streamStats(stream) {
  const stats = createRecordingStats();
  const decoder = captureDecoder();
  for (const frame of new FrameDecoder().push(stream)) {
    if (frame.type !== MESSAGE.CAPTURE_BATCH) continue;
    /* A batch that doesn't parse is skipped, as while recording. */
    try {
      addRecords(stats, parseCaptureBatch(frame.payload, decoder));
    } catch {}
  }
  return stats;
}

const text = (value, limit, fallback) => (typeof value === "string" ? value.slice(0, limit) : fallback);

/* A recording file is user data: keep only the schema's fields, checked,
 * with the counts recomputed from the stream and a fresh id. */
export function importRecording(data) {
  if (data?.schema !== RECORDING_SCHEMA || data.version !== RECORDING_VERSION) {
    throw new Error(`expected a ${RECORDING_SCHEMA} v${RECORDING_VERSION} file`);
  }
  if (typeof data.stream !== "string" || !Array.isArray(data.profiles) || typeof data.pipeline !== "string") {
    throw new Error("the recording has no stream, pipeline or profiles");
  }
  if (!data.profiles.every((profile) => profile && typeof profile === "object" && !Array.isArray(profile))) {
    throw new Error("the recording's profiles aren't tuning profiles");
  }
  let stream;
  try {
    stream = fromBase64(data.stream);
  } catch {
    throw new Error("the recording's stream isn't base64");
  }
  const stats = streamStats(stream);
  const created = typeof data.createdAt === "string" ? new Date(data.createdAt) : null;
  const span = stats.firstMs !== null ? stats.lastMs - stats.firstMs : 0;
  return {
    id: newRecordingId(),
    label: text(data.label, 40, "").replace(/[“”"]/g, "").trim() || "Imported",
    pipeline: data.pipeline.slice(0, 60),
    createdAt: created && !Number.isNaN(created.getTime()) ? created.toISOString() : new Date().toISOString(),
    durationMs: Number.isFinite(data.durationMs) && data.durationMs >= 0 ? data.durationMs : span,
    transport: text(data.transport, 20, null),
    endReason: text(data.endReason, 80, "imported"),
    firmwareDropped: Number.isInteger(data.firmwareDropped) && data.firmwareDropped >= 0 ? data.firmwareDropped : null,
    warnings: (Array.isArray(data.warnings) ? data.warnings : []).filter((warning) => typeof warning === "string")
      .slice(0, 20).map((warning) => warning.slice(0, 300)),
    profiles: data.profiles,
    guided: importGuide(data.guided),
    stats,
    stream,
  };
}

/* A guided recording's labels and pointer path; the converter checks
 * every label when it scores. */
function importGuide(guided) {
  if (guided === null || guided === undefined) return null;
  if (typeof guided?.test !== "string" || !Array.isArray(guided.labels) ||
      (guided.pointer !== undefined && !Array.isArray(guided.pointer?.samples))) {
    throw new Error("the recording's guided test has no labels");
  }
  return {
    test: guided.test.slice(0, 40),
    clockRoundTripMs: Number.isFinite(guided.clockRoundTripMs) ? guided.clockRoundTripMs : null,
    labels: guided.labels.filter((label) => label && typeof label === "object" && !Array.isArray(label)),
    ...(guided.pointer ? { pointer: { acceleration: guided.pointer.acceleration, samples: guided.pointer.samples } } : {}),
  };
}
