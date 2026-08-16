# Source normalization

Source normalization turns source-specific scalar events into traceable raw
motion and then into a device-independent physical displacement. It remains
separate from cursor gain, scroll transfer, text thresholds, and per-device
calibration.

```text
local sensor or central-side split proxy events
  -> complete X/Y frame assembly
  -> source identity, timing, sequence, transport, and CPI metadata
  -> orthogonal mounting orientation
  -> resolution normalization
  -> Q16 millimetres
```

## Source metadata

Each raw signal carries:

- central observation time;
- known or estimated sample span;
- stable source identity and source-local sequence;
- current resolution in counts per inch;
- local or transported provenance;
- coalescing, clipping, timing, discontinuity, and transport evidence.

`zpt_motion_source_state` owns frame reconstruction, sequence allocation, and
the current CPI value. The CPI can be updated between complete frames without
rebuilding source state, providing the generic boundary needed by future
sensor discovery and split-routed controls. An update is rejected while motion
is pending so counts are never labelled with the wrong resolution. The current
ZMK devicetree adapter initializes it from `resolution-cpi`; no sensor driver is
queried or changed by this slice.

Exactly one of `LOCAL` and `TRANSPORTED` is required when a source is created.
Native ZMK split input needs no special canonical format: the central proxy's
ordinary scalar X/Y events pass through the same frame assembler and are marked
as transported. A compact packed codec remains an optional later transport
optimization.

## Orientation

The orthogonal orientation stage applies, in order:

1. optional X/Y swap;
2. inversion of the resulting logical X axis;
3. inversion of the resulting logical Y axis.

It preserves exact integer counts and all source metadata. This covers common
90-degree mounting orientations without fixed-point trigonometry. Arbitrary
rotation and measured cross-axis correction belong to later calibration work,
not the source event adapter.

## Normalized unit

One whole `NORMALIZED_MOTION` unit is one millimetre. Values use the runtime's
signed Q16 representation, so one stored unit is `1 / 65536 mm`.

Nominal displacement is calculated with the exact rational relationship:

```text
millimetres = counts × 25.4 / CPI
```

The implementation uses integer arithmetic equivalent to
`counts × 254 × 65536 / (10 × CPI)` and rounds to nearest, with half values
away from zero. It rejects unknown/zero CPI and values that would overflow
before multiplication rather than clipping silently.

For example:

- 700 counts at 700 CPI is nominally 25.4 mm;
- one count at 700 CPI is approximately 0.03629 mm;
- the same physical displacement at different CPI values reaches the same
  normalized value, subject only to source quantization and Q16 rounding.

Nominal CPI does not correct sensor variation, ball mechanics, mounting scale,
or user technique. Those factors remain explicit per-device calibration stages
and workflows under issue #23. Keeping them separate prevents transport
cadence or semantic gain from being applied twice.

## Current firmware integration

The smoke keymap executes the normalized compositions end to end. Every
pipeline starts from resolution normalization: the cursor pipeline feeds the
coherent-displacement gate, a unit cursor transfer, and the sub-pixel cursor
quantizer; the scroll pipeline feeds the axis-intent estimator, the
axis-constraint, and the scroll batcher; and the text pipeline feeds the
text-navigation stage directly. The compact-side path compiles the same
normalize, transfer, and quantizer stages against the transported source
contract without routing live motion through them.

Runtime CPI discovery/control, calibration recommendations, and cursor mapping
remain separate review layers. The optional compact transport codec is
documented in [`compact-split-codec.md`](compact-split-codec.md); its decoder
preserves source-side sequence and estimated span in this same raw source
contract.
