# Minimal ZMK pipeline boundary

The first firmware integration of the composable runtime is deliberately an
identity cursor path. It validates the ZMK boundary without changing motion
semantics or prematurely fixing the full pipeline devicetree model.

```text
physical or proxied input events
  -> standard ZMK input listener
  -> pipeline ingress input processor
  -> complete raw X/Y frame with source metadata
  -> orthogonal mounting orientation
  -> raw-pointer identity stage
  -> thin cursor sink
  -> virtual input device
  -> dedicated ZMK output listener
  -> HID cursor report
```

## Ingress choice

The standard `zmk,input-listener` processor chain is the initial ingress host.
This retains normal ZMK source selection and allows local and central-side
split proxy devices to use the same adapter. A separate global Zephyr input
callback would compete with the standard listener and would need to recreate
its source ownership externally.

The ingress consumes scalar X/Y values by replacing their source event values
with zero. Returning `STOP` is not sufficient because a layer override can
consume that return value before ZMK's HID listener sees it. Other event types
continue unchanged. A `sync` marker on any later scalar event completes the
pending X/Y frame, so the adapter does not assume that Y is always the final
event.

Attach this processor to the base listener, not to per-layer overrides. The
future router will remain attached once and observe route changes explicitly;
using ZMK's per-event overrides as a router would reintroduce incomplete-frame
and missing-lifecycle problems.

## Identity stage and cursor sink

An orthogonal orientation stage first applies the configured axis swap and
inversions. The identity stage then maps those signed raw counts to whole Q16
pointer deltas without gain, filtering, or rounding. It exists to prove exact
cursor parity and will be bypassed by the normalized cursor composition once a
pointer mapper and quantizer are available.

The cursor sink accepts only whole pointer deltas within ZMK's signed 16-bit
HID movement range. It performs no scaling, clipping, accumulation, or cadence
selection. After validating both axes, it emits `REL_X` followed by a
synchronized `REL_Y` from the pipeline processor's virtual input device.

## Devicetree wiring

```dts
/ {
    my_cursor_pipeline: my_cursor_pipeline {
        compatible = "measlesbagel,zpt-input-processor-pipeline";
        #input-processor-cells = <0>;
        stable-id = "right-cursor";
        source-id = <1>;
        resolution-cpi = <700>;
        invert-x;
        invert-y;
        /* Add for a central-side split proxy source. */
        /* transported; */
    };

    my_cursor_output: my_cursor_output {
        compatible = "zmk,input-listener";
        device = <&my_cursor_pipeline>;
    };
};

&my_physical_input_listener {
    input-processors = <&my_cursor_pipeline>;
};
```

`source-id` and `stable-id` are metadata identities, not boot-time registration
indexes. `resolution-cpi` must match the sensor's compiled/current setting.
`swap-xy`, `invert-x`, and `invert-y` describe physical mounting. Keep stable
identities unchanged when later profiles and telemetry address pipeline
boundaries.

A transported source may additionally set `compact-event-code` to accept the
matched complete-frame format documented in
[`compact-split-codec.md`](compact-split-codec.md). Decoding occurs inside the
source boundary rather than as a generic X/Y expander, preserving packet
sequence and source-side span metadata.

## Current limits

- Only the fixed identity cursor composition is constructed.
- There is no layer or behavior router yet.
- Resolution normalization is available but not yet connected to a pointer
  mapper and quantizer in this identity cursor.
- There are no calibration, tuning, or observer stages.
- The Zephyr adapter does not schedule runtime stage deadlines because the
  identity stage is stateless.
- The Bridges configuration is not migrated by this slice.

The smoke firmware wires the output of the existing test noise device through
the identity processor and into a dedicated output listener. Host tests cover
frame reconstruction, saturation evidence, identity mapping, metadata
preservation, and overflow rejection.
