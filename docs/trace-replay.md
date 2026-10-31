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
  quantizer; version 2 fixtures list their stages and settings.

Reports include input/output frame counts, signed and absolute distance, cadence, direction
changes, HID clipping, intent occupancy/transitions, idle resets, and suppressed frames as
applicable. The models deliberately use integer arithmetic matching firmware behavior.
