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
  --scroll-runner build/host/zpt_scroll_replay \
  --text-runner build/host/zpt_text_nav_replay \
  --json /tmp/replay.json \
  host/tests/fixtures/*.json
```

The command exits nonzero and prints field-level differences when an expected metric changes.
After intentionally changing processor behavior, inspect the report before updating snapshots:

```sh
node host/replay/cli.js \
  --scroll-runner build/host/zpt_scroll_replay \
  --text-runner build/host/zpt_text_nav_replay \
  --update \
  host/tests/fixtures/*.json
git diff -- host/tests/fixtures
```

Never update expectations only to make CI pass. The metrics are the review surface for behavior
changes.

## Fixture format

Fixtures use `zmk-pointing-tools/trace-fixture` version 1. Each fixture records:

- an ID and human description;
- device, CPI, nominal cadence, mode, and provenance metadata;
- the processor kind, exact settings, and optional axis policy;
- ordered events encoded as `[type, deltaMs, ...values]`;
- expected metrics, which may be a subset of the generated report.

Motion is `["motion", deltaMs, x, y]`. Scroll fixtures may also contain
`["keypress", deltaMs]` to reproduce the physical-keypress guard. Delta time is relative to the
previous event, making replay independent of uptime and timestamp wrap in the original capture.

Supported models are currently:

- `adaptive-scroll`, backed by `src/scroll.c` and `src/axis_intent.c`;
- `text-navigation`, backed by `src/text_nav.c`.

Reports include input/output frame counts, signed and absolute distance, cadence, direction
changes, HID clipping, intent occupancy/transitions, idle resets, and suppressed frames as
applicable. The models deliberately use integer arithmetic matching firmware behavior.

## Import a tuner export

Start by copying a similar fixture so its processor settings and metadata are explicit. Then
replace its event list from an exported tuner trace:

```sh
node host/replay/import.js \
  --input ~/Downloads/zmk-pointing-trace.json \
  --stream 0:0 \
  --template host/tests/fixtures/left-split-transport.json \
  --output /tmp/new-left-fixture.json \
  --start 100 \
  --count 500
```

Edit the new fixture's ID, description, provenance, CPI, mode, and processor settings. Remove
irrelevant leading/trailing frames if necessary. Generate and inspect expectations with
`--update`, then commit only a representative anonymized segment. Prefer the smallest segment
that reproduces the behavior; large raw exports should remain outside the repository.

The tuner export does not currently contain physical key events. Add keypress events manually
when reproducing keypress-guard behavior.

## Adding another processor

Keep algorithms host-buildable and deterministic: isolate Zephyr device, workqueue, HID, and
event-manager integration in the firmware wrapper. Add a small stdin/stdout runner under
`host/runners`, teach the modules under `host/replay` its settings and metrics, register the target
in `host/CMakeLists.txt`, and add at least one fixture covering a historical or boundary case.
