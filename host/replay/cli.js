#!/usr/bin/env node
/* SPDX-License-Identifier: MIT */

import { writeFileSync } from "node:fs";
import { parseArgs } from "node:util";

import { replayFixture } from "./runner.js";

function main() {
  const { values, positionals: fixtures } = parseArgs({
    allowPositionals: true,
    options: {
      "scroll-runner": { type: "string" },
      "scroll-pipeline-runner": { type: "string" },
      "cursor-pipeline-runner": { type: "string" },
      "text-pipeline-runner": { type: "string" },
      "text-runner": { type: "string" },
      "noise-runner": { type: "string" },
      update: { type: "boolean", default: false },
      json: { type: "string" },
    },
  });
  if (!values["scroll-runner"] || !values["text-runner"] || !values["noise-runner"] ||
      !values["scroll-pipeline-runner"] || !values["text-pipeline-runner"] ||
      !values["cursor-pipeline-runner"] ||
      fixtures.length === 0) {
    throw new Error("All processor runners and at least one fixture are required");
  }

  const runners = {
    noise: values["noise-runner"],
    scroll: values["scroll-runner"],
    text: values["text-runner"],
    scrollPipeline: values["scroll-pipeline-runner"],
    textPipeline: values["text-pipeline-runner"],
    cursorPipeline: values["cursor-pipeline-runner"],
  };
  const reports = fixtures.map((fixture) => replayFixture(fixture, runners, values.update));
  for (const report of reports) {
    if (report.differences.length) {
      console.error(`FAIL ${report.id}`);
      for (const difference of report.differences) console.error(`  ${difference}`);
    } else {
      const detail = report.metrics.horizontal
        ? `H=${report.metrics.horizontal.signedDistance}, V=${report.metrics.vertical.signedDistance}`
        : `${report.metrics.steps.total} navigation steps`;
      console.log(
        `PASS ${report.id}: ${report.metrics.inputFrames} input, ` +
        `${report.metrics.outputFrames} output, ${detail}`,
      );
    }
  }
  if (values.json) {
    writeFileSync(values.json, `${JSON.stringify({
      schema: "zmk-pointing-tools/replay-report",
      version: 1,
      reports,
    }, null, 2)}\n`);
  }
  if (reports.some((report) => report.differences.length)) process.exitCode = 1;
}

try {
  main();
} catch (error) {
  console.error(error.message);
  process.exitCode = 2;
}
