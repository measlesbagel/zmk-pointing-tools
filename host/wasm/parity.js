#!/usr/bin/env node
/* SPDX-License-Identifier: MIT */

/*
 * WASM and native replay must give identical output. Replays every
 * regression fixture (host/tests/fixtures) and benchmark fixture
 * (host/bench/fixtures) through both runner builds, with the recorded
 * settings, with sensor spans, and with every benchmark profile, and fails
 * on the first byte that differs. See docs/trace-replay.md.
 *
 *   node host/wasm/parity.js --native-dir build/host --wasm-dir build/wasm
 */

import { readdirSync, readFileSync } from "node:fs";
import { dirname, join, relative, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { parseArgs } from "node:util";

import { isStageChain, validateProfile } from "../replay/fixture.js";
import { loadFixture, runFixture } from "../replay/runner.js";
import { compileWasmRunners, runFixtureWasm, WASM_RUNNERS } from "../replay/wasm.js";

const REPOSITORY = resolve(dirname(fileURLToPath(import.meta.url)), "../..");

const jsonFiles = (directory) => readdirSync(directory, { recursive: true })
  .filter((name) => name.endsWith(".json")).sort().map((name) => join(directory, name));

async function main() {
  const { values } = parseArgs({
    options: {
      "native-dir": { type: "string", default: join(REPOSITORY, "build/host") },
      "wasm-dir": { type: "string", default: join(REPOSITORY, "build/wasm") },
    },
  });
  const native = Object.fromEntries(Object.entries(WASM_RUNNERS).map(([name, file]) =>
    [name, join(values["native-dir"], file.replace(/\.wasm$/, ""))]));
  const wasm = await compileWasmRunners((file) => readFileSync(join(values["wasm-dir"], file)));
  const profiles = jsonFiles(join(REPOSITORY, "host/bench/profiles")).map((path) =>
    ({ name: relative(REPOSITORY, path),
      profile: validateProfile(JSON.parse(readFileSync(path, "utf8")), path) }));
  const paths = [...jsonFiles(join(REPOSITORY, "host/tests/fixtures")),
    ...jsonFiles(join(REPOSITORY, "host/bench/fixtures"))];

  let replays = 0;
  let bytes = 0;
  const failures = [];
  for (const path of paths) {
    const fixture = loadFixture(path);
    const chain = isStageChain(fixture);
    const variants = [{ name: "recorded settings", fixture, profile: null }];
    if (fixture.version === 2 && fixture.source.span !== "sensor") {
      variants.push({ name: "sensor spans",
        fixture: { ...fixture, source: { ...fixture.source, span: "sensor" } }, profile: null });
    }
    if (chain) {
      for (const { name, profile } of profiles) variants.push({ name, fixture, profile });
    }
    for (const variant of variants) {
      const expected = runFixture(variant.fixture, native, variant.profile);
      const actual = runFixtureWasm(variant.fixture, wasm, variant.profile);
      replays += 1;
      bytes += expected.length;
      if (expected !== actual) {
        const line = expected.split("\n").findIndex((text, index) =>
          text !== actual.split("\n")[index]);
        failures.push(`${relative(REPOSITORY, path)} (${variant.name}): output differs ` +
          `from line ${line + 1}`);
      }
    }
  }
  for (const failure of failures) console.error(`FAIL ${failure}`);
  console.log(`${failures.length ? "FAIL" : "PASS"} ${paths.length} fixtures, ${replays} ` +
    `replays, ${bytes} output bytes: WASM ${failures.length ? "differs from" : "matches"} ` +
    "native replay");
  if (failures.length || replays === 0) process.exitCode = 1;
}

main().catch((error) => {
  console.error(error.message);
  process.exitCode = 2;
});
