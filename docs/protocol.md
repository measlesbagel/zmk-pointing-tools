# Host protocol

The runtime tuning protocol is not yet specified.

It will be versioned and capability-driven, with operations for describing
devices, reading compiled and live values, applying temporary previews,
resetting previews, optional persistence, and opt-in telemetry.

The first transport will be USB CDC-ACM for use through the Web Serial API. The
protocol itself must not depend on browser or serial-specific concepts.
