export const PLAYGROUND_SCHEMA = "zmk-pointing-tools/playground-run";
export const PLAYGROUND_VERSION = 1;

export const ACTIVITIES = Object.freeze([
  { id: "scroll-vertical", label: "Vertical scroll", category: "Scroll" },
  { id: "scroll-horizontal", label: "Horizontal scroll", category: "Scroll" },
  { id: "scroll-two-axis", label: "Two-axis scroll", category: "Scroll" },
  { id: "text-selection", label: "Text movement and selection", category: "Text" },
  { id: "cursor-targets", label: "Cursor target acquisition", category: "Cursor" },
  { id: "cursor-precision", label: "Small-target precision", category: "Cursor" },
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

/* Small-target precision: the same targets every run, shrinking block by
 * block, with long and short moves mixed in each block, so a Sniper layer
 * and acceleration curves can be compared on equal terms. Positions are CSS
 * pixels inside a fixed layout box, so target sizes are real pixels. */
export const PRECISION_LAYOUT = Object.freeze({ width: 760, height: 280 });
export const PRECISION_SIZES = Object.freeze([32, 16, 8, 4]);
export const PRECISION_START = Object.freeze({ x: 380, y: 140, size: 32 });
const PRECISION_POSITIONS = [[80, 60], [680, 220], [560, 140], [120, 240], [240, 120], [700, 60]];

/* Every timed trial, each with the distance from the target before it. */
export function precisionTrials() {
  const trials = [];
  let previous = PRECISION_START;
  for (const size of PRECISION_SIZES) {
    for (const [x, y] of PRECISION_POSITIONS) {
      trials.push({
        index: trials.length,
        size,
        x,
        y,
        distance: Math.hypot(x - previous.x, y - previous.y),
      });
      previous = { x, y };
    }
  }
  return trials;
}

/* Tracking for one trial from the moment the previous target was
 * acquired. */
export function startPrecisionTrial(trial, from, now) {
  const dx = trial.x - from.x;
  const dy = trial.y - from.y;
  const length = Math.hypot(dx, dy) || 1;
  return {
    trial,
    startedMs: now,
    approach: { x: dx / length, y: dy / length },
    last: from,
    path: 0,
    inside: false,
    enteredMs: undefined,
    entries: 0,
    overshoot: 0,
    misses: 0,
  };
}

export function insidePrecisionTarget(trial, point) {
  return Math.hypot(point.x - trial.x, point.y - trial.y) <= trial.size / 2;
}

/* Fold a pointer position into the trial. Overshoot is how far the pointer
 * went past the target's far edge along the approach, once it had reached
 * the target. */
export function observePrecisionPointer(state, point, now) {
  const { trial } = state;
  state.path += Math.hypot(point.x - state.last.x, point.y - state.last.y);
  state.last = point;
  const inside = insidePrecisionTarget(trial, point);
  if (inside && !state.inside) {
    state.entries += 1;
    state.enteredMs = now;
  }
  if (!inside) state.enteredMs = undefined;
  state.inside = inside;
  if (state.entries > 0) {
    const along = (point.x - trial.x) * state.approach.x + (point.y - trial.y) * state.approach.y;
    state.overshoot = Math.max(state.overshoot, along - trial.size / 2);
  }
  return inside;
}

/* The trial's result. Time runs to the click, or with hover acquisition to
 * the final entry, so the dwell itself isn't counted. */
export function finishPrecisionTrial(state, now, hover) {
  const end = hover && state.enteredMs !== undefined ? state.enteredMs : now;
  return {
    index: state.trial.index,
    size: state.trial.size,
    distance: Math.round(state.trial.distance),
    timeMs: Math.max(0, Math.round(end - state.startedMs)),
    corrections: Math.max(0, state.entries - 1),
    overshootPx: Math.round(state.overshoot * 10) / 10,
    misses: state.misses,
    pathPx: Math.round(state.path),
  };
}

function median(values) {
  if (!values.length) return 0;
  const sorted = [...values].sort((a, b) => a - b);
  const middle = sorted.length >> 1;
  return sorted.length % 2 ? sorted[middle] : (sorted[middle - 1] + sorted[middle]) / 2;
}

/* Fitts throughput in bits per second: the trial's difficulty,
 * log2(distance / size + 1), over its time. */
function throughput(results) {
  const rates = results
    .filter(({ timeMs }) => timeMs > 0)
    .map(({ distance, size, timeMs }) => Math.log2(distance / size + 1) / (timeMs / 1000));
  return rates.length ? rates.reduce((sum, rate) => sum + rate, 0) / rates.length : 0;
}

/* Per-size medians and totals, and overall throughput. */
export function summarizePrecision(results) {
  const sizes = PRECISION_SIZES
    .map((size) => {
      const block = results.filter((result) => result.size === size);
      return {
        size,
        trials: block.length,
        medianTimeMs: Math.round(median(block.map(({ timeMs }) => timeMs))),
        corrections: block.reduce((sum, { corrections }) => sum + corrections, 0),
        medianOvershootPx: median(block.map(({ overshootPx }) => overshootPx)),
        misses: block.reduce((sum, { misses }) => sum + misses, 0),
        throughput: Math.round(throughput(block) * 100) / 100,
      };
    })
    .filter(({ trials }) => trials > 0);
  return {
    trials: results.length,
    totalTimeMs: results.reduce((sum, { timeMs }) => sum + timeMs, 0),
    throughput: Math.round(throughput(results) * 100) / 100,
    sizes,
  };
}
