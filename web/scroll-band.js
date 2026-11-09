/* Scroll to the band: scroll a page until a target line sits in a band at
 * the middle of the view, and hold it there. With the target's position
 * shown, scrolling follows Fitts' law (Hinckley et al., CHI 2002); with it
 * hidden until it comes into view, time grows with distance instead, so
 * the two run as separate blocks. Pure, so it runs under node:test. */

import { median } from "./measure.js";

export const BAND_HOLD_MS = 400;
export const SCROLL_TIMEOUT_MS = 15000;
const DISTANCES = [300, 900, 2400];

function shuffle(items, random) {
  const copy = [...items];
  for (let i = copy.length - 1; i > 0; i -= 1) {
    const j = Math.floor(random() * (i + 1));
    [copy[i], copy[j]] = [copy[j], copy[i]];
  }
  return copy;
}

/* Two blocks, shown then hidden, each with every distance both ways. The
 * page offset carries on from trial to trial, so `distance` is signed:
 * positive means the target is further down the page. */
export function scrollTrials({ band = 40, random = Math.random } = {}) {
  const block = (known) => shuffle(DISTANCES.flatMap((distance) => [distance, -distance]), random)
    .map((distance) => ({ known, distance, band }));
  return [...block(true), ...block(false)];
}

/* trial: { distance, band, startedAt, heldAt | undefined,
 * events: [{ t, delta, offset }] } where offset is how far the page has
 * moved toward the target since the trial began, after that wheel event. */
export function measureScrollTrial(trial) {
  const { distance, band, events } = trial;
  const direction = Math.sign(distance);
  const along = (offset) => offset * direction;
  const target = Math.abs(distance);
  const farEdge = target + band / 2;
  const nearEdge = target - band / 2;
  let reversals = 0;
  let lastSign = 0;
  let enteredAt;
  for (const event of events) {
    const sign = Math.sign(event.delta);
    if (sign && lastSign && sign !== lastSign) reversals += 1;
    if (sign) lastSign = sign;
    const position = along(event.offset);
    const inside = position >= nearEdge && position <= farEdge;
    if (inside && enteredAt === undefined) enteredAt = event.t;
    if (!inside) enteredAt = undefined;
  }
  const furthest = Math.max(0, ...events.map(({ offset }) => along(offset)));
  const gaps = [];
  for (let i = 1; i < events.length; i += 1) gaps.push(events[i].t - events[i - 1].t);
  const firstEntry = events.find(({ offset }) => along(offset) >= nearEdge && along(offset) <= farEdge);
  const reached = trial.heldAt !== undefined;
  return {
    known: trial.known,
    distance: target,
    band,
    reached,
    time: reached && events.length ? trial.heldAt - events[0].t : NaN,
    overshoot: Math.max(0, furthest - farEdge),
    reversals,
    settle: reached && firstEntry ? trial.heldAt - firstEntry.t : NaN,
    wheelInterval: median(gaps),
  };
}

function summarize(measured) {
  const reached = measured.filter(({ reached: ok }) => ok);
  /* Scroll throughput: mean of ID / time over reached trials, with
   * ID = log2(D / W + 1) on the nominal band. */
  const throughputs = reached.map(({ distance, band, time }) => Math.log2(distance / band + 1) / (time / 1000))
    .filter(Number.isFinite);
  return {
    trials: measured.length,
    reached: measured.length ? reached.length / measured.length : NaN,
    throughput: throughputs.length ? throughputs.reduce((a, b) => a + b, 0) / throughputs.length : NaN,
    time: median(reached.map(({ time }) => time)),
    overshoot: median(measured.map(({ overshoot }) => overshoot)),
    reversals: median(measured.map(({ reversals }) => reversals)),
    settle: median(reached.map(({ settle }) => settle)),
    wheelInterval: median(measured.map(({ wheelInterval }) => wheelInterval).filter(Number.isFinite)),
  };
}

export function analyseScrollBand(trials) {
  const measured = trials.map(measureScrollTrial);
  const blocks = {};
  for (const [name, known] of [["Shown", true], ["Hidden", false]]) {
    const own = measured.filter((item) => item.known === known);
    if (own.length) blocks[name] = summarize(own);
  }
  const byDistance = Object.fromEntries(DISTANCES.map((distance) => [`${distance} px`,
    summarize(measured.filter((item) => item.distance === distance))]).filter(([, item]) => item.trials));
  const notes = [];
  const near = byDistance[`${DISTANCES[0]} px`];
  const far = byDistance[`${DISTANCES.at(-1)} px`];
  if (near && near.overshoot >= 40) {
    notes.push("Short scrolls went past the band: steps-per-meter may be high for small moves.");
  }
  if (far && far.reached < 1) {
    notes.push(`Long scrolls didn't always reach the band in ${SCROLL_TIMEOUT_MS / 1000} s: steps-per-meter may be low for long moves.`);
  }
  if (Object.values(blocks).some(({ wheelInterval }) => wheelInterval >= 50)) {
    notes.push("Scroll events arrived 50 ms or more apart: the report interval may make scrolling feel steppy.");
  }
  return { scrollBand: { blocks, byDistance }, notes, trials: measured };
}

export function formatScrollBand({ blocks, byDistance }) {
  const cell = (value, digits) => (Number.isFinite(value) ? value.toFixed(digits) : "n/a");
  const line = (name, item) => `${name}: reached ${cell(item.reached * 100, 0)}%, `
    + `${Number.isFinite(item.throughput) ? `throughput ${cell(item.throughput, 2)} bit/s, ` : ""}`
    + `median time ${cell(item.time, 0)} ms, overshoot ${cell(item.overshoot, 0)} px, reversals ${cell(item.reversals, 1)}, `
    + `settle ${cell(item.settle, 0)} ms, wheel events every ${cell(item.wheelInterval, 0)} ms, trials ${item.trials}`;
  return [
    ...Object.entries(blocks).map(([name, item]) => line(`Target ${name.toLowerCase()}`, item)),
    ...Object.entries(byDistance).map(([name, item]) => line(name, item)),
  ];
}
