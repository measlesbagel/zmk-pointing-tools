/* SPDX-License-Identifier: MIT */

import { spawnSync } from "node:child_process";
import { readFileSync, writeFileSync } from "node:fs";
import { resolve } from "node:path";

import { encodeRunnerInput, validateFixture } from "./fixture.js";
import {
  compareMetrics,
  parseCursorMetrics,
  parseNoiseFilterMetrics,
  parseScrollMetrics,
  parseTextMetrics,
} from "./metrics.js";

export function replayFixture(path, runners, update = false) {
  const fixture = validateFixture(JSON.parse(readFileSync(path, "utf8")), path);
  const runner = fixture.processor.kind === "composed-scroll" ? runners.scrollPipeline
    : fixture.processor.kind === "composed-text" ? runners.textPipeline
    : fixture.processor.kind === "cursor-pipeline" ? runners.cursorPipeline : runners.noisePipeline;
  const result = spawnSync(resolve(runner), [], {
    input: encodeRunnerInput(fixture),
    encoding: "utf8",
  });
  if (result.status !== 0) {
    throw new Error(`${fixture.id}: runner failed: ${result.stderr.trim()}`);
  }

  const metrics = fixture.processor.kind === "composed-scroll"
    ? parseScrollMetrics(result.stdout, fixture)
    : fixture.processor.kind === "composed-text"
      ? parseTextMetrics(result.stdout, fixture)
      : fixture.processor.kind === "cursor-pipeline"
        ? parseCursorMetrics(result.stdout, fixture)
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
