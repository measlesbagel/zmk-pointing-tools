/* SPDX-License-Identifier: MIT */

/*
 * Web Worker that scores traces off the page's main thread, so pointing in
 * the tuner stays smooth while a replay runs. Load it as a module worker
 * through score-client.js. The runner modules (*.wasm) sit next to this
 * file; host/wasm/build.sh assembles them.
 *
 * Message in:  { id, traces, profile }
 * Message out: { id, result } with scoreTraces's result, or { id, error }.
 */

import { scoreTraces } from "./score.js";
import { compileWasmRunners } from "./wasm.js";

let runners = null;

async function load(file) {
  const response = await fetch(new URL(file, import.meta.url));
  if (!response.ok) throw new Error(`${file}: HTTP ${response.status}`);
  return response.arrayBuffer();
}

/* Compile the runners once; a failed load is retried on the next request. */
function compiledRunners() {
  runners ??= compileWasmRunners(load).catch((error) => {
    runners = null;
    throw error;
  });
  return runners;
}

self.addEventListener("message", async ({ data: { id, traces, profile } }) => {
  try {
    self.postMessage({ id, result: scoreTraces(traces, profile, await compiledRunners()) });
  } catch (error) {
    self.postMessage({ id, error: error.message });
  }
});
