/* SPDX-License-Identifier: MIT */

/* The browser scoring API against bench.js. Needs both runner builds:
 * ZPT_NATIVE_DIR (default build/host) and ZPT_WASM_DIR (default
 * build/wasm); the wasm:test dev task sets them. */

import test from "node:test";
import assert from "node:assert/strict";
import { spawnSync } from "node:child_process";
import { mkdtempSync, readdirSync, readFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";

import { scoreTraces } from "../replay/score.js";
import { compileWasmRunners } from "../replay/wasm.js";

const REPOSITORY = resolve(dirname(fileURLToPath(import.meta.url)), "../..");
const NATIVE = process.env.ZPT_NATIVE_DIR ?? join(REPOSITORY, "build/host");
const WASM = process.env.ZPT_WASM_DIR ?? join(REPOSITORY, "build/wasm");
const TUNE = join(REPOSITORY, "host/bench/fixtures/tune");
const SHORT_RISE = join(REPOSITORY, "host/bench/profiles/short-rise.json");

const runners = await compileWasmRunners((file) => readFileSync(join(WASM, file)));
const readJson = (path) => JSON.parse(readFileSync(path, "utf8"));
const tune = readdirSync(TUNE).filter((name) => name.endsWith(".json")).sort()
  .map((name) => readJson(join(TUNE, name)));

/* bench.js's report for the tune set, natively replayed. */
function bench(...options) {
  const json = join(mkdtempSync(join(tmpdir(), "zpt-bench-")), "bench.json");
  const result = spawnSync(process.execPath, [join(REPOSITORY, "host/replay/bench.js"),
    "--build-dir", NATIVE, "--json", json, ...options], { encoding: "utf8" });
  assert.equal(result.status, 0, result.stderr);
  return readJson(json).sets.tune.fixtureResults.map(({ path, ...rest }) => rest);
}

test("scoreTraces reports what bench.js reports for the same traces", () => {
  const result = scoreTraces(tune, null, runners);
  assert.equal(result.fixtures, tune.length);
  assert.deepEqual(result.fixtureResults, bench());
});

test("a profile replaces the recorded settings as bench.js --profile does", () => {
  const profile = readJson(SHORT_RISE);
  const withProfile = scoreTraces(tune, profile, runners);
  assert.deepEqual(withProfile.fixtureResults, bench("--profile", SHORT_RISE));
  assert.notDeepEqual(withProfile.scores, scoreTraces(tune, null, runners).scores);
});

test("invalid traces and runner failures are reported", () => {
  assert.throws(() => scoreTraces([{ ...tune[0], events: [] }], null, runners),
    /events are required/);
  const cursor = tune.find((trace) => trace.processor.kind === "cursor-pipeline");
  const stage = cursor.processor.stages.find((candidate) => candidate.type === "transfer");
  const profile = { schema: "zmk-pointing-tools/tuning-profile", version: 1,
    targets: [{ stableId: stage.stableId, parameters: [{ key: "no-such-key", value: 1 }] }] };
  assert.throws(() => scoreTraces([cursor], profile, runners),
    new RegExp(`runner failed: ${stage.stableId}: unknown parameter no-such-key`));
});
