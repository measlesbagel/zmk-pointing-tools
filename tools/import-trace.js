#!/usr/bin/env node
/* SPDX-License-Identifier: MIT */

import { basename } from "node:path";
import { readFileSync, writeFileSync } from "node:fs";

function usage() {
  console.error("Usage: import-trace.js --input EXPORT --stream DEVICE:STAGE --template FIXTURE --output FIXTURE [--start N] [--count N]");
}

function argumentsFrom(argv) {
  const options = { start: 0 };
  for (let index = 0; index < argv.length; index += 2) {
    const option = argv[index];
    if (!["--input", "--stream", "--template", "--output", "--start", "--count"].includes(option)) {
      throw new Error(`Unknown option: ${option}`);
    }
    options[option.slice(2)] = argv[index + 1];
  }
  for (const required of ["input", "stream", "template", "output"]) {
    if (!options[required]) throw new Error(`Missing --${required}`);
  }
  options.start = Number(options.start);
  options.count = options.count === undefined ? undefined : Number(options.count);
  if (!Number.isSafeInteger(options.start) || options.start < 0 ||
      (options.count !== undefined && (!Number.isSafeInteger(options.count) || options.count < 1))) {
    throw new Error("--start and --count must be positive integer frame ranges");
  }
  return options;
}

try {
  const options = argumentsFrom(process.argv.slice(2));
  const capture = JSON.parse(readFileSync(options.input, "utf8"));
  const fixture = JSON.parse(readFileSync(options.template, "utf8"));
  const stream = capture.streams?.find(({ key }) => key === options.stream);
  if (!stream || !Array.isArray(capture.samples)) throw new Error(`Stream ${options.stream} is absent from the export`);
  const selected = capture.samples
    .filter(({ key, deviceId, stage }) => key === options.stream || `${deviceId}:${stage}` === options.stream)
    .slice(options.start, options.count === undefined ? undefined : options.start + options.count);
  if (selected.length === 0) throw new Error("Selected frame range is empty");

  let previous;
  fixture.events = selected.map((sample) => {
    const delta = previous === undefined ? 0 : (sample.timestamp - previous) >>> 0;
    previous = sample.timestamp;
    return ["motion", delta, sample.x, sample.y];
  });
  fixture.metadata = {
    ...fixture.metadata,
    capture: {
      source: basename(options.input),
      exportedAt: capture.exportedAt,
      streamKey: options.stream,
      streamLabel: stream.label,
      startFrame: options.start,
      frameCount: selected.length,
    },
  };
  delete fixture.expect;
  writeFileSync(options.output, `${JSON.stringify(fixture, null, 2)}\n`);
  console.log(`Imported ${selected.length} ${options.stream} frames into ${options.output}`);
} catch (error) {
  usage();
  console.error(error.message);
  process.exitCode = 2;
}
