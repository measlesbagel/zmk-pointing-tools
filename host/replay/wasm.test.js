import test from "node:test";
import assert from "node:assert/strict";

import { runFixtureWasm } from "./wasm.js";

/* A WASI command whose _start traps at once (an `unreachable`), as a runner
 * does on a stack overflow or an out-of-bounds access. */
const TRAPPING_RUNNER = new WebAssembly.Module(new Uint8Array([
  0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
  0x01, 0x04, 0x01, 0x60, 0x00, 0x00, // type: () -> ()
  0x03, 0x02, 0x01, 0x00, // function 0 has type 0
  0x05, 0x03, 0x01, 0x00, 0x01, // one page of memory
  0x07, 0x13, 0x02, 0x06, ...new TextEncoder().encode("memory"), 0x02, 0x00,
  0x06, ...new TextEncoder().encode("_start"), 0x00, 0x00,
  0x0a, 0x05, 0x01, 0x03, 0x00, 0x00, 0x0b, // body: unreachable
]));

test("a runner that traps fails with the fixture id and the trap", () => {
  const fixture = { id: "trap-check", version: 1, processor: { kind: "composed-noise",
    settings: { enabled: true, cpi: 800, activationDistanceMicrometers: 1, coherencePercent: 0,
      qualificationTimeoutMs: 1, idleTimeoutMs: 1, suppressAfterKeypressMs: 0 } },
  events: [["motion", 1, 1, 0]] };
  assert.throws(() => runFixtureWasm(fixture, { noisePipeline: TRAPPING_RUNNER }),
    /^Error: trap-check: runner trapped: unreachable/);
});
