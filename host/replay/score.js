/* SPDX-License-Identifier: MIT */

/*
 * Scores traces the way the benchmark does (docs/trace-benchmark.md).
 * bench.js scores its fixture sets with the native runners through
 * scoreFixtures.
 */

import { aggregate, scoreFixture } from "./quality.js";

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
