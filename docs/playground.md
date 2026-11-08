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

The games are built on the playground activities below, which stay available
under **Free play**.

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
