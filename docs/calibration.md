# Calibration

The local tuner can set cursor gain, scroll speed, and text step size from
repeated gestures instead of hand-picked numbers. Two nominally identical
trackballs differ in hand mechanics, mounting, CPI, and sensor variation, so
each pipeline is calibrated from how its own ball is actually used.

Calibration is a host workflow. Firmware only reports what a stage emits;
every recommendation is computed in the browser and applied through the
ordinary runtime-tuning preview, so generic stage defaults stay
device-independent.

## Workflow

1. Connect the central half and open **Calibration**.
2. **Measure from** a resolution-normalize stage in the pipeline of the ball
   being calibrated. The panel selects the first stage whose stable id
   contains `normalize`.
3. **Calibrate** a tunable target. Targets are matched by their parameters:
   - cursor transfer (`scale-multiplier`, `scale-divisor`) by pixels per
     gesture;
   - scroll batcher (`steps-per-meter`) by scroll steps per gesture;
   - text navigation (`horizontal-threshold`, `vertical-threshold`) by key
     taps per gesture.

   Without a tunable target the panel still recommends a value.
4. For cursor gain, choose the acceleration stage to account for. The panel
   preselects the acceleration target whose stable id shares the longest
   prefix with the transfer's.
5. Enter the output one gesture should produce, press **Start**, repeat the
   same comfortable gesture 5 to 10 times with a short pause between them,
   and press **Stop**.
6. Review the recommendation and its confidence, then **Apply as preview**
   to try it. Changing the output, target, or acceleration choice re-analyses
   the same recording.
7. **Export calibration** or use the tuning panel's **Copy config** to keep
   the values.

Recording sets the measured stage to the every-frame level and restores its
previous level on **Stop**. Enable as few other targets as possible while
recording so the shared telemetry queue does not drop samples.

## Measurement

Every-frame samples from the normalize stage carry the frame's Q16 millimetre
motion, CPI, source-side sample span, source sequence, transport flags, and
source id (see [`protocol.md`](protocol.md#stage-state-telemetry)). The
analysis:

- splits motion into gestures at pauses longer than 150 ms or a change of
  source, skipping still frames;
- keeps gestures of at least 1 mm and 3 frames;
- calibrates the source with the most travel and reports how many gestures
  from other sources it ignored, since each ball has its own gain;
- measures each gesture's net distance along its main axis for scroll and
  text, and its net two-dimensional distance for the cursor;
- uses the median gesture distance, so one outlier does not move the result.

Distance is measured before gain, batching, coalescing, or report intervals,
which is why a recommendation never scales report cadence a second time.
Transport effects are reported separately from distance: arrival interval
(host-side cadence), sensor span (source-side cadence), coalesced frames,
lost frames from source-sequence gaps and transport gap flags, clipped
frames, and frames moving against the gesture, which can be a ball rolling
back or a sign-corrupted split frame.

## Recommendations

With `D` the median gesture distance in millimetres:

| Target | Requested output | Recommendation |
| --- | --- | --- |
| Scroll batcher | `S` steps | `steps-per-meter = S × 1000 / D` |
| Text navigation | `T` taps | threshold in µm `= D × 1000 / (T + 0.5)` on the gesture axis |
| Cursor transfer | `U` pixels | gain `= U / (D × CPI / 25.4)` as the closest multiplier/divisor pair |

Text navigation drops partial travel after its idle timeout, so a gesture
produces `floor(D / threshold)` taps. Centring the typical gesture between
`T` and `T + 1` taps tolerates variation either way.

The cursor quantizer emits `CPI / 25.4` pointer units per millimetre at unity
gain, so cursor gain is relative to the CPI recorded during calibration and
cursor speed still scales if CPI changes later. With an acceleration stage
selected, each frame is weighted by the gain that stage's current
parameters would apply, using the same linear-decay speed window and ramp, so
`D` is the accelerated distance. The multiplier/divisor pair is the one with
the smallest divisor within 0.1 % of the ideal gain.

Values are clamped to the parameter's advertised range, and the panel says
when that happened.

## Confidence

Confidence comes from the coefficient of variation of the gesture distances:
high at 10 % or less, medium at 20 % or less, and low above that. Fewer than
three usable gestures produce no recommendation. Dropped telemetry samples
(from status counters and gaps in the state sequence), lost or clipped
frames, a CPI change during recording, or a clamped value cap confidence at
low, and each is listed as a limitation.

The analysis does not model the motion gate's activation distance, axis-intent
engagement, or operating-system pointer acceleration; use a flat pointer
profile when calibrating the cursor.

Analysis is a pure function of the recorded frames and options, so the same
recording always produces the same recommendation.

## Export

**Export calibration** writes a tuning profile (see
[`profiles.md`](profiles.md)) whose targets contain only the recommended
values, so **Import profile** applies it again. The profile also carries a
`calibration` object with schema `zmk-pointing-tools/calibration` version 1:
the mode, unit, requested output, measured stage, acceleration parameters
used, summary, per-gesture evidence, recommendation, confidence with its
limitations, and every recorded frame, so the analysis can be reproduced.
