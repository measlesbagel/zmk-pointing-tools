export const PLAYGROUND_SCHEMA = "zmk-pointing-tools/playground-run";
export const PLAYGROUND_VERSION = 1;

export const ACTIVITIES = Object.freeze([
  { id: "scroll-vertical", label: "Vertical scroll", category: "Scroll" },
  { id: "scroll-horizontal", label: "Horizontal scroll", category: "Scroll" },
  { id: "scroll-two-axis", label: "Two-axis scroll", category: "Scroll" },
  { id: "text-selection", label: "Text movement and selection", category: "Text" },
  { id: "cursor-targets", label: "Cursor target acquisition", category: "Cursor" },
  { id: "cursor-precision", label: "Cursor precision targets", category: "Cursor" },
  { id: "cursor-path", label: "Diagonal and circle path", category: "Cursor" },
  { id: "click-drag", label: "Click and drag", category: "Buttons" },
]);

export function createRun(activityId, context = {}, now = Date.now()) {
  const activity = ACTIVITIES.find(({ id }) => id === activityId);
  if (!activity) throw new Error(`Unknown playground activity ${activityId}`);
  return {
    schema: PLAYGROUND_SCHEMA,
    version: PLAYGROUND_VERSION,
    activity: { ...activity },
    startedAt: new Date(now).toISOString(),
    startedMs: now,
    finishedAt: undefined,
    durationMs: undefined,
    startSequence: context.nextSequence ?? 0,
    tuningProfile: context.tuningProfile,
    metrics: {
      wheelEvents: 0,
      wheelX: 0,
      wheelY: 0,
      arrowKeys: 0,
      selectionChanges: 0,
      pointerMoves: 0,
      pointerDistance: 0,
      clicks: 0,
      errors: 0,
      targets: 0,
      drags: 0,
    },
    completed: false,
  };
}

export function recordWheel(run, deltaX, deltaY) {
  run.metrics.wheelEvents += 1;
  run.metrics.wheelX += Math.abs(deltaX);
  run.metrics.wheelY += Math.abs(deltaY);
}

export function recordArrow(run) { run.metrics.arrowKeys += 1; }
export function recordSelection(run) { run.metrics.selectionChanges += 1; }
export function recordClick(run, correct = true) {
  run.metrics.clicks += 1;
  if (correct) run.metrics.targets += 1;
  else run.metrics.errors += 1;
}
export function recordDrag(run, correct) {
  run.metrics.drags += 1;
  if (!correct) run.metrics.errors += 1;
}

export function recordPointer(run, previous, current) {
  run.metrics.pointerMoves += 1;
  if (previous) run.metrics.pointerDistance += Math.hypot(
    current.x - previous.x,
    current.y - previous.y,
  );
  return current;
}

export function finishRun(run, completed = true, now = Date.now()) {
  if (run.finishedAt) return run;
  run.finishedAt = new Date(now).toISOString();
  run.durationMs = Math.max(0, now - run.startedMs);
  run.completed = completed;
  return run;
}

export function exportRun(run, context = {}) {
  const stateEvents = (context.stateEvents ?? [])
    .filter(({ sequence }) => sequence >= run.startSequence);
  const { startedMs, startSequence, ...result } = run;
  return {
    ...result,
    tuningProfileAtExport: context.tuningProfile,
    telemetry: {
      stateSchemaVersion: context.stateSchemaVersion ?? 0,
      stateEvents,
      stateDropped: context.stateDropped ?? 0,
      queueCapacity: context.queueCapacity ?? 0,
    },
  };
}
