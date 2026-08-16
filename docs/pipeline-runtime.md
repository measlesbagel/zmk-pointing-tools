# Motion pipeline runtime

The pipeline runtime is the shared C implementation of the architecture in
[`architecture.md`](architecture.md). Host tests exercise the complete runtime
without Zephyr, and the firmware adapter exercises it through the composed
pipelines documented in
[`zmk-pipeline-boundary.md`](zmk-pipeline-boundary.md).

## Signals

`signal.h` defines tagged signal domains for raw motion, normalized motion,
pointer deltas, discrete scroll steps, and actions. Each signal carries source
and timing metadata plus reusable annotations.

Raw motion preserves signed sensor counts. Continuous processed domains use a
signed 64-bit fixed-point value with 16 fractional bits. One normalized-motion
unit is one millimetre; later semantic mappers define pointer and scroll units.
The representation is fixed so host and firmware use identical integer
arithmetic. The complete tagged signal has a compile-time maximum size of 64
bytes so stages can copy and buffer it without allocation.

Source metadata includes stable identity, sequence, current CPI, observation
time, and optional sample span. Flags distinguish local, transported,
coalesced, estimated, clipped, gapped, malformed, and discontinuous input.
Stages copy or deliberately amend this evidence rather than reconstructing it
independently.

## Pipeline validation

A pipeline declares one input kind, an ordered array of references to stage
instances, one sink, and a dispatch budget. References preserve each stage's
identity and ownership when configuration systems such as devicetree compose
a pipeline from independently allocated instances. Stage APIs identify their
strategy and sink APIs identify their output type independently from instance
stable IDs. Validation checks:

- stable identities and required callbacks;
- type compatibility at every boundary;
- unique stage/sink identities;
- state storage for stateful stages;
- unique stage state and sink ownership;
- compatibility between the last stage and the sink.

Validation claims stage and sink instances for one pipeline and initializes
them. The same stage or sink instance cannot be reused by another pipeline,
and one pipeline cannot assign the same state allocation to multiple stages.
Construction code must give distinct objects in different pipelines distinct
state allocations. Algorithm APIs and immutable configuration may still be
shared.

## Streaming execution

`zpt_pipeline_push()` sends one signal through the first stage. A stage calls
`zpt_stage_emit()` zero, one, or multiple times; each emission resumes at the
next stage. The final compatible signal reaches the sink.

Emission is synchronous and streaming. Earlier sink output is not rolled back
if a later emission fails. A stage must validate an atomic group before its
first emission when partial output would be invalid.

Every push, flush, or deactivation has a bounded dispatch budget. Each stage
visit and sink visit spends one entry. Exhaustion returns `-E2BIG`, preventing
an accidental fan-out cycle or unbounded multi-output stage from monopolizing
the input thread.

## Delayed output

A stateful stage with a flush callback can request one absolute 32-bit uptime
deadline. The runtime exposes the nearest pipeline deadline to the Zephyr
executor. When due, the stage's flush callback resumes output after that stage;
it does not restart the pipeline.

Deadline comparisons follow signed 32-bit uptime-difference semantics and are
covered across timestamp wrap. Firmware stage settings must remain well below
half the uptime range.

The host runtime does not own a thread or clock. Tests call
`zpt_pipeline_flush()` explicitly; the Zephyr executor supplies uptime and
delayable work around the same core call.

## Lifecycle

Pipelines are validated, then explicitly activated before accepting input.
Activation resets stage and sink state before invoking activation callbacks.

Deactivation visits stages in input-to-output order. This permits an upstream
stage to release buffered output into downstream stages before those stages
apply their own flush-or-discard policy. Deadlines are canceled, the sink is
deactivated, and all state receives the final reset reason.

Reset reasons distinguish initialization, idle, route entry/exit, source
connection changes, transport discontinuity, external suppression, retuning,
default restoration, and administrative reset. Individual production stages
will document which values they preserve for each reason.

## Router runtime

The semantic-neutral router owns an ordered set of compatible pipelines and
activates exactly one at a time. Validation claims every pipeline, requires a
shared input signal kind, and rejects duplicate pipeline identities. A route
change deactivates the outgoing pipeline with `PIPELINE_LEFT`, clearing its
deadlines and buffered state, before activating the incoming pipeline with
`PIPELINE_ENTERED`.

Push, flush, and nearest-deadline queries delegate only to the active pipeline.
Selecting the current pipeline is a lifecycle no-op. If incoming activation
fails after the old route was left, the router rolls back to the previously
active pipeline so input remains handled; stale input is never sent through a
pipeline that has already been deactivated.

## Current stages

The cursor transfer stage applies a base gain to Q16 millimetre motion,
saturating at the fixed-point limits with a clipped flag, and the cursor
quantizer converts normalized motion to integer-valued pointer deltas while
carrying the fractional remainder between frames so sub-pixel movement
accumulates exactly. With a units-per-meter factor derived from the sensor
CPI and identity gain, the composed path reproduces the count-domain cursor
behavior within one sub-count unit.

Stages carry an optional observer slot that receives generic decision
events (suppression, discard, qualification, intent changes, flushes, and
actions) without coupling algorithms to any telemetry transport. A ZMK
pipeline-telemetry device attaches observers to every stage of its configured
pipelines, allocates a state-telemetry target per stage, and reports decisions
as state samples; stages remain discoverable by stable id through the lookup
API.

Most host tests intentionally use local fake stages and sinks rather than
shipping premature production algorithms. They prove:

- raw-to-normalized-to-pointer typed flow;
- source metadata preservation;
- incompatible stage rejection;
- duplicate state rejection;
- zero and multiple output behavior;
- dispatch-budget enforcement;
- delayed output and 32-bit clock wrap;
- deactivation emission followed by reset;
- rejection of a stage emitting a kind outside its contract.

Production stages provide counts-per-inch normalization to Q16 millimetres,
coherent-displacement motion gating with buffered evidence and idle or
qualification deadlines, the windowed-energy axis-intent estimator carrying
intent, confidence, and speed annotations, the axis-constraint stage
buffering undecided motion and applying the intent annotation, the scroll
batcher transferring steps per millimetre with a fractional remainder on a
report deadline, the text-navigation stage emitting actions from threshold
crossings, and the cursor transfer and sub-pixel quantizer stages. Every
motion stage after the ingress consumes normalized Q16 millimetre motion.
The scroll stages accept a shared suppression policy so a single ZMK
keypress guard participates in the pass, buffer, and drop decisions of the
whole pipeline.
The ZMK adapter uses the standard input listener as its source-specific
processor host and feeds reconstructed frames to a separate router device.
Pipelines, their ordered stages, and cursor sinks are independently allocated
devicetree providers. A Zephyr router executor serializes source pushes, route
changes, and delayable-work flushes while rescheduling the active pipeline's
nearest requested stage deadline after every operation.
