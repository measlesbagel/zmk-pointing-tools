# Examples

The repository-level smoke configuration in
[`../config/corne.keymap`](../config/corne.keymap) is the current example: it
instantiates cursor, scroll, text, and normalized-cursor pipelines, their
stages and sinks, a shared keypress-suppression guard, route behaviors, and
pipeline telemetry against the composed ZMK boundary described in
[`../docs/zmk-pipeline-boundary.md`](../docs/zmk-pipeline-boundary.md).

Future complete examples will cover both single-device and wireless split
dual-device configurations without embedding a personal keymap.
