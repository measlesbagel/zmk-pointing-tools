#!/usr/bin/env node
/* SPDX-License-Identifier: MIT */

import { spawnSync } from "node:child_process";
import { readFileSync, writeFileSync } from "node:fs";
import { resolve } from "node:path";

const SCHEMA = "zmk-pointing-tools/trace-fixture";
const VERSION = 1;
const POLICIES = { free: 0, adaptive: 1, horizontal: 2, vertical: 3 };
const INTENTS = ["undecided", "free", "horizontal", "vertical"];

function usage() {
  console.error("Usage: replay-traces.js --scroll-runner PATH --text-runner PATH [--update] [--json PATH] FIXTURE...");
}

function parseArguments(argv) {
  const result = { fixtures: [], update: false };
  for (let index = 0; index < argv.length; index += 1) {
    const argument = argv[index];
    if (argument === "--update") result.update = true;
    else if (argument === "--scroll-runner") result.scrollRunner = argv[++index];
    else if (argument === "--text-runner") result.textRunner = argv[++index];
    else if (argument === "--json") result.json = argv[++index];
    else if (argument.startsWith("-")) throw new Error(`Unknown option: ${argument}`);
    else result.fixtures.push(argument);
  }
  if (!result.scrollRunner || !result.textRunner || result.fixtures.length === 0) throw new Error("Missing runner or fixture");
  return result;
}

function requireInteger(value, name, minimum = Number.MIN_SAFE_INTEGER) {
  if (!Number.isSafeInteger(value) || value < minimum) throw new Error(`${name} must be an integer >= ${minimum}`);
  return value;
}

function validateFixture(fixture, path) {
  if (fixture.schema !== SCHEMA || fixture.version !== VERSION) {
    throw new Error(`${path}: expected ${SCHEMA} v${VERSION}`);
  }
  if (!fixture.id || !["adaptive-scroll", "text-navigation"].includes(fixture.processor?.kind)) {
    throw new Error(`${path}: id and a supported processor are required`);
  }
  const settings = fixture.processor.settings;
  if (fixture.processor.kind === "adaptive-scroll") {
    for (const key of ["scaleMultiplier", "scaleDivisor", "reportIntervalMs", "idleTimeoutMs",
      "suppressAfterKeypressMs", "engageRatioPercent", "releaseRatioPercent",
      "activationDistance", "intentWindowMs"]) {
      requireInteger(settings?.[key], `${path}: settings.${key}`, key === "suppressAfterKeypressMs" ? 0 : 1);
    }
    if (typeof settings.discardUnclassified !== "boolean") throw new Error(`${path}: discardUnclassified must be boolean`);
    if (!(fixture.processor.policy in POLICIES)) throw new Error(`${path}: unknown axis policy`);
  } else {
    for (const key of ["horizontalThreshold", "verticalThreshold", "idleTimeoutMs",
      "activationDistance", "engageRatioPercent"]) {
      requireInteger(settings?.[key], `${path}: settings.${key}`, 1);
    }
  }
  if (!Array.isArray(fixture.events) || fixture.events.length === 0) throw new Error(`${path}: events are required`);
  for (const [index, event] of fixture.events.entries()) {
    if (!Array.isArray(event) || !["motion", "keypress"].includes(event[0])) throw new Error(`${path}: invalid event ${index}`);
    requireInteger(event[1], `${path}: event ${index} delta`, 0);
    if (event[0] === "motion") {
      if (event.length !== 4) throw new Error(`${path}: motion event ${index} needs delta, x, and y`);
      requireInteger(event[2], `${path}: event ${index} x`);
      requireInteger(event[3], `${path}: event ${index} y`);
    } else if (event.length !== 2) throw new Error(`${path}: keypress event ${index} only accepts delta`);
    if (fixture.processor.kind === "text-navigation" && event[0] !== "motion") {
      throw new Error(`${path}: text navigation fixtures only support motion events`);
    }
  }
}

function runnerInput(fixture) {
  const settings = fixture.processor.settings;
  const configuration = fixture.processor.kind === "adaptive-scroll"
    ? ["C", settings.scaleMultiplier, settings.scaleDivisor, settings.reportIntervalMs,
      settings.idleTimeoutMs, settings.suppressAfterKeypressMs, Number(settings.discardUnclassified),
      settings.engageRatioPercent, settings.releaseRatioPercent, settings.activationDistance,
      settings.intentWindowMs, POLICIES[fixture.processor.policy]]
    : ["C", settings.horizontalThreshold, settings.verticalThreshold, settings.idleTimeoutMs,
      settings.activationDistance, settings.engageRatioPercent];
  const lines = [configuration.join(" ")];
  let timestamp = 0;
  for (const event of fixture.events) {
    timestamp += event[1];
    lines.push(event[0] === "motion" ? `M ${timestamp} ${event[2]} ${event[3]}` : `K ${timestamp}`);
  }
  return `${lines.join("\n")}\n`;
}

function axisMetrics(outputs, index) {
  const values = outputs.map((output) => output[index]);
  const nonzero = values.filter((value) => value !== 0);
  let directionChanges = 0;
  for (let position = 1; position < nonzero.length; position += 1) {
    if (Math.sign(nonzero[position]) !== Math.sign(nonzero[position - 1])) directionChanges += 1;
  }
  return {
    signedDistance: values.reduce((total, value) => total + value, 0),
    absoluteDistance: values.reduce((total, value) => total + Math.abs(value), 0),
    nonzeroFrames: nonzero.length,
    directionChanges,
    clippedFrames: values.filter((value) => value === 32767 || value === -32768).length,
  };
}

