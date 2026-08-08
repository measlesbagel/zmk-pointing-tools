# Guided playground

The local tuner includes repeatable activities for comparing pointing settings
without switching between arbitrary desktop applications. Open **Guided
playground** and choose an activity before starting a run.

Available activities cover:

- vertical, horizontal, and two-axis scrolling;
- text movement and Shift-selection;
- normal and precision cursor target acquisition;
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
