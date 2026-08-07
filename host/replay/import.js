#!/usr/bin/env node
/* SPDX-License-Identifier: MIT */

import { readFileSync, writeFileSync } from "node:fs";
import { basename } from "node:path";
import { parseArgs } from "node:util";

import { importCapture } from "./fixture.js";

function positiveInteger(value) {
  const parsed = Number(value);
  if (!Number.isSafeInteger(parsed) || parsed < 0) throw new Error("Expected a positive integer");
  return parsed;
}

try {
  const { values } = parseArgs({
    options: {
      input: { type: "string" },
      stream: { type: "string" },
      template: { type: "string" },
      output: { type: "string" },
      start: { type: "string", default: "0" },
      count: { type: "string" },
    },
  });
  for (const required of ["input", "stream", "template", "output"]) {
    if (!values[required]) throw new Error(`Missing --${required}`);
  }
  const start = positiveInteger(values.start);
  const count = values.count === undefined ? undefined : positiveInteger(values.count);
  if (count === 0) throw new Error("--count must be greater than zero");

  const fixture = importCapture(
    JSON.parse(readFileSync(values.input, "utf8")),
    JSON.parse(readFileSync(values.template, "utf8")),
    { stream: values.stream, start, count, source: basename(values.input) },
  );
  writeFileSync(values.output, `${JSON.stringify(fixture, null, 2)}\n`);
  console.log(`Imported ${fixture.events.length} ${values.stream} frames into ${values.output}`);
} catch (error) {
  console.error(error.message);
  process.exitCode = 2;
}
