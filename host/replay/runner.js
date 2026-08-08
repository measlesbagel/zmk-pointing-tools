/* SPDX-License-Identifier: MIT */

import { spawnSync } from "node:child_process";
import { readFileSync, writeFileSync } from "node:fs";
import { resolve } from "node:path";

import { encodeRunnerInput, validateFixture } from "./fixture.js";
import {
  compareMetrics,
  parseNoiseFilterMetrics,
  parseScrollMetrics,
  parseTextMetrics,
} from "./metrics.js";

export function replayFixture(path, runners, update = false) {
  const fixture = validateFixture(JSON.parse(readFileSync(path, "utf8")), path);
  const runner = fixture.processor.kind === "adaptive-scroll"
    ? runners.scroll
    : fixture.processor.kind === "text-navigation" ? runners.text : runners.noise;
  const result = spawnSync(resolve(runner), [], {
    input: encodeRunnerInput(fixture),
    encoding: "utf8",
  });
  if (result.status !== 0) {
    throw new Error(`${fixture.id}: runner failed: ${result.stderr.trim()}`);
  }

  const metrics = fixture.processor.kind === "adaptive-scroll"
    ? parseScrollMetrics(result.stdout, fixture)
    : fixture.processor.kind === "text-navigation"
      ? parseTextMetrics(result.stdout, fixture)
      : parseNoiseFilterMetrics(result.stdout, fixture);
  if (update) {
    fixture.expect = metrics;
    writeFileSync(path, `${JSON.stringify(fixture, null, 2)}\n`);
  }
  return {
    id: fixture.id,
    path,
    metadata: fixture.metadata,
    metrics,
    differences: compareMetrics(fixture.expect, metrics),
  };
}
