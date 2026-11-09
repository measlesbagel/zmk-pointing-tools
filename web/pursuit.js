/* Pursuit: keep the pointer on a target that sweeps back and forth at a
 * steady speed and reverses sharply. Time on target is reported apart from
 * RMS error, with lag, speed matching and overshoot after each reversal,
 * which is where speed-rise, speed-fall and report intervals show. Pure,
 * so it runs under node:test. */

/* Precise phases are slow with a small target; reactive ones are fast
 * with a bigger one. Axes alternate so both are covered. */
export const PURSUIT_PHASES = Object.freeze([
  { variant: "precise", axis: "x", speed: 80, radius: 10, duration: 6000 },
  { variant: "precise", axis: "y", speed: 80, radius: 10, duration: 6000 },
  { variant: "reactive", axis: "x", speed: 400, radius: 20, duration: 6000 },
  { variant: "reactive", axis: "y", speed: 400, radius: 20, duration: 6000 },
]);

/* The first part of each phase is for catching the target and isn't
 * scored. */
export const SETTLE_MS = 1000;

/* Target position `elapsed` ms into a phase: a triangle wave about the
 * centre with half-width `amplitude`, starting at the centre and moving
 * toward positive x or y. */
export function pursuitPosition(phase, elapsed, { centre, amplitude }) {
  const travel = (phase.speed * elapsed) / 1000;
  const period = 4 * amplitude;
  const s = ((travel % period) + period) % period;
  const offset = s < amplitude ? s : s < 3 * amplitude ? 2 * amplitude - s : s - 4 * amplitude;
  return phase.axis === "x" ? { x: centre.x + offset, y: centre.y } : { x: centre.x, y: centre.y + offset };
}

/* Times in ms after the phase start at which the target reverses. */
export function reversalTimes(phase, amplitude) {
  const first = (amplitude / phase.speed) * 1000;
  const every = ((2 * amplitude) / phase.speed) * 1000;
  const times = [];
  for (let t = first; t < phase.duration; t += every) times.push(t);
  return times;
}

function median(values) {
  if (!values.length) return NaN;
  const sorted = [...values].sort((a, b) => a - b);
  const middle = Math.floor(sorted.length / 2);
  return sorted.length % 2 ? sorted[middle] : (sorted[middle - 1] + sorted[middle]) / 2;
}

/* The pointer's lag behind the target along the phase's axis: the shift,
 * in steps of `step` ms up to `maximum`, that best lines the pointer's
 * path up with the target's. */
function lag(samples, axis, { step = 8, maximum = 400 } = {}) {
  if (samples.length < 10) return NaN;
  const pointerAt = (t) => {
    let lo = 0;
    let hi = samples.length - 1;
    if (t <= samples[0].t) return samples[0].p[axis];
    if (t >= samples[hi].t) return samples[hi].p[axis];
    while (hi - lo > 1) {
      const mid = (lo + hi) >> 1;
      if (samples[mid].t <= t) lo = mid; else hi = mid;
    }
    const a = samples[lo];
    const b = samples[hi];
    return a.p[axis] + ((b.p[axis] - a.p[axis]) * (t - a.t)) / (b.t - a.t || 1);
  };
  let best = 0;
  let bestError = Infinity;
  for (let shift = 0; shift <= maximum; shift += step) {
    let error = 0;
    for (const sample of samples) error += (pointerAt(sample.t + shift) - sample.target[axis]) ** 2;
    if (error < bestError) [best, bestError] = [shift, error];
  }
  return best;
}

/* samples: [{ t, p: {x, y}, target: {x, y} }], t in ms from the phase
 * start, one per animation frame. */
export function measurePursuitPhase(phase, samples, { amplitude, centre }) {
  const scored = samples.filter(({ t }) => t >= SETTLE_MS);
  let onTime = 0;
  let total = 0;
  let squared = 0;
  for (let i = 1; i < scored.length; i += 1) {
    const dt = scored[i].t - scored[i - 1].t;
    const { p, target } = scored[i];
    const distance = Math.hypot(p.x - target.x, p.y - target.y);
    if (distance <= phase.radius) onTime += dt;
    squared += distance ** 2 * dt;
    total += dt;
  }
  const axis = phase.axis;
  const speeds = [];
  for (let i = 1; i < scored.length; i += 1) {
    const dt = scored[i].t - scored[i - 1].t;
    if (dt > 0) speeds.push((Math.abs(scored[i].p[axis] - scored[i - 1].p[axis]) * 1000) / dt);
  }
  /* After each reversal, how far the pointer carried on past the turning
   * point in the old direction within half a second. */
  const overshoots = reversalTimes(phase, amplitude).map((t, index) => {
    if (t < SETTLE_MS) return NaN;
    /* Reversals alternate between the positive and negative ends. */
    const turning = index % 2 === 0 ? 1 : -1;
    const turn = centre[axis] + turning * amplitude;
    const after = scored.filter((sample) => sample.t >= t && sample.t <= t + 500);
    if (!after.length) return NaN;
    return Math.max(0, ...after.map(({ p }) => (p[axis] - turn) * turning));
  }).filter(Number.isFinite);
  return {
    variant: phase.variant,
    axis,
    onTarget: total ? onTime / total : NaN,
    rmsError: total ? Math.sqrt(squared / total) : NaN,
    lag: lag(scored, axis),
    speedRatio: median(speeds) / phase.speed,
    overshoot: median(overshoots),
  };
}

/* Per-variant summaries, plus notes for anything that stands out. */
export function analysePursuit(phases) {
  const byVariant = {};
  for (const variant of ["precise", "reactive"]) {
    const own = phases.filter((phase) => phase.variant === variant);
    if (!own.length) continue;
    const mean = (key) => own.reduce((sum, item) => sum + item[key], 0) / own.length;
    byVariant[variant] = {
      onTarget: mean("onTarget"),
      rmsError: mean("rmsError"),
      lag: mean("lag"),
      speedRatio: mean("speedRatio"),
      overshoot: mean("overshoot"),
    };
  }
  const notes = [];
  const reactive = byVariant.reactive;
  const precise = byVariant.precise;
  if (reactive && reactive.lag >= 150) {
    notes.push("The pointer trailed the fast target by 150 ms or more: speed-rise may be slow, or reports may be spaced far apart.");
  }
  if (reactive && reactive.overshoot >= 30) {
    notes.push("The pointer ran on past fast reversals: speed-fall may be slow to bring the speed back down.");
  }
  if (precise && precise.speedRatio >= 1.5) {
    notes.push("At slow speed the pointer moved in faster bursts than the target: the slow end of the curve may be high, so smooth tracking turns into nudges.");
  }
  return { pursuit: byVariant, notes };
}

export function formatPursuit(byVariant) {
  const cell = (value, digits) => (Number.isFinite(value) ? value.toFixed(digits) : "n/a");
  return Object.entries(byVariant).map(([variant, item]) => `${variant[0].toUpperCase()}${variant.slice(1)}: `
    + `on target ${cell(item.onTarget * 100, 0)}%, RMS error ${cell(item.rmsError, 1)} px, lag ${cell(item.lag, 0)} ms, `
    + `speed ${cell(item.speedRatio, 2)}× the target's, overshoot after reversals ${cell(item.overshoot, 1)} px`);
}