function parseScrollOutput(output, fixture) {
  const decisions = [];
  const outputs = [];
  for (const line of output.trim().split("\n")) {
    if (!line) continue;
    const fields = line.split("\t");
    const values = fields.slice(1).map(Number);
    if (fields[0] === "D" && values.length === 12) decisions.push(values);
    else if (fields[0] === "O" && values.length === 3) outputs.push(values);
    else throw new Error(`${fixture.id}: malformed runner output: ${line}`);
  }

  const intentFrames = Object.fromEntries(INTENTS.map((intent) => [intent, 0]));
  let intentTransitions = 0;
  for (let index = 0; index < decisions.length; index += 1) {
    intentFrames[INTENTS[decisions[index][1]]] += 1;
    if (index > 0 && decisions[index][1] !== decisions[index - 1][1]) intentTransitions += 1;
  }
  const intervals = outputs.slice(1).map((output, index) => output[0] - outputs[index][0]);
  const durationMs = fixture.events.reduce((total, event) => total + event[1], 0);
  return {
    inputFrames: decisions.length,
    durationMs,
    outputFrames: outputs.length,
    outputCadence: {
      meanIntervalMs: intervals.length ? Number((intervals.reduce((a, b) => a + b, 0) / intervals.length).toFixed(3)) : 0,
      maximumIntervalMs: intervals.length ? Math.max(...intervals) : 0,
    },
    horizontal: axisMetrics(outputs, 1),
    vertical: axisMetrics(outputs, 2),
    intentFrames,
    intentTransitions,
    idleResets: decisions.filter((decision) => decision[10] !== 0).length,
    suppressedFrames: decisions.filter((decision) => decision[11] !== 0).length,
  };
}

function parseTextOutput(output, fixture) {
  const decisions = [];
  const outputs = [];
  for (const line of output.trim().split("\n")) {
    if (!line) continue;
    const fields = line.split("\t");
    const values = fields.slice(1).map(Number);
    if (fields[0] === "D" && values.length === 5) decisions.push(values);
    else if (fields[0] === "O" && values.length === 2) outputs.push(values);
    else throw new Error(`${fixture.id}: malformed runner output: ${line}`);
  }
  const intentFrames = Object.fromEntries(INTENTS.map((intent) => [intent, 0]));
  for (const decision of decisions) intentFrames[INTENTS[decision[1]]] += 1;
  const stepNames = ["left", "right", "up", "down"];
  const steps = Object.fromEntries(stepNames.map((name) => [name, 0]));
  for (const output of outputs) steps[stepNames[output[1]]] += 1;
  steps.total = outputs.length;
  const intervals = outputs.slice(1).map((output, index) => output[0] - outputs[index][0]);
  return {
    inputFrames: decisions.length,
    durationMs: fixture.events.reduce((total, event) => total + event[1], 0),
    outputFrames: outputs.length,
    outputCadence: {
      meanIntervalMs: intervals.length ? Number((intervals.reduce((a, b) => a + b, 0) / intervals.length).toFixed(3)) : 0,
      maximumIntervalMs: intervals.length ? Math.max(...intervals) : 0,
    },
    steps,
    intentFrames,
    intentTransitions: decisions.slice(1).filter((decision, index) => decision[1] !== decisions[index][1]).length,
  };
}

function compareSubset(expected, actual, prefix = "") {
  const differences = [];
  for (const [key, value] of Object.entries(expected ?? {})) {
    const path = prefix ? `${prefix}.${key}` : key;
    if (value && typeof value === "object" && !Array.isArray(value)) differences.push(...compareSubset(value, actual?.[key], path));
    else if (actual?.[key] !== value) differences.push(`${path}: expected ${JSON.stringify(value)}, got ${JSON.stringify(actual?.[key])}`);
  }
  return differences;
}

function replay(path, runners, update) {
  const fixture = JSON.parse(readFileSync(path, "utf8"));
  validateFixture(fixture, path);
  const runner = fixture.processor.kind === "adaptive-scroll" ? runners.scroll : runners.text;
  const result = spawnSync(resolve(runner), [], { input: runnerInput(fixture), encoding: "utf8" });
  if (result.status !== 0) throw new Error(`${fixture.id}: runner failed: ${result.stderr.trim()}`);
  const metrics = fixture.processor.kind === "adaptive-scroll"
    ? parseScrollOutput(result.stdout, fixture)
    : parseTextOutput(result.stdout, fixture);
  if (update) {
    fixture.expect = metrics;
    writeFileSync(path, `${JSON.stringify(fixture, null, 2)}\n`);
  }
  const differences = compareSubset(fixture.expect, metrics);
  return { id: fixture.id, path, metadata: fixture.metadata, metrics, differences };
}

try {
  const options = parseArguments(process.argv.slice(2));
  const runners = { scroll: options.scrollRunner, text: options.textRunner };
  const reports = options.fixtures.map((fixture) => replay(fixture, runners, options.update));
  for (const report of reports) {
    if (report.differences.length) {
      console.error(`FAIL ${report.id}`);
      for (const difference of report.differences) console.error(`  ${difference}`);
    } else {
      const detail = report.metrics.horizontal
        ? `H=${report.metrics.horizontal.signedDistance}, V=${report.metrics.vertical.signedDistance}`
        : `${report.metrics.steps.total} navigation steps`;
      console.log(`PASS ${report.id}: ${report.metrics.inputFrames} input, ${report.metrics.outputFrames} output, ${detail}`);
    }
  }
  if (options.json) writeFileSync(options.json, `${JSON.stringify({ schema: "zmk-pointing-tools/replay-report", version: 1, reports }, null, 2)}\n`);
  if (reports.some((report) => report.differences.length)) process.exitCode = 1;
} catch (error) {
  usage();
  console.error(error.message);
  process.exitCode = 2;
}
