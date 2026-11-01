#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Builds the replay runners for WebAssembly and assembles the browser replay
# module in OUT_DIR: the runner modules (*.wasm) and the JavaScript that
# loads them, with the shared fixture, metrics and quality code from
# host/replay. See docs/trace-replay.md, "Replay in the browser".
#
#   host/wasm/build.sh OUT_DIR
#
# Run it in the devenv shell, which pins the WASI toolchain (devenv.nix).
# ZPT_WASM_BUILD_DIR overrides the CMake build directory (build/wasm).
set -euo pipefail

out=${1:?usage: host/wasm/build.sh OUT_DIR}
root=$(cd "$(dirname "$0")/../.." && pwd)
build=${ZPT_WASM_BUILD_DIR:-$root/build/wasm}

cmake -S "$root/host" -B "$build" -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$root/host/wasm/wasi.cmake" -DBUILD_TESTING=OFF
cmake --build "$build" --target zpt_noise_pipeline_replay zpt_scroll_pipeline_replay \
  zpt_cursor_pipeline_replay zpt_text_nav_pipeline_replay

mkdir -p "$out"
cp "$build"/zpt_{noise,scroll,cursor,text_nav}_pipeline_replay.wasm "$out/"
for module in fixture metrics quality wasm score score-worker score-client; do
  cp "$root/host/replay/$module.js" "$out/"
done
