# Games and the playground

The tuner opens on **Games**. After connecting, it lists the games this
keyboard can play, worked out from the stages its firmware reports: a cursor
game needs a cursor-transfer or acceleration stage, scroll speed needs a
scroll stage, axis lock an axis-intent stage, text movement a text stage, and
so on (`web/games.js`). Nothing is asked up front.

Each game screen shows the game's one or two main settings, with the other
settings that affect it under **More settings**. **Play** opens the game's
activity; previews take effect immediately, and **Save** keeps the game's
stages on the device when the firmware can save (see
[Saving](protocol.md#saving)). Every setting row links to the same setting in
**Settings**, and each setting there lists the games that use it.

**Start first setup** walks through the available games in order: direction,
base speed, then each capability. **Diagnostics** lists what the connected
firmware reports and what is missing, next to the stage-decision stream.

### How games are scored

The tuner plays six cursor games itself, and first setup runs them in
this order:

- **Compass start** checks `swap-xy`, `invert-x` and `invert-y`. Click the
  centre, then roll toward the target that appears, 16 times over 8
  directions. Only the direction of the first 20 px counts, so it works
  even when the axes are wrong. It names what the cursor did ("Left and
  right are mirrored") and the settings that fix it, worked out from the
  ones it was played with. If fewer than three quarters of the movements
  agree, it asks for another go instead.
- **Throughput ring**, described below.
- **Distance flicks** goes from the centre to small targets at near, mid
  and far distances, 6 each in a fixed shuffled order. Every distance is
  scored on its own, and a distance that did clearly worse is flagged: far
  targets needing re-rolls or landing short, near targets landing past the
  centre or needing several corrections, or one distance missed much more
  often. Flags name the part of the curve involved, never a value.
- **Pixel hunt** shows a 3, 2 and then 1 pixel target, 4 of each, inside
  a faint ring. Rest the pointer on the target's pixels for 300 ms; no
  click is needed. Each size reports how often it was reached within 10 s,
  the time until the hold that counted, and how many landings it took. A
  size that wasn't always reached, or took several landings, is flagged
  against the slow end of the curve, the gain and the motion gate.
  Targets are snapped to whole screen pixels, so a 1 px target is exactly
  the pixel the pointer reports.
- **Pursuit** sweeps a target back and forth at a steady speed with sharp
  reversals: two slow phases with a small target, then two fast phases
  with a bigger one, side to side and up and down, 6 s each. The first
  second of each phase isn't scored. It reports time on target separately
  from RMS error, the lag that best lines the pointer's path up with the
  target's, the pointer's speed relative to the target's, and how far the
  pointer ran on past each reversal. A long lag on the fast phases points
  at `speed-rise` or the report intervals, run-on past reversals at
  `speed-fall`, and bursty slow tracking at the slow end of the curve.
- **Still hands** has you click 12 targets as they appear, alternating
  either side of the centre, then rest with hands off the ball and keys
  for 8 s. It reports how far the pointer slipped while each button was
  down, how long after the button came up the pointer first moved 3 px,
  and how far it drifted during the rest. Slip and the wait to move again
  are shown together because `suppress-after-keypress` trades one for the
  other; drift at rest points at the motion gate. Real keyboards report
  their clicks as key presses, so play it with the keyboard's own mouse
  button for a fair reading.

The scroll games:

- **Scroll to the band** moves a ruled page under a fixed band with the
  wheel. Each of 12 trials asks you to bring a line into the band and
  hold it for 400 ms, at 300, 900 and 2400 px each way. The first six show
  where the line is; the last six hide it until it scrolls into view,
  because a known target follows Fitts' law and an unknown one doesn't
  (Hinckley et al., 2002). It reports scroll throughput, time, overshoot
  past the band, reversals, settle time after first reaching the band, and
  the spacing of wheel events, for each block and each distance. It flags
  short scrolls going past the band, long ones not arriving, and wheel
  events 50 ms or more apart.
- **Rail and corner** moves a page under a fixed dot with both wheel
  axes. Six courses: straight tracks down, right, up and left, and two L
  corners that need an axis switch. Each ends when the dot reaches the far
  end. It reports, for straight tracks, how much motion leaked onto the
  other axis, the share of wheel events that moved only on the wrong axis,
  and how often the dot left the track; for corners, how long after
  reaching the corner the second axis took over; and an effective-width
  steering index (track length over 4.133 SD of the sideways position,
  per second) for each axis. No published research covers axis lock, so
  its flag thresholds are starting points. Distance lost at the start of a
  scroll needs the ball's own counts, which the page can't see, so it
  isn't measured here.

The text games:

- **Caret hunt** steps a caret over a 48 by 12 grid of characters with
  the arrow keys the trackball's text movement sends. Each of 16 moves asks
  you to bring the caret onto a highlighted character and stop there for
  400 ms: six along a line (3, 8 and 20 characters each way), six up and
  down (1, 3 and 6 lines), then four that need both. It reports, for each
  block, the steps taken against the fewest needed, overshoot past the
  target in characters and lines, reversals, the share of steps on the
  wrong axis in the straight blocks, runaway steps (steps less than 150 ms
  apart that kept coming after the caret first reached the target), and
  time per step. It flags overshoot and slow steps against
  `horizontal-threshold` and `vertical-threshold`, wrong-axis steps against
  the text pipeline's axis intent, and runaway steps against the thresholds
  and `idle-timeout`.
- **Type-through** has you type a passage with your hands resting near the
  ball. Typing needs no arrow keys, scrolling or pointer motion, so any of
  them are counted: while typing (up to a second after a key) and while
  resting in pauses between bursts. Motion before the first key is ignored.
  It flags caret steps, scrolling or pointer travel during typing against
  `suppress-after-keypress` and the text thresholds, and output during
  pauses against the motion gate. Both games' flag thresholds are starting
  points.

These games record the
whole pointer path, including every coalesced pointer event, and score it
with published pointing measures in `web/measure.js`:

- **Throughput** (bit/s), per ISO 9241-9: effective width
  We = 4.133 × the standard deviation of where clicks landed along the
  movement, IDe = log2(A / We + 1), averaged over sequences. It folds speed
  and accuracy together, so a setting that only makes you faster and
  sloppier doesn't win.
- **Endpoint error**, signed: positive means clicks landed past the target
  (too fast), negative short of it (too slow).
- **Re-entries**: how often the cursor left the target and came back.
  MacKenzie, Kauppinen & Silfverberg (2001) found this exposes trackball
  problems throughput alone hides.
- **Overshoot**: how far the path went past the target's far edge, as a
  share of the distance (Casiez et al., 2008).
- **Submovements** and **re-rolls**: velocity peaks per movement, and pauses
  of 30 ms or more inside one (a thumb lifting to roll again; the threshold
  still needs calibrating on real hardware).

Results are split by direction and by ring difficulty, kept in this browser,
and copyable as plain labelled text with the settings each run used. Ticking
two runs shows them side by side with the settings that differed. One run
each is only a rough guide; use a blind comparison to compare settings.

### Blind comparison

The Throughput ring and Distance flicks can compare two settings blind.
Type the values to try into the game's main settings without previewing
them, then press **Blind compare**. The values in effect now are side A;
the typed ones are side B. A session is 7 short blocks:

1. A warm-up that isn't scored, because scores climb at the start of every
   session.
2. The two sides in ABBA or BAAB order, picked at random.
3. An unscored adaptation block after every switch, because overshoot
   takes a block or two to settle on a new gain.

Sides play as X and Y, and the settings are hidden until the end. You say
which felt better before the reveal. The result is the median difference
in movement time on hits and in overshoot, B minus A, with a 95% bootstrap
interval, beside your preference. When the interval crosses zero the page
says the session couldn't tell the sides apart; it never picks a winner.
Flicks also flag a distance band that did clearly worse on either side.
The values in effect before the session are restored at the end, or if it
is abandoned.

Turn off the computer's own pointer acceleration while playing, so the
measures reflect only the keyboard's curve.

The other games still use the playground activities below, which stay
available under **Free play**.

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

### Sweeping one setting

Under **Sweep one setting**, pick one of the game's settings, a range and
3 to 5 values. The tuner spaces the values evenly and plays each one
twice, in a shuffled order and then that order reversed, with an unscored
warm-up and an unscored adaptation block before every change. The values
play under hidden letters (P, Q, R, …) with the settings hidden. At the
end it plots throughput, time on hits, overshoot and re-rolls against the
value, and keeps the same numbers as text in history. It never picks a
value: higher gain tends to level throughput off while overshoot keeps
rising (Casiez et al., 2008), so the trade-off is yours. The value in
effect before the sweep is restored at the end, or if it is abandoned.
