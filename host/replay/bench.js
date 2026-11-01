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
import { METRICS } from "./quality.js";
import { loadFixture, runFixture } from "./runner.js";
import { scoreFixtures } from "./score.js";

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

function loadSet(directory, sensorSpans) {
  return readdirSync(directory).filter((name) => name.endsWith(".json")).sort().map((name) => {
    const path = join(directory, name);
    const fixture = loadFixture(path);
    if (sensorSpans && fixture.version === 2) fixture.source.span = "sensor";
    return { path, fixture };
  });
}

function scoreSet(loaded, native, profile) {
  const result = scoreFixtures(loaded.map(({ fixture }) => fixture),
    (fixture) => runFixture(fixture, native, profile), profile);
  result.fixtureResults = result.fixtureResults.map(({ id, ...scores }, index) =>
    ({ id, path: relative(REPOSITORY, loaded[index].path), ...scores }));
  return result;
}

/* Tuning keys only scroll and text stages publish. */
const SCROLL_TEXT_KEYS = new Set(["steps-per-meter", "report-interval", "engage-ratio",
  "release-ratio", "window", "discard-unclassified", "fold-interval", "horizontal-threshold",
  "vertical-threshold"]);

/* A profile target that matches no cursor stage would change nothing and
 * leave every score at the baseline. Scroll and text targets are expected
 * in a full tuner export and only warned about; other unmatched targets
 * are warned about too, unless none of the profile's cursor targets
 * matches anything, which is an error. */
function checkProfile(profile, fixtures, path) {
  const stableIds = new Set(fixtures.filter(({ fixture }) => fixture.version === 2 &&
    fixture.processor.kind === "cursor-pipeline")
    .flatMap(({ fixture }) => fixture.processor.stages.map((stage) => stage.stableId)));
  const unmatched = profile.targets.filter((target) => !stableIds.has(target.stableId));
  const scrollText = unmatched.filter((target) =>
    target.parameters.some(({ key }) => SCROLL_TEXT_KEYS.has(key)));
  const cursorTargets = profile.targets.filter((target) => !scrollText.includes(target));
  const unmatchedCursor = unmatched.filter((target) => !scrollText.includes(target));
  if (cursorTargets.length > 0 && unmatchedCursor.length === cursorTargets.length) {
    throw new Error(`${path}: no target matches a cursor stage in any fixture ` +
      `(${unmatchedCursor.map((target) => target.stableId).join(", ")}); known stages: ` +
      `${[...stableIds].sort().join(", ")}`);
  }
  if (cursorTargets.length === 0) {
    throw new Error(`${path}: the profile has no cursor targets; --profile only changes ` +
      "cursor fixtures");
  }
  const shaping = profile.targets.filter((target) => !scrollText.includes(target) &&
    target.parameters.some(({ key }) => /^(curve-|scale-)/.test(key)));
  if (shaping.length) {
    console.warn(`note: ${path} sets curve or gain keys; the benchmark scores how faithfully ` +
      "the pipeline follows its curve and gain, not whether they feel right. Check curve " +
      "changes on the keyboard (docs/trace-benchmark.md).");
  }
  for (const target of unmatchedCursor) {
    console.warn(`warning: ${path}: target ${target.stableId} matches no stage in any ` +
      "fixture and is ignored");
  }
  if (scrollText.length) {
    console.warn(`warning: ${path}: scroll and text targets are ignored ` +
      `(${scrollText.map((target) => target.stableId).join(", ")}); --profile only changes ` +
      "cursor fixtures");
  }
  const others = fixtures.filter(({ fixture }) => fixture.processor.kind !== "cursor-pipeline");
  if (others.length) {
    console.warn(`note: --profile doesn't change the ${others.length} scroll and text fixtures`);
  }
}

/* Baselines hold { value, strokes } per metric. A metric that lost
 * strokes or vanished is worse: a setting must not improve a score by
 * making strokes unscorable. */
function compare(name, score, baseline) {
  if (baseline === undefined) return { change: "new" };
  if (score.value === undefined) return { change: "worse", note: "missing" };
  if (baseline.strokes !== undefined && score.strokes !== baseline.strokes) {
    return { delta: round(round(score.value) - baseline.value), change: "worse",
      note: `strokes ${baseline.strokes}→${score.strokes}` };
  }
  const delta = round(round(score.value) - baseline.value);
  if (delta === 0) return { delta: 0, change: "same" };
  const better = METRICS[name].higherIsBetter ? delta > 0 : delta < 0;
  return { delta, change: better ? "better" : "worse" };
}

function withBaseline(sets, baseline) {
  for (const [set, result] of Object.entries(sets)) {
    /* Version 1 baselines hold bare values without stroke counts. */
    const previous = Object.fromEntries(Object.entries(baseline?.sets?.[set] ?? {})
      .map(([name, entry]) => [name, typeof entry === "number" ? { value: entry } : entry]));
    const scores = {};
    for (const name of Object.keys(METRICS)) {
      if (!(name in result.scores) && !(name in previous)) continue;
      const score = result.scores[name] ?? { value: undefined, strokes: 0 };
      scores[name] = Object.assign(score, { baseline: previous[name]?.value },
        compare(name, score, previous[name]));
    }
    result.scores = scores;
  }
  return sets;
}

function rows(result) {
  return Object.entries(result.scores).map(([name, score]) => {
    const arrow = METRICS[name].higherIsBetter ? "↑" : "↓";
    const change = score.change === "same" ? "=" : score.change === "new" ? "new"
      : [score.delta === undefined ? null
        : `${score.delta > 0 ? "+" : ""}${score.delta.toFixed(DIGITS)}`,
      score.change, score.note && `(${score.note})`].filter(Boolean).join(" ");
    return [`${name} ${arrow}`, String(score.strokes),
      score.value === undefined ? "-" : score.value.toFixed(DIGITS),
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

  const loaded = Object.fromEntries(setNames.map((set) => [set,
    loadSet(join(values.fixtures, set), values["sensor-spans"])]));
  if (profile) checkProfile(profile, Object.values(loaded).flat(), values.profile);

  const started = Date.now();
  const sets = Object.fromEntries(setNames.map((set) => [set,
    scoreSet(loaded[set], native, profile)]));
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
    writeFileSync(values.baseline, `${JSON.stringify({ schema: BASELINE_SCHEMA, version: 2,
      sets: Object.fromEntries(setNames.map((set) => [set, Object.fromEntries(
        Object.entries(sets[set].scores).filter(([, score]) => score.value !== undefined)
          .map(([name, score]) => [name, { value: round(score.value), strokes: score.strokes }]),
      )])),
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
