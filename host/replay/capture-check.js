/* SPDX-License-Identifier: MIT */

/* Checks that input capture is faithful: a capture recorded from the
 * firmware's stages (host/tests/capture_session.c), converted to a fixture
 * v2 trace and replayed with the recorded settings, must send exactly the
 * cursor reports the pipeline sent while recording.
 *
 *   node host/replay/capture-check.js --session build/host/capture_session \
 *     --cursor-pipeline-runner build/host/zpt_cursor_pipeline_replay */

import { spawnSync } from "node:child_process";
import { parseArgs } from "node:util";

import { captureToFixtures, decodeCaptureStream } from "./capture.js";
import { parseLines } from "./metrics.js";
import { runFixture } from "./runner.js";

const { values } = parseArgs({
  options: {
    session: { type: "string" },
    "cursor-pipeline-runner": { type: "string" },
  },
});

const generated = spawnSync(values.session, [], { encoding: "utf8", maxBuffer: 64 << 20 });
if (generated.status !== 0) {
  console.error(`capture session failed: ${generated.stderr}`);
  process.exit(2);
}
const session = JSON.parse(generated.stdout);
const bytes = Uint8Array.from(session.stream.match(/../g), (hex) => parseInt(hex, 16));
const { fixtures, warnings } = captureToFixtures(decodeCaptureStream(bytes), session.profile,
  { id: "capture-check" });

const failures = [];
/* The session drops records once, so the recording replays as two traces:
 * the one before the drop, and the one the tap resynced at a rest. */
const traces = Math.max(...session.outputs.map(([trace]) => trace)) + 1;
if (session.dropped === 0) failures.push("the session should drop records");
if (traces < 2) failures.push(`the session should resync after its drop (${traces} traces)`);
if (fixtures.length !== traces) failures.push(`expected ${traces} traces, got ${fixtures.length}`);
const expectedWarnings = warnings.filter((warning) => !/records dropped before/.test(warning));
if (expectedWarnings.length > 0) failures.push(`unexpected warnings: ${expectedWarnings.join("; ")}`);
if (session.injectedKeypresses === 0 || session.lateFlushes === 0) {
  failures.push("the session should land key presses mid-frame and run flushes late");
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
  const recorded = session.outputs.filter(([index]) => index === trace)
    .map(([, time, x, y]) => [(time - startMs) >>> 0, x, y]);
  reports += recorded.length;
  if (recorded.length === 0) failures.push(`trace ${trace + 1}: the session sent no reports`);
  if (JSON.stringify(replayed) !== JSON.stringify(recorded)) {
    const index = recorded.findIndex((report, i) =>
      JSON.stringify(report) !== JSON.stringify(replayed[i]));
    failures.push(`trace ${trace + 1}: replay differs from firmware at report ${index}: ` +
      `firmware ${JSON.stringify(recorded[index])}, replay ${JSON.stringify(replayed[index])} ` +
      `(${recorded.length} firmware reports, ${replayed.length} replayed)`);
  }
});

if (failures.length > 0) {
  for (const failure of failures) console.error(failure);
  process.exit(1);
}
console.log(`capture replay matches firmware: ${reports} reports in ${fixtures.length} traces ` +
  `from ${events.length} events (${keypresses} key presses, ${flushes} flushes, ` +
  `${late} late events, ${session.dropped} records dropped)`);
