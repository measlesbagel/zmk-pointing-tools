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
- An offline, repository-owned Web Serial tuner
- Versioned profile export/import and generated devicetree handoff
- Deterministic byte-exact regression replay through the composed stages
- Guided scroll, text, cursor, click, and drag playground activities

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
through the composed stages. Acceleration curves, cursor report coalescing,
and runtime tuning of stage parameters remain under development.

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
See [`host/README.md`](host/README.md) for the native development and test
subsystem.

## Local tuner

With Devenv 2.2 installed:

```console
devenv up
```

Then open <http://localhost:8787> in desktop Chrome. The simulator works without
hardware. Serving from localhost is required because Web Serial is unavailable
to pages opened directly from the filesystem.

## License

MIT
