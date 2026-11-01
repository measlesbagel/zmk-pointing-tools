# Host development tools

The host subsystem builds the firmware's pure C processor models as a native
library and exercises them without Zephyr or keyboard hardware.

The shared composable pipeline core and its lifecycle contract are documented
in [`../docs/pipeline-runtime.md`](../docs/pipeline-runtime.md). Host tests also
exercise the generic frame assembler, source metadata, mounting orientation,
and CPI-to-Q16 millimetre normalization.

```sh
cmake -S host -B build/host -G Ninja
cmake --build build/host
ctest --test-dir build/host --output-on-failure
```

The build produces native composed pipeline replay runners for the noise,
scroll, text-navigation, and cursor fixture kinds. CTest runs the C unit tests
and all versioned fixtures under `host/tests/fixtures` through the JavaScript
replay client.

`node host/replay/bench.js` scores the benchmark fixture sets in
`host/bench` against their committed baseline; see
[`../docs/trace-benchmark.md`](../docs/trace-benchmark.md).

`host/wasm` builds the same runners for WebAssembly, so the hosted tuner can
replay and score traces in the browser, and checks they match native replay;
see [`../docs/trace-replay.md`](../docs/trace-replay.md#replay-in-the-browser).
The `host/wasm` checks need both builds' output, so they run from
`devenv tasks run wasm:test` rather than `npm test`.

The native and WebAssembly binaries are build artifacts and are never committed or included
in keyboard firmware.
