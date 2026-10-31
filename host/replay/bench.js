#!/usr/bin/env node
/* SPDX-License-Identifier: MIT */

/*
 * Trace replay benchmark: scores fixture sets against a committed baseline.
 * See docs/trace-benchmark.md. Exits nonzero only when it can't run, never
 * because a score changed.
 */

import { existsSync, readdirSync, readFileSync, writeFileSync } from "node:fs";
import { basename, dirname, join, relative, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { parseArgs } from "node:util";

import { validateProfile } from "./fixture.js";
import { aggregate, METRICS, scoreFixture } from "./quality.js";
import { loadFixture, runFixture } from "./runner.js";

const REPOSITORY = resolve(dirname(fileURLToPath(import.meta.url)), "../..");
const BASELINE_SCHEMA = "zmk-pointing-tools/benchmark-baseline";
const DIGITS = 2;

const round = (value) => Number(value.toFixed(DIGITS));

function runners(buildDirectory) {
  const runner = (name) => {
    const path = join(buildDirectory, `zpt_${name}_pipeline_replay`);
    if (!existsSync(path)) throw new Error(`${path} is missing; build host/ first`);
    return path;
  };
  return {
    noisePipeline: runner("noise"),
    scrollPipeline: runner("scroll"),
    textPipeline: runner("text_nav"),
    cursorPipeline: runner("cursor"),
  };
}

function scoreSet(directory, native, profile, sensorSpans) {
  const paths = readdirSync(directory).filter((name) => name.endsWith(".json")).sort()
    .map((name) => join(directory, name));
  const fixtures = paths.map((path) => {
    const fixture = loadFixture(path);
    if (sensorSpans && fixture.version === 2) fixture.source.span = "sensor";
    const strokes = scoreFixture(fixture, runFixture(fixture, native, profile));
    return { id: fixture.id, path: relative(REPOSITORY, path),
      synthetic: fixture.metadata?.synthetic === true, scores: aggregate(strokes), strokes };
  });
  return { fixtures: fixtures.length, scores: aggregate(fixtures.flatMap((f) => f.strokes)),
    fixtureResults: fixtures };
}

function compare(name, value, baseline) {
  if (baseline === undefined) return { change: "new" };
  const delta = round(value) - round(baseline);
  if (delta === 0) return { delta: 0, change: "same" };
  const better = METRICS[name].higherIsBetter ? delta > 0 : delta < 0;
  return { delta: round(delta), change: better ? "better" : "worse" };
}

function withBaseline(sets, baseline) {
  for (const [set, result] of Object.entries(sets)) {
    for (const [name, score] of Object.entries(result.scores)) {
      Object.assign(score, { baseline: baseline?.sets?.[set]?.[name] },
        compare(name, score.value, baseline?.sets?.[set]?.[name]));
    }
  }
  return sets;
}

function rows(result) {
  return Object.entries(result.scores).map(([name, score]) => {
    const arrow = METRICS[name].higherIsBetter ? "↑" : "↓";
    const change = score.change === "same" ? "=" : score.change === "new" ? "new"
      : `${score.delta > 0 ? "+" : ""}${score.delta.toFixed(DIGITS)} ${score.change}`;
    return [`${name} ${arrow}`, String(score.strokes), score.value.toFixed(DIGITS),
      score.baseline === undefined ? "-" : score.baseline.toFixed(DIGITS), change];
  });
}

const HEADER = ["metric", "strokes", "score", "baseline", "change"];

function textTable(set, result) {
  const table = [HEADER, ...rows(result)];
  const widths = HEADER.map((_, column) => Math.max(...table.map((row) => row[column].length)));
  const line = (row) => row.map((cell, column) => (column === 0
    ? cell.padEnd(widths[column]) : cell.padStart(widths[column]))).join("  ");
  return [`${set} set (${result.fixtures} fixtures)`, line(table[0]),
    widths.map((width) => "-".repeat(width)).join("  "), ...table.slice(1).map(line)].join("\n");
}

function markdownTable(set, result) {
  return [`### ${set} set (${result.fixtures} fixtures)`, "",
    `| ${HEADER.join(" | ")} |`, `| ${HEADER.map((_, i) => (i ? "---:" : "---")).join(" | ")} |`,
    ...rows(result).map((row) => `| ${row.join(" | ")} |`)].join("\n");
}

function main() {
  const { values } = parseArgs({
    options: {
      "build-dir": { type: "string", default: join(REPOSITORY, "build/host") },
      fixtures: { type: "string", default: join(REPOSITORY, "host/bench/fixtures") },
      baseline: { type: "string", default: join(REPOSITORY, "host/bench/baseline.json") },
      profile: { type: "string" },
      "sensor-spans": { type: "boolean", default: false },
      "write-baseline": { type: "boolean", default: false },
      json: { type: "string" },
      markdown: { type: "string" },
    },
  });
  const native = runners(values["build-dir"]);
  const profile = values.profile
    ? validateProfile(JSON.parse(readFileSync(values.profile, "utf8")), values.profile) : null;
  const setNames = readdirSync(values.fixtures, { withFileTypes: true })
    .filter((entry) => entry.isDirectory()).map((entry) => entry.name)
    .sort((a, b) => (a === "tune" ? -1 : b === "tune" ? 1 : a.localeCompare(b)));
  if (setNames.length === 0) throw new Error(`${values.fixtures} has no fixture sets`);

  const started = Date.now();
  const sets = Object.fromEntries(setNames.map((set) => [set,
    scoreSet(join(values.fixtures, set), native, profile, values["sensor-spans"])]));
  const baseline = existsSync(values.baseline)
    ? JSON.parse(readFileSync(values.baseline, "utf8")) : null;
  withBaseline(sets, baseline);

  const options = [values.profile && `profile ${basename(values.profile)}`,
    values["sensor-spans"] && "sensor spans"].filter(Boolean);
  console.log(`Trace benchmark${options.length ? ` (${options.join(", ")})` : ""}, ` +
    `${Date.now() - started} ms\n`);
  console.log(setNames.map((set) => textTable(set, sets[set])).join("\n\n"));
  if (sets.holdout) {
    console.log("\nThe holdout set checks generalisation; don't tune against it.");
  }

  if (values.json) {
    writeFileSync(values.json, `${JSON.stringify({ schema: "zmk-pointing-tools/benchmark-report",
      version: 1, profile: values.profile ?? null, sensorSpans: values["sensor-spans"], sets },
    null, 2)}\n`);
  }
  if (values.markdown) {
    writeFileSync(values.markdown, `## Trace benchmark\n\n${setNames
      .map((set) => markdownTable(set, sets[set])).join("\n\n")}\n\n` +
      "Scores are reported, not enforced. ↓ lower is better, ↑ higher is better. " +
      "Fixtures are synthetic until real recordings land; see docs/trace-benchmark.md.\n");
  }
  if (values["write-baseline"]) {
    writeFileSync(values.baseline, `${JSON.stringify({ schema: BASELINE_SCHEMA, version: 1,
      sets: Object.fromEntries(setNames.map((set) => [set, Object.fromEntries(
        Object.entries(sets[set].scores).map(([name, score]) => [name, round(score.value)]))])),
    }, null, 2)}\n`);
    console.log(`\nWrote ${relative(process.cwd(), values.baseline)}`);
  }
}

try {
  main();
} catch (error) {
  console.error(error.message);
  process.exitCode = 2;
}
