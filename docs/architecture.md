# Architecture

## Goals

ZMK Pointing Tools separates four concerns that are currently commonly mixed
inside keyboard board definitions:

1. A sensor driver reads physical hardware and emits relative input frames.
2. Transport processors efficiently move peripheral frames to a split central.
3. Semantic processors interpret complete vectors as cursor, scroll, text, or
   other output according to state owned by the central.
4. An optional host service introspects and temporarily tunes those processors.

Final policy and compiled defaults remain in the consuming ZMK configuration.

## Split data path

```text
peripheral sensor
  -> fixed hardware correction
  -> frame-aware coalescing/transport
  -> central input-split proxy
  -> semantic processing using central layer state
  -> ZMK HID

central sensor
  -> fixed hardware correction
  -> semantic processing using central layer state
  -> ZMK HID
```

Sensor-local operations such as changing CPI are invoked locally or relayed
from the central. User-facing mode decisions remain on the central so both
halves observe the same keymap state.

## Processor model

Processors should preserve complete X/Y frames for any operation that needs
directional intent. Initial processor families are expected to include:

- coalescing with an idle flush timer;
- vector filtering and axis-intent classification;
- synchronized scroll mapping and fractional scaling;
- movement-to-behavior output;
- optional automatic-layer activation with accumulated-motion thresholds.

Forced vertical and horizontal operation remain available, but adaptive
gesture-level axis locking is the intended default.

## Runtime model

Compiled values are the authoritative baseline. Runtime changes are temporary
by default and return to the compiled baseline on reset. Persistence is an
explicit optional operation rather than an automatic side effect of moving a
slider.

The host protocol should describe pointing devices and capabilities rather than
hard-code left/right trackballs. A transport-independent runtime registry will
serve Web Serial initially and may support other transports later.

The first registry implementation assigns session-local target IDs during boot
and exposes typed parameter metadata, compiled baselines, current values,
validated temporary updates, and reset operations. Persistence is intentionally
absent from protocol version 2 so experimentation cannot cause hidden flash
writes or make the repository disagree with keyboard behavior.

Parameter explanations are owned by the processor and fetched on demand in
protocol version 3. Host tools therefore remain generic as new processor types
and settings are added.

## Telemetry

Telemetry is opt-in and active only during a tuning session. It should expose
enough staged information to compare raw, filtered, classified, and emitted
movement without permanently consuming split bandwidth.

Recorded traces should be exportable for deterministic processor regression
tests. Experimental gesture classifiers, including motion-derived tap-to-click,
must remain disabled by default.
