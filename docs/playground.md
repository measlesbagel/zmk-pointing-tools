# Guided playground

The local tuner includes repeatable activities for comparing pointing settings
without switching between arbitrary desktop applications. Open **Guided
playground** and choose an activity before starting a run.

Available activities cover:

- vertical, horizontal, and two-axis scrolling;
- text movement and Shift-selection;
- cursor target acquisition and a small-target precision test;
- diagonal and circular cursor paths;
- click-and-drag behavior.

Starting or resetting reconstructs the selected surface at the same initial
position. A modal contains scroll input so a running activity cannot move the
outer tuner page. Activities can be operated with ordinary mouse and keyboard
input when keyboard hardware is disconnected.

## Measurements

Runs record elapsed time, absolute horizontal and vertical wheel movement,
arrow and selection events, pointer path length, clicks, acquired targets,
drags, and errors as applicable. Automatically completed activities stop when
their goal is reached; path exercises use the explicit **Finish** button.

## Small-target precision

**Small-target precision** compares precision setups, such as a Sniper layer
against an acceleration curve, with numbers. Every run makes the same 24
moves: six targets at each of 32, 16, 8, and 4 CSS pixels, mixing long and
short moves within each size. Targets are acquired by holding the pointer
still inside them for half a second, or by clicking, chosen before starting.

Each trial is timed from the moment the previous target was acquired. Hold
timing stops when the pointer last entered the target, so the dwell itself is
not counted. Per trial the run records time, corrections (times the pointer
left the target and came back before acquiring it), overshoot (how far it
went past the target's far edge along the approach), misses in click mode,
and path length. Per size it reports median time, total corrections, median
overshoot, misses, and Fitts throughput in bits per second.

Give each run a label. Finished runs are kept in the browser's local storage
and listed side by side, newest first, until **Clear runs**. Set the host's
own pointer acceleration to flat while testing, or it adds its own curve on
top of the keyboard's.

## Guided tests record capture

Cursor target acquisition, Small-target precision, the three scroll
activities, and text movement and selection are guided tests. When the
keyboard supports input capture ([protocol](protocol.md#input-capture)),
starting one starts a recording of the pipeline it exercises, and finishing
or closing it stops it; the line under the controls says which pipeline
records, or why nothing does. The recording appears in **Record and score**
and is part of the run's result.

- **Which pipeline.** Pipelines are your configuration, so the tuner tells
  them apart by their last stage: a scroll batcher makes a scroll pipeline, a
  text navigation stage a text pipeline, anything else a cursor pipeline.
  With several of a kind, it takes the one picked in **Record and score**,
  else the one active when the test starts, else the first.
- **Labels.** Each trial or stretch becomes a label for the converter
  ([ground truth](trace-benchmark.md#recorded-traces-ground-truth)): cursor
  trials with their target's position and size and where the pointer was
  when the trial started, scroll tests with their axis (`both` for two-axis),
  and the text test with two stretches: straight down to the TARGET line
  (vertical), then along it (horizontal). A trial runs from the previous
  acquisition to this one. The first cursor or precision target appears only
  once the recording has started, so the first trial isn't cut off.
- **Clocks.** Labels are written in the page's clock and moved to the
  keyboard's when the recording stops: every capture status carries the
  device's uptime, and the status with the shortest round trip during the
  recording sets the offset, to within half that round trip. A round trip
  over 100 ms adds a warning to the recording. Firmware older than capture
  schema 3 sends no uptime: the test still records, but its labels are
  dropped and it is saved as a free recording.
- **Pointer path.** Cursor tests also keep the browser's pointer path, every
  2 ms at most, which the scoring uses to fit the screen scale and to find
  when each trial starts homing. Say whether the computer's own pointer
  acceleration is off in **Computer's pointer acceleration**: with it on,
  pixel metrics aren't scored.

Reset during a test labels the open scroll or text stretch, keeps the
recording so far as its own recording, and starts a new one. A guided
recording in which no trial finished has no labels; it is listed but not
ticked for scoring.

## Exports

**Export run** creates a `zmk-pointing-tools/playground-run` JSON document. It
contains:

- the activity and measured results;
- tuning profiles captured at run start and export;
- trace and semantic-state records produced during the run;
- stream metadata, queue capacity, and trace/state drop counts.

Exports are intended for comparisons and future analysis, not as firmware
configuration. Use the tuning panel's **Copy config** action to transfer chosen
values into Git-tracked devicetree configuration.
