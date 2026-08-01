# ZMK Pointing Tools

Reusable pointing-device processing, runtime tuning, telemetry, and local web
tools for ZMK keyboards.

This project is being developed initially for a wireless Bridges V2 split with
two PAW3222 trackballs, but its firmware APIs and host protocol are intended to
work with arbitrary ZMK pointing devices.

## Planned capabilities

- Frame-aware movement coalescing for split pointing devices
- Synchronized two-axis scrolling with fractional accumulation
- Adaptive, vertical-only, horizontal-only, and free axis policies
- Generic movement-to-behavior processing for text navigation
- Threshold-aware optional automatic mouse layers
- Runtime preview and introspection of tunable parameters
- Opt-in motion telemetry and trace recording
- An offline, repository-owned Web Serial tuning interface
- Experimental motion-derived gestures such as tap-to-click

## Repository boundaries

This repository implements reusable pointing behavior. It deliberately does
not contain keyboard matrix definitions, sensor wiring, or personal keymaps.

- Hardware: [`measlesbagel/bridges-v2-zmk-firmware`](https://github.com/measlesbagel/bridges-v2-zmk-firmware)
- Consumer configuration: [`measlesbagel/zmk-config`](https://github.com/measlesbagel/zmk-config)

See [`docs/architecture.md`](docs/architecture.md) for the initial design.

## Status

Architecture and module scaffolding. No processors or host service are enabled
yet.

## License

MIT
