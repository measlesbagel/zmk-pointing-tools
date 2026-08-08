# Composable motion pipeline architecture

Status: accepted direction; the shared runtime, source/router ZMK boundary,
ordered devicetree stage references, explicit sink selection, and layer routing
are implemented. Explicit behavior routing and observer discovery remain
provisional.

This document defines the intended architecture for issue
[#36](https://github.com/measlesbagel/zmk-pointing-tools/issues/36). It is a
design contract for subsequent incremental work, not documentation of the
current firmware implementation.

## Goals

- Treat local and split pointing devices as first-class input sources.
- Reconstruct complete, time-aware two-dimensional motion once.
- Build named behaviors from reusable, ordered, typed stages.
- Reuse gating, smoothing, axis intent, and transfer algorithms before any
  compatible output mode.
- Keep terminal sinks limited to output adaptation.
- Give stateful stages explicit activation, reset, flush, and retuning
  lifecycle.
- Run the same pure C pipeline and algorithms in firmware and host replay.
- Make tuning and telemetry describe pipeline structure rather than
  monolithic semantic processors.
- Preserve raw evidence while supporting device-independent normalized units.
- Avoid a virtual Zephyr input device between every internal stage.

## Non-goals

- Replacing ZMK's split transport.
- Hiding board wiring, sensor mounting, or selected CPI in reusable code.
- Allowing arbitrary stage combinations without type or ordering validation.
- Maintaining compatibility with the current experimental firmware protocol,
  devicetree bindings, profiles, or trace schema.
- Selecting the final smoothing, acceleration, or compact split-codec
  algorithm before trace-backed evaluation.
- Requiring runtime persistence; compiled Git-tracked configuration remains
  authoritative.

## Accepted decisions

The exploratory review established these initial decisions:

1. Ingress preserves raw counts and evidence; an explicit stage produces
   normalized fixed-point motion.
2. Routing supports both active-layer selection and explicit behavior-driven
   selection.
3. Pipelines use explicit ordered stage references. Convenience presets may
   expand to that representation later.
4. Transport batching and semantic output batching are separate operations.
5. Strategies are selected at compile time initially; runtime tuning changes
   validated numeric and boolean settings only.
6. Compact split transport is optional and matched at both ends. Its initial
   packet allocation is trace-backed and documented in
   [`compact-split-codec.md`](compact-split-codec.md).
7. Sinks are thin terminal adapters. Interpretation, accumulation,
   quantization, and scheduling happen in preceding stages.

## Research constraints

The architecture follows from several constraints in the underlying input
stack.

### Zephyr frames are scalar event sequences

A Zephyr `input_event` carries one event code and value. Multi-axis devices
mark the last event in a stable set with `sync`. The event has no timestamp.
Complete X/Y reconstruction and a timestamp therefore have to be supplied at
an ingress boundary.

See [Zephyr input events](https://docs.zephyrproject.org/latest/services/input/index.html#input-events).

### ZMK input processors are event-oriented

ZMK invokes an input processor once per scalar event. The processor can mutate
that event and return continue or stop. The standard API has no generic vector,
activation, reset, flush, delayed-emission, or output callback contract.

Layer-specific input-listener overrides are also selected independently for
each event. A processor is not notified when its layer becomes inactive.

See [ZMK input processors](https://zmk.dev/docs/keymaps/input-processors) and
[input processor usage](https://zmk.dev/docs/keymaps/input-processors/usage).

### Split input forwards events, not motion frames

On a peripheral, `zmk,input-split` optionally applies processors and forwards
each resulting Zephyr event. The central recreates those events on a proxy
input device. Source capture time, frame sequence, and coalescing span are not
part of the standard packet.

See [ZMK split pointing devices](https://zmk.dev/docs/hardware-integration/pointing#listener-and-input-split-device).

### Current workarounds expose missing lifecycle

The existing processors independently reassemble X/Y, own unrelated timers,
observe key activity separately, and emit through virtual devices. The legacy
pipeline switch delegates scalar events but cannot reset arbitrary processor
state when a mode changes. More stages built this way would multiply event
boundaries and stale-state risks.

## Terminology

### Source

A physical or proxied input device together with immutable identity and
runtime metadata such as CPI. Examples are a local PAW3222 or the central-side
proxy for a split peripheral PAW3222.

### Source adapter

The boundary that turns source-specific Zephyr events into canonical motion.
It owns frame reconstruction and uses reusable transport, orientation, and
calibration mechanisms configured by the board or keymap.

### Router

The component that selects one named pipeline for a source and provides
explicit enter/leave lifecycle. Selection can follow active ZMK layers or an
explicit behavior. Automatic mode handling may become another controller of
the router later.

### Pipeline

A named ordered list of stage instances followed by one sink. Examples are
`right-cursor`, `right-scroll`, `right-text`, `right-precision`, and
`left-scroll`.

### Stage

An independently useful and ordering-sensitive operation over a typed signal.
A stage owns per-pipeline runtime state. Examples include a motion gate,
smoother, axis-intent estimator, constraint, transfer function, quantizer, and
batcher.

### Strategy

An interchangeable implementation of one stage contract. One Euro and
heading-aware smoothing are smoother strategies. Piecewise and power curves
are acceleration strategies. Strategies are selected at compile time in the
initial architecture.

### Policy

One configurable condition or rule within a stage. Keypress suppression,
activation displacement, and directional coherence are motion-gate policies,
not separate semantic modes.

### Sink

A terminal adapter that encodes an already-decided typed result as Zephyr,
ZMK, or HID-facing output. A sink does not infer intent, apply gain, accumulate
fractional motion, or choose output cadence.

### Observer

A cross-cutting consumer of pipeline events that does not modify the signal.
Tracing, semantic telemetry, and a future automatic-layer controller are
observers.

## Ownership boundaries

### Reusable library

The pointing library owns:

- complete X/Y frame assembly;
- canonical frame and signal types;
- source metadata contracts;
- generic axis swap, inversion, and arbitrary rotation;
- calibration and resolution-normalization stages;
- optional split frame codecs and matching decoders;
- source and output coalescing mechanisms;
- router and pipeline lifecycle;
- stage and sink interfaces;
- algorithms, tuning, telemetry, and host replay;
- diagnostics for malformed, clipped, estimated, or missing data.

### Board firmware

The board repository owns:

- sensor bus and interrupt wiring;
- concrete input and split-proxy devices;
- source identifiers and split `reg` values;
- physical mounting orientation values;
- supported CPI values and sensor-control bindings;
- selection and configuration of an optional transport codec;
- safe board-specific transport defaults.

### Personal configuration

The user configuration owns:

- named pipelines and routing rules;
- enabled stages and selected strategies;
- mode-specific tuning values;
- semantic behavior bindings;
- runtime-preview values copied back from the tuner.

### ZMK and Zephyr

ZMK continues to own split transport, layers, behaviors, endpoints, and HID
reporting. Zephyr continues to own sensor input devices and scalar input
events. The library adapts at those boundaries rather than replacing them.

## Data flow

### Local source

```text
sensor events
  -> frame assembler
  -> source orientation/calibration
  -> canonical motion
  -> router
  -> selected pipeline
  -> sink
```

### Split source

```text
peripheral sensor events
  -> optional transport coalescer
  -> optional compact encoder
  -> ZMK input split
  -> central proxy events
  -> matching decoder/frame assembler
  -> source orientation/calibration
  -> canonical motion
  -> router
  -> selected pipeline
  -> sink
```

Transport coalescing and output batching are distinct. Transport coalescing
reduces split traffic while preserving source distance and sample span. Output
batching controls semantic report cadence after filtering and conversion.

## Canonical motion and units

Raw evidence and normalized processing must remain distinguishable.

### Raw frame

A raw frame preserves reconstructed sensor counts and source evidence. At
minimum it needs conceptually equivalent fields to:

```c
struct raw_motion_frame {
    int64_t x_counts;
    int64_t y_counts;
    uint32_t observed_at_ms;
    uint32_t sample_span_us;
    uint16_t source_id;
    uint16_t sequence;
    uint16_t resolution_cpi;
    uint32_t flags;
};
```

`observed_at_ms` is central arrival time unless a source or codec provides
better information. `sample_span_us` records the motion interval when known and
may be estimated from configuration. `resolution_cpi` is the source's current
nominal counts-per-inch setting, not a calibrated gain.

Useful flags include:

- local or transported;
- coalesced;
- source time or span estimated;
- clipped or saturated;
- sequence gap;
- malformed or incomplete frame;
- source disconnect/reconnect boundary.

### Normalized motion

Ordered orthogonal-orientation and resolution-normalization stages produce
fixed-point displacement without overwriting the source raw trace. Calibration
remains a separate optional transform so nominal sensor resolution, measured
device correction, and semantic gain are never conflated. Algorithms that
compare physical speed or use device-independent distance should consume
normalized motion.

Normalized motion uses signed Q16 millimetres. This representation:

- retains fractional movement across stages;
- covers realistic coalesced sensor deltas without overflow;
- avoids floating-point requirements in firmware;
- converts deterministically in host and firmware;
- exposes understandable physical units to the tuner.

Algorithms may explicitly consume raw motion when quantization at low CPI is
part of the intended policy. This must be declared rather than inferred.

## Signal domains

Pipelines are typed. A stage declares accepted and produced signal kinds so a
configuration cannot, for example, put a motion smoother after text actions.

Initial domains to validate are:

| Domain | Meaning |
| --- | --- |
| raw motion | Reconstructed source counts and evidence |
| normalized motion | Q16 nominal or calibrated two-dimensional millimetres |
| pointer delta | Continuous host-pointer displacement |
| scroll delta | Continuous logical two-dimensional scroll displacement |
| scroll steps | Integral horizontal/vertical wheel steps |
| action | Directional or bound ZMK behavior action |

Metadata and annotations accompany a signal without silently changing its
domain. Examples include current speed, axis intent, confidence, gate phase,
and source quality flags.

## Stage categories

One stage API can support several conceptual categories:

| Category | Role | Examples |
| --- | --- | --- |
| transform | Change values without semantic conversion | orientation, calibration, constant gain |
| gate | Pass, buffer, or discard | keypress guard, movement qualification |
| filter | Modify a signal over time | adaptive smoother |
| estimator | Add reusable metadata | velocity, axis intent |
| constraint | Modify values using metadata or fixed policy | free, horizontal, vertical, adaptive axis lock |
| mapper | Change signal domain | motion to pointer, motion to scroll, direction to action |
| quantizer | Convert continuous values to discrete output with remainder | pointer counts, wheel steps, text steps |
| batcher | Coalesce output and request a deadline | report cadence |
| observer | Record or react without modifying | trace, telemetry, automatic layer candidate |

### Granularity rule

A capability is a separate stage when it is independently useful, stateful,
tunable, or meaningfully order-sensitive. Interchangeable implementations of
the same input/output contract are strategies. Conditions that jointly make
one pass/buffer/drop decision are policies inside one gate.

For example:

- keypress suppression is a gate policy;
- accumulated displacement and coherence are gate policies or a gate strategy;
- ratio/hysteresis is an axis-intent strategy;
- axis intent itself is an estimator stage;
- suppressing the minor axis is a separate constraint stage;
- One Euro is a smoothing strategy;
- report interval is a batcher setting, not a sink behavior.

## Axis intent

Axis intent is semantic-neutral.

The estimator reads motion history and annotates a signal with at least:

- undecided;
- free;
- horizontal;
- vertical;
- optional confidence.

It does not have to discard the minor axis. A following constraint stage can
preserve both axes, remove the minor axis, lock to a fixed axis, or project
motion onto an estimated axis.

This supports:

- scroll direction cleanup;
- text-navigation direction selection;
- optional cursor straight-line or constrained-drag behavior;
- potential heading-aware smoothing without duplicating classification.

## Thin sinks

Sinks receive final output-domain signals.

### Cursor sink

- Accept integral pointer deltas.
- Encode synchronized `REL_X` and `REL_Y` events.
- Report output failures.

### Scroll sink

- Accept integral horizontal and vertical scroll steps.
- Encode synchronized `REL_HWHEEL` and `REL_WHEEL` events.
- Expose host high-resolution-scroll capability to upstream output adaptation.
- Report output failures.

### Action sink

- Accept an already-selected behavior action.
- Queue its required press/release sequence outside unsafe nested input stack
  contexts.
- Report queue failure.

Sinks may perform mandatory protocol packetization and validation. They do not
choose motion thresholds, direction, scale, fractional accumulation, cooldown,
or report cadence. An upstream packetizer should normally split oversized
output instead of relying on sink clipping.

## Example pipelines

### Cursor

```text
normalized motion
  -> motion gate
  -> adaptive smoother
  -> velocity estimator
  -> gain/acceleration
  -> pointer mapper
  -> pointer quantizer
  -> optional output batcher
  -> cursor sink
```

### Scroll

```text
normalized motion
  -> motion gate
  -> optional smoother
  -> axis-intent estimator
  -> optional axis constraint
  -> scroll mapper/transfer
  -> scroll-step quantizer
  -> output batcher
  -> scroll sink
```

### Text navigation

```text
normalized motion
  -> optional motion gate
  -> optional smoother
  -> axis-intent estimator
  -> optional axis constraint
  -> distance-to-step quantizer
  -> direction/action mapper
  -> action sink
```

### Precision cursor

Precision is a cursor pipeline preset, not a sink type:

```text
normalized motion
  -> motion gate
  -> stronger smoother
  -> reduced or flat gain
  -> pointer mapper
  -> pointer quantizer
  -> cursor sink
```

## Stage lifecycle

The host and firmware core need conceptually equivalent operations to:

```text
activate(context)
push(signal, emit)
flush(now, emit)
deactivate(reason, emit-or-discard-policy)
reset(reason)
retune(candidate-settings)
```

A stage can emit zero, one, or multiple outputs. It can request a future flush
deadline. Delayed output resumes at the next stage rather than restarting the
pipeline.

Initial reset reasons are:

- initialization;
- idle boundary;
- pipeline entered;
- pipeline left;
- source disconnected or reconnected;
- transport discontinuity;
- keypress or other external suppression;
- runtime settings changed;
- settings reset to compiled defaults;
- explicit administrative reset.

Each stage documents which state survives each reason. Fractional output
remainder may survive ordinary idle but must not cross an incompatible
strategy change. Pending gate motion should not cross a pipeline change.

## Pipeline runtime

The runtime is statically allocated, bounded, deterministic, and heap-free.
It owns:

- the ordered stage table;
- per-stage state and settings;
- downstream emission routing;
- bounded zero/one/many fan-out;
- the nearest requested deadline;
- flush and reset propagation;
- activation state;
- observer dispatch.

The core router applies the same constraints across named pipelines. It owns
their validation and enter/leave lifecycle, delegates input and deadlines only
to the selected pipeline, and remains independent of the ZMK layer or behavior
policy that chooses a route.

The host implementation uses the same core source files and integer arithmetic
as firmware. Zephyr adapters provide clocks, work scheduling, keypress/layer
signals, and output integration around that core. The first adapter hosts frame
ingress in a standard ZMK input-listener processor chain and emits cursor
events from a virtual device to a dedicated output listener.

## Routing and mode lifecycle

The initial router supports both:

1. layer-based routes; and
2. explicit behavior-selected routes.

Layer routing is suitable for momentary text, scroll, and precision modes.
Explicit selection supports persistent or manually selected device roles.
Precedence must be deterministic and visible to telemetry.

The router observes layer changes directly rather than relying only on ZMK's
per-event input-listener override. On a route transition it:

1. deactivates the outgoing pipeline with a configured flush/discard policy;
2. clears deadlines owned by that pipeline;
3. activates or resets the incoming pipeline;
4. publishes the route transition;
5. sends the next source frame only to the incoming pipeline.

Automatic layer handling remains separate. A future controller can observe
qualified activity at a named boundary and request layer or route changes
without being embedded in the cursor sink.

## Source and split transport design

### Standard split path

The library must support ordinary X/Y events forwarded by `zmk,input-split`.
The central adapter reconstructs frames and marks source timing as observed or
estimated according to available evidence.

### Optional compact codec

Compact transport is an optimization, not the canonical source contract. The
encoder and decoder form a matched pair around ZMK split input.

The codec design must evaluate:

- signed axis range under worst-case CPI and coalescing interval;
- exact distance preservation and overflow backlog;
- sequence bits for drop/reorder detection;
- representation of empty or one-axis frames;
- frame synchronization and malformed payload handling;
- whether a source-time delta is worth additional bits or packets;
- version mismatch detection without compatibility parsing;
- BLE traffic savings versus native split events.

The current codec uses exact signed 11-bit axes, source-side span, a rolling
sequence, and a current-format guard. The previous Bridges sign-extension
repair becomes unnecessary because the codec owns signed decoding explicitly.

### Source versus output batching

Source batching/coalescing:

- occurs before semantic processing;
- reduces transport or sensor report frequency;
- preserves summed source motion;
- records the covered sample span;
- must not apply semantic scale.

Output batching:

- occurs after semantic conversion;
- controls host report cadence;
- preserves fixed-point remainder or discrete actions according to domain;
- must not be mistaken for source sampling time.

Both can appear in one pipeline but remain independent stages with separate
telemetry.

## Configuration model

The initial devicetree model uses explicit ordered phandle lists. Names below
are illustrative, not accepted bindings:

```dts
right_cursor: right_cursor {
    compatible = "measlesbagel,zpt-pipeline";
    stable-id = "right-cursor";
    stages = <&right_gate>,
             <&right_smoother>,
             <&right_velocity>,
             <&right_gain>,
             <&right_pointer_quantizer>;
    sink = <&zpt_cursor_sink>;
};

right_router: right_router {
    compatible = "measlesbagel,zpt-router";
    source = <&right_motion_source>;
    default-pipeline = <&right_cursor>;

    text-route {
        layers = <TEXT>;
        pipeline = <&right_text>;
    };

    precision-route {
        layers = <SNIPER>;
        pipeline = <&right_precision>;
    };
};
```

Every stateful stage reference is an instance, not shared mutable state. Two
pipelines can use the same strategy implementation and defaults but must have
independent state unless a future stage explicitly declares safe shared state.
Initialization rejects duplicate use of a stateful instance.

Convenience includes or macros can provide common presets later. The explicit
model remains the source of truth and the tuner should display the expanded
pipeline.

## Strategy and runtime tuning

The first implementation selects strategies at compile time and tunes numeric
or boolean parameters at runtime. This keeps state shape, validation, and
memory allocation static.

Changing a strategy requires a rebuild. A later runtime strategy selector is
only justified if hardware testing demonstrates value and the pipeline can
reset it safely.

Runtime updates are applied atomically per stage:

1. validate the complete candidate settings;
2. reject the entire update on failure;
3. swap settings under the stage's synchronization policy;
4. reset only the state invalidated by that change;
5. publish a retuning event.

Profiles address values by pipeline and stage stable identity rather than
boot-time registration order.

## Telemetry and tracing

Observers attach to named boundaries such as:

- source raw;
- source normalized;
- after gate;
- after smoothing;
- after intent and constraint;
- before sink;
- sink output.

A telemetry record carries pipeline, stage/boundary, source, sequence, signal
kind, timestamp, and relevant strategy state. Strategy-specific state is
described by metadata rather than occupying one monolithic ten-value layout.

Tracing must remain optional and bounded. Dropped observer records never block
or alter motion processing.

## Current responsibility migration

| Current component | Responsibilities to extract | Destination |
| --- | --- | --- |
| noise filter | frame assembly, key activity, qualification, virtual output, tuning, telemetry | ingress, external context, motion-gate stage, sink/runtime, generic observers |
| scroll processor | frame assembly, key suppression, intent, constraint, transfer, remainder, timer, wheel output | ingress, motion gate, intent estimator, axis constraint, scroll transfer, quantizer, batcher, thin scroll sink |
| text navigation | frame assembly, gesture intent, distance accumulation, direction mapping, behavior queue | ingress, intent estimator, text quantizer, action mapper, thin action sink |
| sign-extend processor | packed-value interpretation | matched split codec decoder |
| trace processor | event-stage observation | boundary observer |
| pipeline switch | scalar delegation and selection | router with explicit pipeline lifecycle |
| report-rate limiter | per-axis event coalescing | frame-aware source or output batcher, depending on placement |

Migration removes duplicated generic keypress observation and axis-intent
logic. Semantic behavior remains reproducible through composed stages.

## Validation strategy

### Host tests

- Stage contract and type compatibility.
- Activation, deactivation, reset, retune, and deadline lifecycle.
- Zero/one/many emission and bounded fan-out.
- Fixed-point overflow, remainder, and deterministic rounding.
- Source frame reconstruction with partial, repeated, and malformed axes.
- Standard and compact split transport fixtures.
- Pipeline parity for current cursor, scroll, and text traces.
- Strategy-independent telemetry boundaries.

### Firmware smoke tests

- Devicetree generation and stage-order validation.
- One identity cursor path through ZMK ingress and thin sink.
- Layer and behavior route transitions.
- Timer cancellation and source disconnect.
- USB and BLE HID output boundaries.
- Split peripheral encode and central decode.

### Hardware evaluation

- Confirm no cursor/semantic leakage when a sink changes type.
- Compare local and split distance, cadence, and latency.
- Verify no stale movement after route changes or keypress suppression.
- Compare current and migrated scroll/text behavior before tuning changes.
- Measure dropped transport, telemetry, and output records.

## Incremental delivery plan

Each milestone is a separate reviewable pull request or small series. Existing
firmware remains usable until a migrated pipeline passes replay and hardware
parity.

1. **Architecture:** land this document and resolve review comments.
2. **Host runtime:** add signal types, pipeline execution, lifecycle, fake
   stages, and fake sinks without Zephyr.
3. **Minimal ZMK boundary:** add one ingress adapter and identity cursor
   pipeline with a thin cursor sink.
4. **Source normalization:** add orientation, source metadata, fixed-point
   normalization, and native split reconstruction.
5. **Transport codec:** evaluate and implement the optional matched compact
   encoder/decoder with diagnostics. The reusable codec and initial ZMK
   endpoints are implemented; Bridges migration remains gated on composed
   scroll parity.
6. **Motion gate migration:** the coherent-displacement strategy and normalized
   stage are implemented, and the legacy adapter reuses the same strategy.
   Remove its independent virtual-device plumbing only after firmware routing
   reaches parity.
7. **Scroll migration:** compose intent, constraint, transfer, quantizer,
   batcher, and thin scroll sink with parity fixtures.
8. **Text migration:** compose intent, text quantization, action mapping, and
   thin action sink with parity fixtures.
9. **Cleanup:** remove superseded processors and transitional dependencies.
10. **New algorithms:** evaluate adaptive smoothing and cursor acceleration
    only on the established architecture.

## Host prototype decisions and remaining questions

The host-only runtime fixes these representation details:

- continuous domains use signed 64-bit values with 16 fractional bits;
- signals use a tagged union with shared source metadata and annotations;
- every push, flush, or deactivation has a configured dispatch budget;
- each stateful stage can own one wrap-safe 32-bit uptime deadline;
- stage output is synchronous, streaming, and can contain zero, one, or many
  signals;
- stages receive explicit activation, deactivation, flush, and reset reasons;
- runtime validation checks signal compatibility, stage ownership, and
  duplicate state within a pipeline.

See [`pipeline-runtime.md`](pipeline-runtime.md) for the prototype contract.

Normalized motion uses Q16 millimetres. The standard ZMK input listener is the
accepted source-specific ingress host. One source processor remains attached
there while a separate router observes ZMK layer events and applies complete
pipeline lifecycle rather than using per-event processor overrides. These
questions remain for later measured slices:

- buffered-output policy for each production stage and reset reason;
- which type errors devicetree can catch at build time versus initialization;

These questions do not change the thin-sink, typed-stage, explicit-lifecycle,
or source-ownership decisions in this document.
