# Minimal ZMK pipeline boundary

The first firmware integration of the composable runtime remains deliberately
an identity cursor path. Its ordered stages are now independently allocated
devicetree providers, validating explicit composition without changing motion
semantics.

```text
physical or proxied input events
  -> standard ZMK input listener
  -> source ingress input processor
  -> complete raw X/Y frame with source metadata
  -> layer-aware router
  -> selected named pipeline
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

Attach this source processor to the base listener, not to per-layer overrides.
It remains attached once while the separate router observes route changes
explicitly; using ZMK's per-event overrides as a router would reintroduce
incomplete-frame and missing-lifecycle problems.

## Identity stage and cursor sink

An orthogonal orientation stage first applies the configured axis swap and
inversions. The identity stage then maps those signed raw counts to whole Q16
pointer deltas without gain, filtering, or rounding. It exists to prove exact
cursor parity and will be bypassed by the normalized cursor composition once a
pointer mapper and quantizer are available.

The cursor sink accepts only whole pointer deltas within ZMK's signed 16-bit
HID movement range. It performs no scaling, clipping, accumulation, or cadence
selection. After validating both axes, it emits `REL_X` followed by a
synchronized `REL_Y` from the router's virtual input device.

## Devicetree wiring

```dts
/ {
    my_orientation: my_orientation {
        compatible = "measlesbagel,zpt-stage-orientation";
        stable-id = "orientation";
        invert-x;
        invert-y;
    };

    my_pointer_identity: my_pointer_identity {
        compatible = "measlesbagel,zpt-stage-raw-pointer-identity";
        stable-id = "raw-pointer-identity";
    };

    my_cursor_sink: my_cursor_sink {
        compatible = "measlesbagel,zpt-sink-cursor";
        stable-id = "cursor";
    };

    my_cursor_pipeline: my_cursor_pipeline {
        compatible = "measlesbagel,zpt-pipeline";
        stable-id = "right-cursor";
        stages = <&my_orientation &my_pointer_identity>;
        sink = <&my_cursor_sink>;
    };

    my_router: my_router {
        compatible = "measlesbagel,zpt-router";
        stable-id = "right-router";
        pipelines = <&my_cursor_pipeline>;
        default-pipeline = <&my_cursor_pipeline>;

        /* Additional routes reference other independently allocated pipelines:
         * precision-route {
         *     layers = <3>;
         *     pipeline = <&my_precision_pipeline>;
         * };
        */
    };

    my_precision_route: my_precision_route {
        compatible = "measlesbagel,zpt-behavior-route";
        #binding-cells = <0>;
        router = <&my_router>;
        /* Replace with another pipeline owned by my_router. */
        pipeline = <&my_cursor_pipeline>;
    };

    my_cursor_source: my_cursor_source {
        compatible = "measlesbagel,zpt-input-processor-source";
        #input-processor-cells = <0>;
        source-id = <1>;
        resolution-cpi = <700>;
        router = <&my_router>;
        /* Add for a central-side split proxy source. */
        /* transported; */
    };

    my_cursor_output: my_cursor_output {
        compatible = "zmk,input-listener";
        device = <&my_router>;
    };
};

