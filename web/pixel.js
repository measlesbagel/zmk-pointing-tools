/* Pixel hunt: reach a 1 to 3 pixel target and hold the pointer on it for
 * 300 ms. Shows whether the slow end of the curve and the gain let a
 * single pixel be reached at all. Pure, so it runs under node:test. */

import { median } from "./measure.js";

export const PIXEL_SIZES = Object.freeze([3, 2, 1]);
export const HOLD_MS = 300;
export const PIXEL_TIMEOUT_MS = 10000;

/* Targets sit on whole pixels so a 1 px target is exactly one screen
 * pixel. `left` and `top` are the target's first pixel. */
export function pixelTrials(size, { perSize = 4, distance = 140, random = Math.random } = {}) {
  const centre = { x: Math.round(size.width / 2), y: Math.round(size.height / 2) };
  const reach = Math.min(distance, Math.min(size.width, size.height) / 2 - 20);
  const trials = [];
  for (const width of PIXEL_SIZES) {
    for (let i = 0; i < perSize; i += 1) {
      const angle = (2 * Math.PI * (i + random())) / perSize;
      const cx = Math.round(centre.x + reach * Math.cos(angle));
      const cy = Math.round(centre.y - reach * Math.sin(angle));
      const offset = Math.floor(width / 2);
      trials.push({ width, home: { ...centre, width: 28 }, target: { left: cx - offset, top: cy - offset, width } });
    }
  }
  return trials;
}

/* Inside means on one of the target's pixels: a point at x lies on pixel
 * floor(x). */
export function onTarget(target, point) {
  const x = Math.floor(point.x);
  const y = Math.floor(point.y);
  return x >= target.left && x < target.left + target.width && y >= target.top && y < target.top + target.width;
}

/* trial: { width, target, startedAt, heldAt | undefined, samples: [{t,x,y}] }.
 * `heldAt` is when the successful hold began. Entries count every time the
 * pointer landed on the target, the successful one included. */
export function measurePixelTrial(trial) {
  let entries = 0;
  let wasOn = false;
  for (const sample of trial.samples) {
    const on = onTarget(trial.target, sample);
    if (on && !wasOn) entries += 1;
    wasOn = on;
  }
  return {
    width: trial.width,
    reached: trial.heldAt !== undefined,
    acquireTime: trial.heldAt !== undefined ? trial.heldAt - trial.startedAt : NaN,
    entries,
  };
}

function summary(measured) {
  const reached = measured.filter(({ reached: ok }) => ok);
  return {
    trials: measured.length,
    reached: measured.length ? reached.length / measured.length : NaN,
    acquireTime: median(reached.map(({ acquireTime }) => acquireTime)),
    entries: median(measured.map(({ entries }) => entries)),
  };
}

export function analysePixel(trials) {
  const measured = trials.map(measurePixelTrial);
  const bySize = Object.fromEntries(PIXEL_SIZES
    .map((width) => [`${width} px`, measured.filter((item) => item.width === width)])
    .filter(([, items]) => items.length)
    .map(([name, items]) => [name, summary(items)]));
  const notes = [];
  for (const [name, item] of Object.entries(bySize)) {
    if (item.reached < 1) {
      notes.push(`${name} targets weren't always reached in ${PIXEL_TIMEOUT_MS / 1000} s: the slow end of the curve or the gain may be too high to land on them, or the motion gate may be swallowing small nudges.`);
    } else if (item.entries >= 3) {
      notes.push(`${name} targets took several landings before one held: the slow end of the curve may be high.`);
    }
  }
  return { pixel: bySize, notes, trials: measured };
}

/* One line per size for history. */
export function formatPixel(bySize) {
  const cell = (value, digits) => (Number.isFinite(value) ? value.toFixed(digits) : "n/a");
  return Object.entries(bySize).map(([name, item]) => `${name}: reached ${cell(item.reached * 100, 0)}%, `
    + `median time to hold ${cell(item.acquireTime, 0)} ms, landings ${cell(item.entries, 1)}, trials ${item.trials}`);
}
