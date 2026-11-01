#!/usr/bin/env node
/* SPDX-License-Identifier: MIT */

/* Turns a firmware input capture (docs/protocol.md, "Input capture") into
 * fixture v2 traces of cursor, scroll, or text pipelines that replay
 * through the stage-chain runner.
 *
 *   node host/replay/capture.js --stream capture.bin --profile a.json \
 *     [--profile b.json …] --out trace.json [--id my-trace] [--device right-ball] \
 *     [--provenance "…"] [--labels labels.json] [--truth labels-only|derive]
 *
 * The stream is the device's protocol bytes as received: a capture
 * description (0x8d) and the capture batches (0x92) of one recording; other
 * frames are ignored. Each profile is a tuner export
 * (zmk-pointing-tools/tuning-profile v1) of settings the device ran while
 * recording, such as the two sides of an A/B comparison. A recording
 * becomes one trace per stretch the pipeline ran unchanged, each with the
 * profile whose settings hash its entered record carries; see
 * captureToFixtures in capture-convert.js, which the tuner shares.
 * --labels names the intended axis of stretches of a scroll or text
 * recording: a JSON list of { startMs, endMs, activity, axis } in device
 * uptime. --truth derive also derives strokes for the motion outside labels,
 * with source "derived" or "typing"; the default, labels-only, writes only the labelled ones
 * (docs/trace-benchmark.md, "Recorded traces: ground truth"). */

import { readFileSync, writeFileSync } from "node:fs";
import { resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { parseArgs } from "node:util";

import { captureToFixtures, decodeCaptureStream } from "./capture-convert.js";

export { captureToFixtures, decodeCaptureStream };

function main() {
  const { values } = parseArgs({
    options: {
      stream: { type: "string" },
      profile: { type: "string", multiple: true },
      out: { type: "string" },
      id: { type: "string" },
      device: { type: "string" },
      provenance: { type: "string" },
      labels: { type: "string" },
      truth: { type: "string", default: "labels-only" },
    },
  });
  if (!values.stream || !values.profile || !values.out) {
    console.error("usage: capture.js --stream <bytes> --profile <profile.json> [--profile …] " +
      "--out <trace.json> [--labels <labels.json>] [--truth labels-only|derive]");
    process.exit(2);
  }
  const capture = decodeCaptureStream(new Uint8Array(readFileSync(values.stream)));
  const profiles = values.profile.map((path) => JSON.parse(readFileSync(path, "utf8")));
  const labels = values.labels ? JSON.parse(readFileSync(values.labels, "utf8")) : [];
  const { fixtures, warnings } = captureToFixtures(capture, profiles, { ...values, labels });
  for (const warning of warnings) console.warn(`warning: ${warning}`);
  fixtures.forEach((fixture, index) => {
    const path = fixtures.length === 1 ? values.out
      : values.out.replace(/(\.json)?$/, `-${index + 1}.json`);
    writeFileSync(path, `${JSON.stringify(fixture, null, 2)}\n`);
    console.log(`${path}: ${fixture.events.length} events`);
  });
}

if (process.argv[1] && resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  main();
}