&my_physical_input_listener {
    input-processors = <&my_cursor_source>;
};
```

`source-id` and each `stable-id` are metadata identities, not boot-time
registration indexes. `resolution-cpi` must match the sensor's
compiled/current setting.
The orientation stage's `swap-xy`, `invert-x`, and `invert-y` properties
describe physical mounting. Each stage phandle identifies one allocated
instance and cannot be shared by multiple pipelines. Keep stable identities
unchanged when later profiles and telemetry address pipeline boundaries.

Pipeline and sink nodes are also independently allocated providers. Pipeline
validation claims all referenced stages and its sink, so none of those mutable
instances can be shared with another pipeline. The router binds each sink's
platform output device once, validates all owned pipelines, and activates the
default route. The source ingress only reconstructs canonical frames and sends
them to that router.

A transported source may additionally set `compact-event-code` to accept the
matched complete-frame format documented in
[`compact-split-codec.md`](compact-split-codec.md). Decoding occurs inside the
source boundary rather than as a generic X/Y expander, preserving packet
sequence and source-side span metadata.

## Stage and sink providers

The current devicetree-backed providers are:

| Compatible | Input → output | State |
| --- | --- | --- |
| `measlesbagel,zpt-stage-orientation` | raw motion → raw motion | stateless |
| `measlesbagel,zpt-stage-raw-pointer-identity` | raw motion → pointer delta | stateless |
| `measlesbagel,zpt-stage-resolution-normalize` | raw motion → normalized motion | stateless |
| `measlesbagel,zpt-stage-coherent-displacement` | normalized motion → normalized motion | stateful |
| `measlesbagel,zpt-sink-cursor` | pointer delta → synchronized ZMK events | stateless |

The pipeline validates the expanded signal types and rejects an incompatible
order at initialization. The coherent-displacement provider expresses its
activation threshold as integer micrometres and converts it to the canonical
Q16 millimetre domain. It cannot yet terminate at the fixed cursor sink without
a normalized-motion pointer mapper, so it is present for composition and
deadline integration rather than the identity smoke path.

`dispatch-budget` defaults to 16 stage/sink visits per push, flush, or
deactivation. Increase it only when a deliberately fan-out-capable stage needs
more bounded dispatches.

## Firmware deadlines and concurrency

Each router is hosted by a ZMK executor that serializes source pushes, route
changes, and Zephyr work-queue flushes. After every operation it asks the active
pipeline for the nearest absolute deadline, reschedules one delayable work
item, and flushes the router when that deadline is due. A stage therefore
expires buffered state without requiring a later physical motion frame.

Resetting or deactivating a pipeline clears its core deadlines and cancels
pending work. A stale work callback that was already running is harmless: it
rechecks pipeline state while holding the same executor mutex. The adapter
requires Zephyr's threaded input mode so input processing never tries to take
the mutex from an interrupt context.

## Layer routing

Every `measlesbagel,zpt-router` lists its complete pipeline ownership set and a
default. Child route nodes map one or more stable ZMK layer IDs to a pipeline.
The router subscribes directly to `zmk_layer_state_changed`, so outgoing state
is deactivated and cleared as soon as a route changes rather than on the next
scalar motion event.

When multiple configured route layers are active, the route whose layer is
highest in the current ZMK layer order wins. If two route nodes name the same
layer, declaration order breaks the tie. If no configured route is active, the
default pipeline is selected.

## Explicit behavior routing

A `measlesbagel,zpt-behavior-route` node binds one router and one of its owned
pipelines. Using that zero-parameter behavior in a keymap selects the pipeline
on press and removes the override on release. Explicit overrides take
precedence over layer routes.

Overrides are tracked by key position rather than as one global flag. If
several route behaviors overlap, the most recently pressed wins; releasing it
reveals the next newest held override, then the current layer route. The fixed
per-router capacity defaults to four and is configured by
`CONFIG_ZMK_POINTING_TOOLS_ROUTER_MAX_EXPLICIT_ROUTES`. Persistent route
selection is intentionally left for a separate behavior policy.

## Current limits

- Sink providers cover cursor, scroll, and text actions; sink selection is
  explicit.
- Persistent explicit route selection is not implemented yet; the current
  behavior is momentary.
- Acceleration curves and report coalescing for the cursor path are future
  transfer-stage work.
- The Bridges configuration is not migrated by this slice.

The smoke firmware instantiates the composed cursor, scroll, and text
pipelines and their providers; the smoke router owns them selected by route
behaviors. The cursor path composes resolution normalization, the motion
gate, the cursor transfer gain, and the sub-pixel cursor quantizer,
terminating at the thin cursor sink; the cursor-identity-roundtrip fixture
proves the composed path reproduces the count domain within one sub-count
unit at 700 CPI. The scroll pipeline composes normalization, the
axis-intent estimator, the axis-constraint, and the steps-per-metre scroll
batcher with a thin wheel sink and a shared
`measlesbagel,zpt-keypress-suppression` guard, and the text pipeline composes
normalization and the text-navigation stage with an action sink invoking
bound keymap behaviors.

A `measlesbagel,zpt-pipeline-telemetry` device observes every stage of its
configured pipelines: each stage receives a state-telemetry target id and
reports suppression, discard, qualification, intent, flush, and action
decisions as state samples keyed by the stage's stable identity, while the
lookup API locates a stage by stable id without coupling the algorithms to
telemetry.

Host tests cover frame reconstruction, saturation evidence, identity mapping,
metadata preservation, and overflow rejection. The `composed-noise`,
`composed-scroll`, `composed-text`, and `cursor-pipeline` fixture kinds replay
traces through the composed stages and must produce byte-identical runner
output against the expectations captured from the superseded processors,
proving migration parity before their removal.
