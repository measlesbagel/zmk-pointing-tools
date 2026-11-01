/* SPDX-License-Identifier: MIT */

/* Checks that input capture is faithful: a capture recorded from the
 * firmware's stages (host/tests/capture_session.c), converted to a fixture
 * v2 trace and replayed with the recorded settings, must send exactly the
 * cursor reports the pipeline sent while recording.
 *
 *   node host/replay/capture-check.js --session build/host/capture_session \
 *     --cursor-pipeline-runner build/host/zpt_cursor_pipeline_replay \
 *     [--buffer 4096] [--credits 2] [--interval-us 7500] [--expect drops|no-drops] \
 *     [--min-peak <bytes>]
 *
 * The session's records go through the firmware's burst buffer and a
 * simulated Bluetooth link, which refills every credit each connection
 * event and doesn't share the connection with HID reports. With --expect no-drops, the buffer must hold
 * every burst, and with --min-peak the flicks must outrun the link by at
 * least that much; with drops, the buffer must fill.
 * Either way every record must arrive or show as a gap, in order, and every
 * trace must replay exactly. */

import { spawnSync } from "node:child_process";
import { parseArgs } from "node:util";

import { captureToFixtures, decodeCaptureStream } from "./capture.js";
import { parseLines } from "./metrics.js";
import { CAPTURE } from "../../web/protocol.js";
import { runFixture } from "./runner.js";

const { values } = parseArgs({
  options: {
    session: { type: "string" },
    "cursor-pipeline-runner": { type: "string" },
    buffer: { type: "string", default: "4096" },
    credits: { type: "string", default: "2" },
    "interval-us": { type: "string", default: "7500" },
    expect: { type: "string", default: "no-drops" },
    "min-peak": { type: "string", default: "0" },
  },
});

const generated = spawnSync(values.session,
  [values.buffer, values.credits, values["interval-us"]], { encoding: "utf8", maxBuffer: 64 << 20 });
if (generated.status !== 0) {
  console.error(`capture session failed: ${generated.stderr}`);
  process.exit(2);
}
const session = JSON.parse(generated.stdout);
const bytes = Uint8Array.from(session.stream.match(/../g), (hex) => parseInt(hex, 16));
const capture = decodeCaptureStream(bytes);
const { fixtures, warnings } = captureToFixtures(capture, session.profiles,
  { id: "capture-check" });

const failures = [];
/* Every record arrives once, in order, or is a counted gap. */
const sequences = capture.records.map((record) => record.sequence);
if (sequences.some((sequence, index) => index > 0 && sequence <= sequences[index - 1])) {
  failures.push("records arrived out of order or twice");
}
if (capture.records.length + session.dropped !== session.records) {
  failures.push(`${session.records} records recorded, ${capture.records.length} arrived and ` +
    `${session.dropped} dropped`);
}
const gaps = sequences.reduce((sum, sequence, index) =>
  sum + (sequence - (index === 0 ? 0 : sequences[index - 1] + 1)), 0) +
  (session.records - 1 - (sequences.at(-1) ?? -1));
if (gaps !== session.dropped) failures.push(`${gaps} records missing, ${session.dropped} dropped`);
/* The session changes settings once and resyncs after it and after any
 * drop, so every stretch replays as its own trace with the profile it ran.
 * Reports are tagged with the sequence of the entered record that started
 * their trace. */
const tags = [...new Set(session.outputs.map(([tag]) => tag))];
const entered = capture.records.filter((record) => record.kind === CAPTURE.RECORD_ENTERED)
  .map((record) => record.sequence);
const tagOf = (fixture) => Math.max(...entered.filter((sequence) =>
  sequence < fixture.metadata.capture.firstSequence));
const traces = fixtures.length;
for (const tag of tags) {
  if (!fixtures.some((fixture) => tagOf(fixture) === tag)) {
    failures.push(`reports of the trace entered at record ${tag} have no trace`);
  }
}
const linkBytesPerMs = (Number(values.credits) * 62) / (Number(values["interval-us"]) / 1000);
if (values.expect === "drops") {
  if (session.dropped === 0) failures.push("the buffer should fill and drop records");
  if (traces < 3) failures.push(`the session should resync after its drops (${traces} traces)`);
} else {
  if (session.dropped !== 0) failures.push(`${session.dropped} records dropped`);
  if (session.peakBuffer < Number(values["min-peak"])) {
    failures.push(`the flicks should outrun the link (peak buffer ${session.peakBuffer} bytes)`);
  }
  if (traces !== 2) failures.push(`expected 2 traces around the tuning change, got ${traces}`);
}
const expectedWarnings = warnings.filter((warning) =>
  !/records dropped before|settings changed while recording/.test(warning));
const profilesUsed = new Set(fixtures.map((fixture) => JSON.stringify(fixture.processor.profile)));
if (profilesUsed.size !== 2) failures.push(`expected traces of both profiles, got ${profilesUsed.size}`);
if (expectedWarnings.length > 0) failures.push(`unexpected warnings: ${expectedWarnings.join("; ")}`);
if (session.injectedKeypresses === 0 || session.lateFlushes === 0) {
  failures.push("the session should land key presses mid-frame and run flushes late");
}
if (!fixtures.slice(1).some((fixture) => fixture.events[0][0] === "keypress")) {
  failures.push("a trace after a resync should start with the key press the guard still holds");
}
const events = fixtures.flatMap((fixture) => fixture.events);
const count = (type) => events.filter((event) => event[0] === type).length;
const keypresses = count("keypress");
const flushes = count("flush");
const late = events.filter((event) => (event[0] === "motion" ? event[5] : event[2]) > 0).length;
if (keypresses < 2 || flushes < 2 || late === 0) {
  failures.push("the session should exercise key presses, flushes, and late events " +
    `(${keypresses}, ${flushes}, ${late})`);
}

/* Replay starts at the millisecond the trace starts at; firmware reports
 * carry uptime, which the trace's startMs maps to replay time. */
let reports = 0;
fixtures.forEach((fixture, trace) => {
  const output = runFixture(fixture, { cursorPipeline: values["cursor-pipeline-runner"] });
  const replayed = parseLines(output, fixture, 9, 4).outputs.map(([time, x, y]) => [time, x, y]);
  const startMs = fixture.metadata.capture.startMs;
  const tag = tagOf(fixture);
  const recorded = session.outputs.filter(([index]) => index === tag)
    .map(([, time, x, y]) => [(time - startMs) >>> 0, x, y]);
  reports += recorded.length;
  if (JSON.stringify(replayed) !== JSON.stringify(recorded)) {
    const index = recorded.findIndex((report, i) =>
      JSON.stringify(report) !== JSON.stringify(replayed[i]));
    failures.push(`trace ${trace + 1}: replay differs from firmware at report ${index}: ` +
      `firmware ${JSON.stringify(recorded[index])}, replay ${JSON.stringify(replayed[index])} ` +
      `(${recorded.length} firmware reports, ${replayed.length} replayed)`);
  }
});

if (reports === 0) failures.push("the session sent no reports");
if (failures.length > 0) {
  for (const failure of failures) console.error(failure);
  process.exit(1);
}
console.log(`buffer ${session.bufferSize} B, link ${linkBytesPerMs.toFixed(1)} B/ms: ` +
  `${(session.frameBytes / session.frames).toFixed(2)} bytes per frame, peak ` +
  `${session.peakBuffer} B buffered`);
console.log(`capture replay matches firmware: ${reports} reports in ${fixtures.length} traces ` +
  `from ${events.length} events (${keypresses} key presses, ${flushes} flushes, ` +
  `${late} late events, ${session.dropped} records dropped)`);
