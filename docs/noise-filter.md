# Dead-zone and noise filter

The noise filter qualifies complete X/Y vectors independently from scroll axis
intent, cursor gain, or text-navigation classification. It is disabled unless
the devicetree node contains `enabled`.

## Chosen policy

While idle, the filter buffers vectors instead of dropping each small frame.
Movement becomes intentional when:

1. its accumulated net vector reaches `activation-distance`; and
2. its rotation-invariant directional coherence reaches `coherence-percent`.

On qualification, the entire buffered vector is emitted. Subsequent vectors
pass unchanged until `idle-timeout-ms` elapses. Pending input that cannot
qualify within `qualification-timeout-ms` is discarded and starts a fresh
window. An optional physical-keypress guard can immediately reject typing
vibration.

For pending vectors `v₁…vₙ`, coherence is:

```text
|Σv| / sqrt(n × Σ|v|²)
```

The firmware compares the squared form using integer arithmetic. It is
rotation invariant, so equal-strength diagonal and cardinal motion are treated
the same. Repeated one-count intentional movement can accumulate and qualify,
while alternating jitter has low net displacement and coherence.

## Alternatives considered

| Policy | Reason it is not the baseline |
| --- | --- |
| Per-axis threshold | Biases diagonals and can independently chatter X and Y |
| Per-frame radial threshold | Rotation invariant, but permanently loses slow sub-threshold movement |
| Hard accumulated radial gate | Preserves distance, but random or alternating motion can eventually qualify |
| Velocity threshold | Useful for acceleration, but rejects deliberate low-speed precision movement |
| Statistical/adaptive noise floor | Potentially useful later, but harder to reason about and replay deterministically |

Temporal active/idle phases provide hysteresis without changing vector
direction. Qualification introduces a short startup delay and then releases
the buffered distance; keep activation values small enough that this release
does not feel like a jump.

## Devicetree usage

The processor consumes the original events and emits qualified X/Y frames from
its own virtual device. Give that virtual source a dedicated input listener:

```dts
/ {
    cursor_noise: cursor_noise {
        compatible = "measlesbagel,zpt-input-processor-noise-filter";
        #input-processor-cells = <0>;
        tuning-id = "cursor-noise";
        tuning-label = "Cursor noise filter";
        enabled;
        activation-distance = <6>;
        coherence-percent = <60>;
        qualification-timeout-ms = <160>;
        idle-timeout-ms = <120>;
        suppress-after-keypress-ms = <40>;
    };

    cursor_noise_output: cursor_noise_output {
        compatible = "zmk,input-listener";
        device = <&cursor_noise>;
        input-processors = <&my_output_transform>;
    };
};

&my_physical_input_listener {
    input-processors = <&my_orientation_transform>, <&cursor_noise>;
};
```

Use separate instances when devices or semantic modes need different policy.
For example, a cursor can use a small activation distance while an always-on
scroll device uses a keypress guard. When disabled, the processor still routes
complete frames through its virtual output but does not modify their values.

All settings are runtime tunable and reset the pending gesture when changed.
Semantic telemetry reports idle, pending, active, and bypass phases along with
qualification, suppression, discarded pending input, and emitted vectors.
