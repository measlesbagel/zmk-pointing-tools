/* SPDX-License-Identifier: MIT */

/*
 * Scores traces the way the benchmark does (docs/trace-benchmark.md).
 * bench.js scores its fixture sets with the native runners through
 * scoreFixtures; the tuner scores recorded traces with the WebAssembly
 * runners through scoreTraces, in a worker (score-worker.js). Both report the
 * same shape. No Node imports: the tuner loads this file as is.
 */

import { validateFixture, validateProfile } from "./fixture.js";
import { aggregate, fitScreenScale, scoreFixture } from "./quality.js";
import { runFixtureWasm } from "./wasm.js";

/* Score fixtures replayed by run(fixture, profile), which returns the raw
 * runner output. Returns one benchmark set: the fixture count, the scores
 * over every stroke, and each fixture's scores and strokes. A guided cursor
 * recording also replays with its recorded settings, whatever the profile,
 * to fit one screen scale over all its traces (fitScreenScale), which each
 * of their results carries. */
export function scoreFixtures(fixtures, run, profile = null) {
  const outputs = fixtures.map((fixture) => run(fixture, profile));
  const recordings = new Map();
  fixtures.forEach((fixture, index) => {
    if (!fixture.truth?.strokes?.some((stroke) => stroke.pointer)) return;
    const key = fixture.metadata?.capture?.recording ?? fixture.id;
    if (!recordings.has(key)) recordings.set(key, []);
    recordings.get(key).push({ fixture,
      output: profile === null ? outputs[index] : run(fixture, null) });
  });
  const screens = new Map([...recordings].map(([key, traces]) => [key, fitScreenScale(traces)]));
  const fixtureResults = fixtures.map((fixture, index) => {
    const output = outputs[index];
    const screen = screens.get(fixture.metadata?.capture?.recording ?? fixture.id) ?? null;
    const strokes = scoreFixture(fixture, output, profile, screen);
    return { id: fixture.id, synthetic: fixture.metadata?.synthetic === true,
      scores: aggregate(strokes), strokes, ...(screen ? { screen } : {}) };
  });
  return { fixtures: fixtureResults.length,
    scores: aggregate(fixtureResults.flatMap((result) => result.strokes)), fixtureResults };
}

/* Score fixture v1 or v2 traces, such as tuner recordings, through the
 * compiled runners (compileWasmRunners). profile is a tuning profile
 * (zmk-pointing-tools/tuning-profile v1) whose values replace the traces'
 * recorded cursor stage settings by stable id, or null to score the
 * recorded settings. Traces and profile are validated first: recordings
 * are user data. */
export function scoreTraces(traces, profile, runners) {
  const override = profile ?? null;
  if (!Array.isArray(traces) || traces.length === 0) {
    throw new Error("scoreTraces needs at least one trace");
  }
  const fixtures = traces.map((trace, index) =>
    validateFixture(trace, `trace ${trace?.id ?? index}`));
  if (override !== null) validateProfile(override);
  return scoreFixtures(fixtures, (fixture, replayed) => runFixtureWasm(fixture, runners, replayed),
    override);
}
