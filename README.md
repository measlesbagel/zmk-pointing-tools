# ZMK Pointing Tools

Reusable pointing-device processing, runtime tuning, telemetry, and local web
tools for ZMK keyboards.

This project is being developed initially for a wireless Bridges V2 split with
two PAW3222 trackballs, but its firmware APIs and host protocol are intended to
work with arbitrary ZMK pointing devices.

## Capabilities

- Composable motion pipelines: sources, gates, estimators, constraints,
  transfer stages, quantizers, batchers, and thin sinks
- Canonical source identity, transport, CPI metadata, and Q16 millimetre normalization
- Synchronized two-axis scrolling with fractional accumulation
- Adaptive, vertical-only, horizontal-only, and free scroll policies
- Rotation-invariant buffered dead-zone and noise qualification
- Gesture-locked movement-to-behavior processing for text navigation
- Cursor base gain and sub-pixel quantizer stages
- Layer- and behavior-driven pipeline routing with leave-before-enter lifecycle
- Shared physical-keypress suppression across every pipeline stage
- Per-stage decision telemetry with stable identity and discovery
- An offline, repository-owned tuner that connects over Web Serial or Web Bluetooth
- Versioned profile export/import and generated devicetree handoff
- Deterministic byte-exact regression replay through the composed stages
- Guided scroll, text, cursor, click, and drag playground activities
- Repeated-gesture calibration of cursor gain, scroll speed, and text steps

Planned capabilities include:

- Threshold-aware optional automatic mouse layers
- Experimental motion-derived gestures such as tap-to-click

## Repository boundaries

This repository implements reusable pointing behavior. It deliberately does
not contain keyboard matrix definitions, sensor wiring, or personal keymaps.

- Hardware: [`measlesbagel/bridges-v2-zmk-firmware`](https://github.com/measlesbagel/bridges-v2-zmk-firmware)
- Consumer configuration: [`measlesbagel/zmk-config`](https://github.com/measlesbagel/zmk-config)

See [`docs/architecture.md`](docs/architecture.md) for the accepted direction
for composable sources, typed motion stages, routing, and thin output sinks.
See [`docs/source-layout.md`](docs/source-layout.md) for how those ownership
boundaries map to source and public-header directories.
See [`docs/source-normalization.md`](docs/source-normalization.md) for source
metadata, mounting orientation, native split reconstruction, and Q16 physical
units.

## Status

The composable pipeline architecture is fully migrated: cursor, scroll, and
text behavior run through shared stages with a ZMK router, explicit lifecycle,
keypress suppression, and per-stage telemetry. The superseded monolithic
processors are removed, and every regression fixture replays byte-exactly
through the composed stages, and their parameters are tunable at runtime from
the web tuner. The cursor path adds an eight-point acceleration curve, which the
tuner edits graphically with presets, and report coalescing with separate USB
and Bluetooth rates.

See [`docs/architecture.md`](docs/architecture.md) for the pipeline model.
See [`docs/zmk-pipeline-boundary.md`](docs/zmk-pipeline-boundary.md) for the
composable runtime integration with ZMK.
See [`docs/motion-gate.md`](docs/motion-gate.md) for dead-zone design and
tuning guidance.
See [`docs/profiles.md`](docs/profiles.md) for profile and configuration-handoff
semantics.
See [`docs/trace-replay.md`](docs/trace-replay.md) for deterministic regression
replay.
See [`docs/playground.md`](docs/playground.md) for guided hardware evaluation.
See [`docs/calibration.md`](docs/calibration.md) for per-device gain
calibration.
See [`docs/agent-tools.md`](docs/agent-tools.md) for letting your own AI agent
tune through the tuner, and [`docs/agent-guide.md`](docs/agent-guide.md) for a
guide to give the agent itself.
See [`host/README.md`](host/README.md) for the native development and test
subsystem.
See [`docs/build.md`](docs/build.md) for host checks and the firmware smoke
build.

## Hosted tuner

The tuner is published from `main` at
<https://measlesbagel.github.io/zmk-pointing-tools/>. Open it in desktop Chrome
or Edge to connect over USB or Bluetooth, simulate, or calibrate without cloning
the repository. The commit it was built from is shown at the bottom of the page.

### Connecting over Bluetooth

**Connect via Bluetooth** reaches the keyboard's central half without a cable,
beside its normal connection as a keyboard. It needs:

- Firmware with the Bluetooth transport. It's on by default for Bluetooth
  keyboards that include the telemetry service; see
  [`docs/protocol.md`](docs/protocol.md#bluetooth-gatt).
- A computer already paired (bonded) with the keyboard. The tuner can change
  settings, so the keyboard refuses computers it isn't bonded with.
- Chrome or Edge. Firefox and Safari have no Web Bluetooth. On Linux, if the
  tuner says Web Bluetooth is unavailable, turn on Experimental Web Platform
  features in `chrome://flags`.

The browser's picker lists nearby devices rather than filtering for the
keyboard, because the tuning service isn't advertised. A keyboard that is
connected to a computer usually stops advertising, so it may be missing from
the picker. If it is, switch the keyboard to a free Bluetooth profile, or turn
the computer's Bluetooth off and on, then try again.
Everything works as over USB, including agent tools, previews, revert and
calibration. Every-frame telemetry is limited by the Bluetooth connection: the
firmware drops the samples the link can't carry and the tuner shows them as
dropped, so calibration over Bluetooth may need longer or slower strokes.

## Local tuner

For offline use and development, serve `web/` locally.

With Devenv 2.2 installed:

```console
devenv up
```

Then open <http://localhost:8787> in desktop Chrome. The simulator works without
hardware. Serving from localhost is required because Web Serial and Web
Bluetooth are unavailable to pages opened directly from the filesystem.

## License

MIT
