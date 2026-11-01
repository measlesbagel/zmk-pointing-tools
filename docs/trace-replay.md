# Deterministic trace replay

Trace replay runs captured motion through the same host-buildable C models used by the firmware.
It makes processor output and internal classification metrics reproducible without a keyboard,
Bluetooth timing, or wall-clock scheduling.

## Run the regression suite

```sh
cmake -S host -B build/host -G Ninja
cmake --build build/host
ctest --test-dir build/host --output-on-failure
```

Generate a machine-readable report for review or comparison:

```sh
node host/replay/cli.js \
  --noise-pipeline-runner build/host/zpt_noise_pipeline_replay \
  --scroll-pipeline-runner build/host/zpt_scroll_pipeline_replay \
  --text-pipeline-runner build/host/zpt_text_nav_pipeline_replay \
  --cursor-pipeline-runner build/host/zpt_cursor_pipeline_replay \
  --json /tmp/replay.json \
  host/tests/fixtures/*.json
```

The command exits nonzero and prints field-level differences when an expected metric changes.
After intentionally changing processor behavior, inspect the report before updating snapshots:

```sh
node host/replay/cli.js \
  --noise-pipeline-runner build/host/zpt_noise_pipeline_replay \
  --scroll-pipeline-runner build/host/zpt_scroll_pipeline_replay \
  --text-pipeline-runner build/host/zpt_text_nav_pipeline_replay \
  --cursor-pipeline-runner build/host/zpt_cursor_pipeline_replay \
  --update \
  host/tests/fixtures/*.json
git diff -- host/tests/fixtures
```

Never update expectations only to make CI pass. The metrics are the review surface for behavior
changes.

To score whether a change is better rather than only whether it changed, use the trace benchmark
in [`trace-benchmark.md`](trace-benchmark.md). It also defines fixture format version 2.

## Replay in the browser

The hosted tuner replays traces without a checkout (#124). The Pages build compiles the four
replay runners and the firmware stage code they link to WebAssembly and publishes them under
`replay/` next to the tuner, with the JavaScript that runs them:

| File | Role |
| --- | --- |
| `zpt_*_pipeline_replay.wasm` | The runners in `host/runners`, built from `host/CMakeLists.txt` for `wasm32-wasip1` (`host/wasm/wasi.cmake`). Each is a WASI command that reads runner input on stdin and writes the replay on stdout, as the native runner does. |
| `wasm.js` | Runs a runner module in-process with a stdio-only WASI shim; `runFixtureWasm` is the counterpart of `runner.js`'s `runFixture`. |
| `score.js` | `scoreTraces(traces, profile, runners)`: validates fixture v1 or v2 traces, replays them, and scores them with `quality.js`. `bench.js` scores its sets through the same `scoreFixtures`. |
| `score-worker.js`, `score-client.js` | Run scoring in a module Web Worker, so pointing in the tuner stays smooth. |
| `fixture.js`, `metrics.js`, `quality.js` | Shared unchanged with the host tools. |
| `capture-convert.js` | `decodeCaptureStream` and `captureToFixtures`: firmware input captures to fixture v2 traces, shared with `capture.js`. Its import of the tuner's `protocol.js` is rewritten to `../protocol.js`, so `replay/` must sit next to the tuner. |

A page scores traces like this:

```js
import { createScoreWorker } from "./replay/score-client.js";

const scorer = createScoreWorker();
const set = await scorer.scoreTraces(traces, profile); // profile: tuning profile, or null
// set: { fixtures, scores, fixtureResults }, one set as bench.js --json reports it
```

`profile` replaces the traces' recorded stage-chain settings (cursor, and recorded scroll and
text pipelines) by stable id, as
`bench.js --profile` does; `null` scores the recorded settings. The result has no baseline
comparison: to compare candidates, score each and compare their `scores`. Everything comes from
the traces and the profile, so nothing assumes a particular board, keymap or config repository.

Build and check it locally in the devenv shell, which pins the WASI clang, wasi-libc and wasm-ld
through `devenv.lock`:

```sh
devenv tasks run wasm:test   # builds both runner sets, then checks parity and the scoring API
host/wasm/build.sh /tmp/replay   # or assemble the browser module by hand
```

`host/wasm/parity.js` replays every regression fixture and benchmark fixture through the native
and the WebAssembly runners, with the recorded settings, with sensor spans, and with every
benchmark profile, and fails if any output byte differs. `repository:check` runs it, and the
Pages deploy fails without it. The runner modules are build output and are never committed.

## Fixture format

Regression fixtures use `zmk-pointing-tools/trace-fixture` version 1, described here. Version 2
adds microsecond timing, sensor sample spans, the source, the full cursor pipeline, and ground
truth; see [`trace-benchmark.md`](trace-benchmark.md#fixture-format-v2). Both versions replay
through the same runners and regression checks. Each version 1 fixture records:

- an ID and human description;
- device, CPI, nominal cadence, mode, and provenance metadata;
- the pipeline kind, exact settings, and optional axis policy;
- ordered events encoded as `[type, deltaMs, ...values]`;
- expected metrics, which may be a subset of the generated report.

Motion is `["motion", deltaMs, x, y]`. Scroll fixtures may also contain
`["keypress", deltaMs]` to reproduce the physical-keypress guard. Delta time is relative to the
previous event, making replay independent of uptime and timestamp wrap in the original capture.

Supported pipeline kinds are currently:

- `composed-noise`, backed by resolution normalization and the
  coherent-displacement gate;
- `composed-scroll`, backed by resolution normalization, the axis-intent
  estimator, the axis-constraint, and the scroll batcher;
- `composed-text`, backed by resolution normalization, the shared
  axis-intent estimator, the axis-constraint, and the text-navigation
  threshold mapper;
- `cursor-pipeline`, backed by any ordered list of the cursor stages:
  orientation, resolution normalization, the coherent-displacement gate, cursor
  acceleration, the cursor transfer, the sub-pixel cursor quantizer, and the
  report coalescer. Version 1 fixtures run normalization, transfer, and
  quantizer; version 2 fixtures list their stages and settings;
- `scroll-pipeline` and `text-pipeline` (version 2 only), recorded scroll and
  text-navigation pipelines: an ordered list of orientation, resolution
  normalization, the motion gate, `axis-intent`, `axis-constraint`, and a last
  `scroll-batcher` or `text-nav` stage, with their settings, as firmware input
  capture describes them (`policy` and `suppress-after-keypress-ms` beside the
  tuning keys). Unlike the composed kinds, they replay with microsecond event
  times, recorded flushes, and late events (#123), and `--profile` applies to
  them by stage stable id.

`cursor-pipeline`, `scroll-pipeline`, and `text-pipeline` all replay through
`zpt_cursor_pipeline_replay`, the stage-chain runner; it tells them apart by the
kind of signal the last stage sends. Its scroll and text output is one
`D <t_ms> <x> <y> <intent> <suppressed> <t_us>` line per frame, then
`O <t_ms> <x> <y> <t_us>` per scroll report or `O <t_ms> <direction> <t_us>` per
text step. The composed kinds and their fixtures are unchanged.

Reports include input/output frame counts, signed and absolute distance, cadence, direction
changes, HID clipping, intent occupancy/transitions, idle resets, and suppressed frames as
applicable. The models deliberately use integer arithmetic matching firmware behavior.
