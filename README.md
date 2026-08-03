# ZMK Pointing Tools

Reusable pointing-device processing, runtime tuning, telemetry, and local web
tools for ZMK keyboards.

This project is being developed initially for a wireless Bridges V2 split with
two PAW3222 trackballs, but its firmware APIs and host protocol are intended to
work with arbitrary ZMK pointing devices.

## Capabilities

- Frame-aware movement coalescing
- Synchronized two-axis scrolling with fractional accumulation
- Adaptive, vertical-only, horizontal-only, and free scroll policies
- Gesture-locked movement-to-behavior processing for text navigation
- Pass-through staged motion telemetry and trace recording
- An offline, repository-owned Web Serial trace viewer
- Discoverable, validated scroll and text-navigation previews that never write flash
- Firmware-provided hover/focus explanations for tuning parameters
- Versioned profile export/import and generated devicetree handoff
- Deterministic regression replay of exported hardware traces

Planned capabilities include:

- Threshold-aware optional automatic mouse layers
- Experimental motion-derived gestures such as tap-to-click

## Repository boundaries

This repository implements reusable pointing behavior. It deliberately does
not contain keyboard matrix definitions, sensor wiring, or personal keymaps.

- Hardware: [`measlesbagel/bridges-v2-zmk-firmware`](https://github.com/measlesbagel/bridges-v2-zmk-firmware)
- Consumer configuration: [`measlesbagel/zmk-config`](https://github.com/measlesbagel/zmk-config)

See [`docs/architecture.md`](docs/architecture.md) for the initial design.

## Status

The current milestone provides pass-through telemetry, synchronized adaptive
scrolling, gesture-locked text navigation, and a local static tuning surface.
Scroll and text-navigation values can be previewed temporarily, exported as a
profile, and reset to compiled defaults. Explicit optional persistence remains
under development.

See [`docs/processors.md`](docs/processors.md) for devicetree usage.
See [`docs/profiles.md`](docs/profiles.md) for profile and configuration-handoff
semantics.
See [`docs/trace-replay.md`](docs/trace-replay.md) for deterministic processor
regression testing.

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
