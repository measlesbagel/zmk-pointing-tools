# Composable motion gate

The motion gate is a normalized-motion stage that distinguishes a coherent
intentional start from idle vibration without permanently discarding every
small input vector. It is semantic-neutral and can precede cursor, scroll, or
text-navigation mapping.

```text
Q16 millimetres
  -> coherent-displacement motion gate
  -> unchanged or accumulated Q16 millimetres
  -> any compatible downstream stage
```

## Coherent-displacement strategy

While idle, the strategy buffers complete vectors. It qualifies when the
accumulated radial displacement reaches `activation_distance` and its
rotation-invariant directional coherence reaches `coherence_percent`:

```text
|sum(v)| / sqrt(sample_count * sum(|v|^2))
```

A `coherence_percent` of 0 disables the coherence requirement entirely, so
any accumulated vector reaching the activation distance qualifies.

Qualification emits the complete buffered vector rather than losing its fine
movement. Later vectors pass unchanged until an idle deadline ends the active
gesture. Pending motion that cannot qualify before its own deadline is
discarded and starts fresh.

The reusable strategy uses signed 64-bit values and is unit-independent. The
pipeline stage consumes `NORMALIZED_MOTION` with a signed Q16 millimetre
activation distance, giving the same physical policy to sources with
different CPI.

## Evidence preservation

When multiple pending signals qualify together, the stage:

- emits their exact accumulated X/Y displacement;
- sums known sample spans with explicit saturation evidence;
- preserves all source-quality flags;
- uses the latest observation time and source sequence;
- marks the output as coalesced.

This is semantic accumulation, not source-transport batching. It happens after
source normalization and does not change CPI or calibration.

## Suppression policy

An optional suppression-policy callback participates in the same pass, buffer,
or drop decision. It receives the current signal and time but does not make the
strategy depend on ZMK key events. A later ZMK policy adapter can therefore
provide physical-keypress context, while host replay can provide deterministic
synthetic context.

Suppression drops the current vector, discards pending motion, and resets an
active gesture. It does not emit delayed movement after suppression ends.

## Lifecycle and deadlines

The stage owns no thread or timer. It requests the nearest idle or qualification
deadline from the shared pipeline runtime. The host or ZMK platform adapter
calls pipeline flush when that deadline is due.

Initialization, route entry/exit, source discontinuity, external suppression,
retuning, default restoration, and administrative reset all clear pending
motion and active state. Pending movement never crosses a pipeline change or
settings update.

## Current status

The pure strategy, pipeline stage, external suppression contract, deadline
behavior, and host tests are implemented. The noise fixtures replay through
the composed gate with resolution normalization and cover the scenarios that
previously exercised the monolithic noise-filter processor.
