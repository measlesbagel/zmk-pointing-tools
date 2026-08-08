# Motion pipeline runtime

The pipeline runtime is the shared C implementation of the architecture in
[`architecture.md`](architecture.md). Host tests exercise the complete runtime
without Zephyr. The first firmware adapter uses it for the identity cursor path
documented in [`zmk-pipeline-boundary.md`](zmk-pipeline-boundary.md).

## Signals

`signal.h` defines tagged signal domains for raw motion, normalized motion,
pointer deltas, continuous scroll deltas, discrete scroll steps, and actions.
Each signal carries source and timing metadata plus reusable annotations.

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

A pipeline declares one input kind, an ordered array of stage instances, one
sink, and a dispatch budget. Stage APIs identify their strategy and sink APIs
identify their output type independently from instance stable IDs. Validation
checks:

- stable identities and required callbacks;
- type compatibility at every boundary;
- unique stage/sink identities;
- state storage for stateful stages;
- unique state ownership within the pipeline;
- compatibility between the last stage and the sink.

Validation claims stage instances for one pipeline and initializes them. The
same stage instance cannot be reused by another pipeline, and one pipeline
cannot assign the same state allocation to multiple stages. Construction code
must give distinct stage objects in different pipelines distinct state
allocations. Algorithm APIs and immutable configuration may still be shared.

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
deadline. The runtime exposes the nearest pipeline deadline to a future Zephyr
scheduler. When due, the stage's flush callback resumes output after that
stage; it does not restart the pipeline.

Deadline comparisons follow signed 32-bit uptime-difference semantics and are
covered across timestamp wrap. Firmware stage settings must remain well below
half the uptime range.

The host runtime does not own a thread or clock. Tests and the future Zephyr
adapter call `zpt_pipeline_flush()` with the observed time.

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

## Current stages

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

Production stateless stages now provide exact orthogonal orientation,
counts-per-inch normalization to Q16 millimetres, and raw-count pointer identity
for boundary testing. The ZMK adapter uses the standard input listener as its
source-specific processor host; routing and timer scheduling remain later
slices.
