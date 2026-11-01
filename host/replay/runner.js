/* SPDX-License-Identifier: MIT */

import { spawnSync } from "node:child_process";
import { readFileSync, writeFileSync } from "node:fs";
import { resolve } from "node:path";

import { encodeRunnerInput, runnerFor, validateFixture } from "./fixture.js";
import {
  compareMetrics,
  parseChainMetrics,
  parseCursorMetrics,
  parseNoiseFilterMetrics,
  parseScrollMetrics,
  parseTextMetrics,
} from "./metrics.js";

export function loadFixture(path) {
  return validateFixture(JSON.parse(readFileSync(path, "utf8")), path);
}

/* Run a fixture through its native runner and return the raw output. An
 * override profile replaces stage-chain settings by stable id. */
export function runFixture(fixture, runners, overrideProfile = null) {
  const result = spawnSync(resolve(runners[runnerFor(fixture)]), [], {
    input: encodeRunnerInput(fixture, overrideProfile),
    encoding: "utf8",
    maxBuffer: 64 * 1024 * 1024,
  });
  if (result.status !== 0) {
    throw new Error(`${fixture.id}: runner failed: ${(result.stderr ?? result.error).trim()}`);
  }
  return result.stdout;
}

export function replayFixture(path, runners, update = false) {
  const fixture = loadFixture(path);
  const output = runFixture(fixture, runners);
  const metrics = fixture.processor.kind === "composed-scroll"
    ? parseScrollMetrics(output, fixture)
    : fixture.processor.kind === "composed-text"
      ? parseTextMetrics(output, fixture)
      : fixture.processor.kind === "cursor-pipeline"
        ? parseCursorMetrics(output, fixture)
        : ["scroll-pipeline", "text-pipeline"].includes(fixture.processor.kind)
          ? parseChainMetrics(output, fixture)
          : parseNoiseFilterMetrics(output, fixture);
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
