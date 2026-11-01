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
if (fixtures.length !== 1) failures.push(`expected one trace, got ${fixtures.length}`);
if (warnings.length > 0) failures.push(`unexpected warnings: ${warnings.join("; ")}`);
const fixture = fixtures[0];
const keypresses = fixture.events.filter((event) => event[0] === "keypress").length;
const flushes = fixture.events.filter((event) => event[0] === "flush").length;
if (keypresses < 2 || flushes < 2) {
  failures.push(`the session should exercise key presses and flushes (${keypresses}, ${flushes})`);
}

/* Replay starts at the millisecond the trace starts at; firmware reports
 * carry uptime. */
const output = runFixture(fixture, { cursorPipeline: values["cursor-pipeline-runner"] });
const { outputs } = parseLines(output, fixture, 9, 4);
const replayed = outputs.map(([time, x, y]) => [time, x, y]);
const offset = session.outputs.length > 0 && replayed.length > 0
  ? session.outputs[0][0] - replayed[0][0] : 0;
const recorded = session.outputs.map(([time, x, y]) => [time - offset, x, y]);
if (recorded.length === 0) failures.push("the session sent no reports");
if (JSON.stringify(replayed) !== JSON.stringify(recorded)) {
  const index = recorded.findIndex((report, i) =>
    JSON.stringify(report) !== JSON.stringify(replayed[i]));
  failures.push(`replay differs from firmware at report ${index}: firmware ` +
    `${JSON.stringify(recorded[index])}, replay ${JSON.stringify(replayed[index])} ` +
    `(${recorded.length} firmware reports, ${replayed.length} replayed)`);
}

if (failures.length > 0) {
  for (const failure of failures) console.error(failure);
  process.exit(1);
}
console.log(`capture replay matches firmware: ${recorded.length} reports from ` +
  `${fixture.events.length} events (${keypresses} key presses, ${flushes} flushes)`);
