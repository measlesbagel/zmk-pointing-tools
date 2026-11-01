/* SPDX-License-Identifier: MIT */

/*
 * Runs the replay runners compiled to WebAssembly (host/wasm/build.sh) in
 * the same process: in a browser worker or in Node. Each runner is a WASI
 * command that reads its input on stdin and writes the replay on stdout,
 * exactly like the native runner runner.js spawns, so runFixtureWasm
 * returns the same output runFixture does (host/wasm/parity.js checks
 * that). See docs/trace-replay.md, "Replay in the browser".
 *
 * This file must not import Node modules: the tuner loads it as is.
 */

import { encodeRunnerInput, runnerFor } from "./fixture.js";

/* Runner module file names, by the runner names runner.js uses. */
export const WASM_RUNNERS = {
  noisePipeline: "zpt_noise_pipeline_replay.wasm",
  scrollPipeline: "zpt_scroll_pipeline_replay.wasm",
  textPipeline: "zpt_text_nav_pipeline_replay.wasm",
  cursorPipeline: "zpt_cursor_pipeline_replay.wasm",
};

/* Compile every runner. load(fileName) returns the module bytes or a
 * promise of them. */
export async function compileWasmRunners(load) {
  const entries = await Promise.all(Object.entries(WASM_RUNNERS).map(async ([name, file]) =>
    [name, await WebAssembly.compile(await load(file))]));
  return Object.fromEntries(entries);
}

/* WASI preview 1 errno and file type values. */
const SUCCESS = 0;
const EBADF = 8;
const ESPIPE = 70;
const ENOSYS = 52;
const FILETYPE_UNKNOWN = 0;

class Exit {
  constructor(status) {
    this.status = status;
  }
}

/* Run a WASI command with the given stdin text and no arguments, files or
 * environment. Returns its exit status and output. A runner only needs
 * stdio, so every other call fails with ENOSYS. */
export function runWasi(module, input) {
  const stdin = new TextEncoder().encode(input);
  let stdinOffset = 0;
  const output = { 1: [], 2: [] };
  let memory;
  const view = () => new DataView(memory.buffer);
  const iovecs = (pointer, count) => Array.from({ length: count }, (_, index) => ({
    base: view().getUint32(pointer + index * 8, true),
    length: view().getUint32(pointer + index * 8 + 4, true),
  }));

  const wasi = {
    fd_read(fd, iovs, count, readPointer) {
      if (fd !== 0) return EBADF;
      let read = 0;
      for (const { base, length } of iovecs(iovs, count)) {
        const chunk = stdin.subarray(stdinOffset, stdinOffset + length);
        new Uint8Array(memory.buffer, base, chunk.length).set(chunk);
        stdinOffset += chunk.length;
        read += chunk.length;
        if (chunk.length < length) break;
      }
      view().setUint32(readPointer, read, true);
      return SUCCESS;
    },
    fd_write(fd, iovs, count, writtenPointer) {
      if (!(fd in output)) return EBADF;
      let written = 0;
      for (const { base, length } of iovecs(iovs, count)) {
        output[fd].push(new Uint8Array(memory.buffer, base, length).slice());
        written += length;
      }
      view().setUint32(writtenPointer, written, true);
      return SUCCESS;
    },
    /* Not a terminal, so libc buffers stdout fully instead of by line. */
    fd_fdstat_get(fd, pointer) {
      if (fd > 2) return EBADF;
      const data = view();
      data.setUint8(pointer, FILETYPE_UNKNOWN);
      data.setUint16(pointer + 2, 0, true);
      data.setBigUint64(pointer + 8, 0xffffffffffffffffn, true);
      data.setBigUint64(pointer + 16, 0xffffffffffffffffn, true);
      return SUCCESS;
    },
    fd_seek: () => ESPIPE,
    fd_close: () => SUCCESS,
    proc_exit(status) {
      throw new Exit(status);
    },
  };
  const imports = new Proxy(wasi, {
    get: (target, name) => target[name] ?? (() => ENOSYS),
  });

  const instance = new WebAssembly.Instance(module, { wasi_snapshot_preview1: imports });
  memory = instance.exports.memory;
  let status = 0;
  try {
    instance.exports._start();
  } catch (error) {
    if (!(error instanceof Exit)) throw error;
    status = error.status;
  }
  const decode = (chunks) => {
    const decoder = new TextDecoder();
    return chunks.map((chunk) => decoder.decode(chunk, { stream: true })).join("") +
      decoder.decode();
  };
  return { status, stdout: decode(output[1]), stderr: decode(output[2]) };
}

/* The WebAssembly counterpart of runner.js's runFixture: replay a fixture
 * through its compiled runner and return the raw output. An override
 * profile replaces cursor stage settings by stable id. */
export function runFixtureWasm(fixture, modules, overrideProfile = null) {
  const result = runWasi(modules[runnerFor(fixture)], encodeRunnerInput(fixture, overrideProfile));
  if (result.status !== 0) {
    throw new Error(`${fixture.id}: runner failed: ${result.stderr.trim()}`);
  }
  return result.stdout;
}
