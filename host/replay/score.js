/* SPDX-License-Identifier: MIT */

/*
 * Scores traces the way the benchmark does (docs/trace-benchmark.md).
 * bench.js scores its fixture sets with the native runners through
 * scoreFixtures; the tuner scores recorded traces with the WebAssembly
 * runners through scoreTraces, in a worker (score-worker.js). Both report the
 * same shape. No Node imports: the tuner loads this file as is.
 */

import { validateFixture, validateProfile } from "./fixture.js";
import { aggregate, scoreFixture } from "./quality.js";
import { runFixtureWasm } from "./wasm.js";

/* Score fixtures replayed by run(fixture, profile), which returns the raw
 * runner output. Returns one benchmark set: the fixture count, the scores
 * over every stroke, and each fixture's scores and strokes. */
export function scoreFixtures(fixtures, run, profile = null) {
  const fixtureResults = fixtures.map((fixture) => {
    const strokes = scoreFixture(fixture, run(fixture, profile), profile);
    return { id: fixture.id, synthetic: fixture.metadata?.synthetic === true,
      scores: aggregate(strokes), strokes };
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
  return scoreFixtures(fixtures, (fixture) => runFixtureWasm(fixture, runners, override),
    override);
}
