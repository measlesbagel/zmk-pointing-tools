# Tests

Host tests exercise the reusable axis-intent, adaptive-scroll, and text-navigation models. The
deterministic replay suite runs versioned real-world-style fixtures through the same C models and
checks reviewable behavior metrics. See [trace replay](../docs/trace-replay.md).

The CI smoke firmware instantiates every processor binding, while browser tests cover protocol
frame parsing and fragmented serial input.
