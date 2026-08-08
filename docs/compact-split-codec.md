# Compact split motion codec

The compact split codec is an optional matched adapter around ZMK's existing
input-split transport. It sends one complete signed X/Y source frame in one
ordinary split input event while preserving source-side cadence and enough
sequence evidence to identify common packet loss.

It does not replace ZMK's BLE or wired transport, normalize resolution, apply
semantic gain, or provide a compatibility parser for old packet formats.

```text
peripheral sensor X/Y events
  -> optional source coalescer
  -> compact encoder
  -> one configured relative input event
  -> ZMK input split
  -> central proxy
  -> pipeline ingress compact decoder
  -> canonical raw motion and transport diagnostics
```

## Trace-backed range

The initial allocation was selected from the Bridges PAW3222 captures rather
than from its previous 16-bit packing convention:

| Capture | Cadence | Largest repaired absolute axis value |
| --- | ---: | ---: |
| fast uncoalesced cursor | 8–9 ms | 127 counts |
| left/right repeated scroll, left side | about 15–23 ms | 45 counts |
| historical failing left transport after signed repair | about 15–30 ms | 112 counts |

Each encoded axis is signed 11-bit, covering `-1024..1023` counts. This is
eight times the PAW3222's observed single-report limit and more than nine times
the largest captured split frame. The range is not a generic promise for every
sensor or coalescing interval: a board must choose a transport interval that
fits its source, CPI, and driver range.

## Current packet

The 32-bit input-event value is allocated as follows:

```text
31       30 29       26 25       22 21            11 10             0
+----------+-----------+-----------+----------------+----------------+
| format 2 | span code | sequence  | signed Y (11)  | signed X (11)  |
+----------+-----------+-----------+----------------+----------------+
```

- X and Y use exact two's-complement counts.
- Sequence is modulo 16. The decoder expands normal wrap and marks a gap or
  ambiguous duplicate as a discontinuity.
- Span code zero means unknown, codes 1–14 represent source-side elapsed time
  in 2 ms increments, and code 15 is the saturated 30 ms lower bound.
- The two-bit current-format tag rejects obvious incompatible or malformed
  packets. It selects only the latest format; there is no legacy decoder.

The peripheral adapter estimates sample span from elapsed time between complete
encoded frames, before BLE delivery jitter. The estimate is explicitly marked
`TIMING_ESTIMATED`. A saturated span also sets `SAMPLE_SPAN_CLIPPED`.

Zero motion and one-axis motion are valid complete frames. Negative values are
decoded as signed values by the codec itself, so no later sign-extension repair
is needed.

## Distance and overflow policy

Every in-range packet round-trips exact signed distance. The encoder never
clips an axis into the wire range.

An out-of-range frame is rejected and counted. Its sequence number is still
consumed, causing the next successful packet to carry visible gap and
discontinuity evidence. Retaining overflow as a delayed backlog was rejected
for this format: stale motion would acquire misleading timing and latency, and
the available traces do not justify that complexity. A source that can exceed
the range should reduce source coalescing, use native X/Y transport, or use a
future explicitly fragmented format.

The four-bit sequence can estimate up to 14 missing packets between received
packets. A zero modulo delta is ambiguous between a duplicate and loss of a
whole 16-packet cycle; it is marked as a discontinuity without claiming an
exact drop count. BLE notification order means reordering is not expected in
the normal ZMK path.

## Transport cost

ZMK's BLE split input payload is eight bytes for one scalar input event. Native
two-axis forwarding therefore uses two notifications and 16 payload bytes per
complete frame; this codec uses one notification and eight payload bytes. The
50% payload reduction excludes shared ATT/GATT overhead, but eliminating one
notification also avoids its per-notification overhead.

Source coalescing remains independent. The codec neither chooses its cadence
nor applies semantic scale.

## ZMK configuration boundary

The peripheral encoder is an ordinary zero-parameter input processor:

```dts
compact_encoder: compact_encoder {
    compatible = "measlesbagel,zpt-input-processor-compact-split-encoder";
    #input-processor-cells = <0>;
    event-code = <INPUT_REL_Z>;
};

&peripheral_input_split {
    device = <&trackball>;
    input-processors = <&source_coalescer 16>, <&compact_encoder>;
};
```

The matching decoder is configured on a transported central pipeline ingress
so decoded sequence and timing metadata enter the canonical source directly:

```dts
compact_cursor_pipeline: compact_cursor_pipeline {
    compatible = "measlesbagel,zpt-input-processor-pipeline";
    #input-processor-cells = <0>;
    stable-id = "left-cursor";
    source-id = <2>;
    resolution-cpi = <700>;
    transported;
    compact-event-code = <INPUT_REL_Z>;
    compact-transport-coalesced;
};
```

The encoder and decoder event codes must match. The code is board wiring, while
the packet implementation and diagnostics are reusable library behavior.

## Current integration limit

Host tests exercise signed boundaries, trace-observed values, exact distance,
span saturation, sequence wrap/gaps, format rejection, and overflow evidence.
The smoke firmware compiles both ZMK endpoints. Bridges still uses its existing
packed transport until the composed pipeline can replace the current scroll
path without changing hardware behavior.
