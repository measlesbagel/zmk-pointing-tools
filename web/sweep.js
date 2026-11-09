/* Parameter sweep: one setting at 3 to 5 values, played blind in a
 * balanced order, with the measures plotted against the value. Pure, so
 * it runs under node:test. It never picks a value: Casiez et al. found
 * throughput levels off at high gain while overshoot keeps rising, so the
 * choice is a trade-off for the person to make. */

import { median, summarize } from "./measure.js";

const HIDDEN = ["P", "Q", "R", "S", "T"];

/* Evenly spaced whole values from `from` to `to`, inside the setting's
 * range, without repeats. */
export function sweepValues(from, to, count, { minimum = -Infinity, maximum = Infinity } = {}) {
  const low = Math.max(minimum, Math.min(from, to));
  const high = Math.min(maximum, Math.max(from, to));
  const steps = Math.max(2, Math.min(5, count));
  const values = Array.from({ length: steps }, (_, i) => Math.round(low + ((high - low) * i) / (steps - 1)));
  return [...new Set(values)];
}

function shuffle(items, random) {
  const copy = [...items];
  for (let i = copy.length - 1; i > 0; i -= 1) {
    const j = Math.floor(random() * (i + 1));
    [copy[i], copy[j]] = [copy[j], copy[i]];
  }
  return copy;
}

/* Every value twice: a shuffled order, then the same order reversed, so
 * practice and fatigue fall evenly. An unscored warm-up starts the session
 * and an unscored adaptation block comes before every scored block that
 * changes value. Values play under shuffled letters. */
export function planSweep(count, random = Math.random) {
  const keys = Array.from({ length: count }, (_, i) => i);
  const forward = shuffle(keys, random);
  const order = [...forward, ...[...forward].reverse()];
  const blocks = [{ condition: order[0], scored: false, kind: "warm-up" }];
  let previous = order[0];
  for (const condition of order) {
    if (condition !== previous) blocks.push({ condition, scored: false, kind: "adaptation" });
    blocks.push({ condition, scored: true, kind: "scored" });
    previous = condition;
  }
  const letters = shuffle(HIDDEN.slice(0, count), random);
  return { order, blocks, labels: Object.fromEntries(keys.map((key) => [key, letters[key]])) };
}

function mean(values) {
  const finite = values.filter(Number.isFinite);
  return finite.length ? finite.reduce((sum, value) => sum + value, 0) / finite.length : NaN;
}

/* One row per value, in value order, from the scored blocks. */
export function summarizeSweep(blocks, values) {
  return values.map((value, key) => {
    const own = blocks.filter((block) => block.scored && block.condition === key);
    const measured = own.flatMap((block) => block.measured);
    const summary = summarize(measured);
    return {
      value,
      trials: measured.length,
      throughput: mean(own.map((block) => block.throughput)),
      movementTime: median(measured.filter(({ hit }) => hit).map(({ movementTime }) => movementTime)),
      overshoot: summary.overshoot,
      rerolls: summary.rerolls,
      hitRate: summary.hitRate,
    };
  }).sort((a, b) => a.value - b.value);
}

export const SWEEP_MEASURES = Object.freeze([
  { key: "throughput", label: "Throughput", unit: "bit/s", digits: 2 },
  { key: "movementTime", label: "Median time on hits", unit: "ms", digits: 0 },
  { key: "overshoot", label: "Median overshoot", unit: "%", digits: 1, scale: 100 },
  { key: "rerolls", label: "Re-rolls per target", unit: "", digits: 2 },
]);

/* A small line chart of one measure against the value, as SVG markup.
 * Colours come from the page's CSS variables so it follows the theme. */
export function sweepChart(rows, measure, { width = 260, height = 120 } = {}) {
  const scale = measure.scale ?? 1;
  const points = rows
    .map((row) => ({ x: row.value, y: row[measure.key] * scale }))
    .filter(({ y }) => Number.isFinite(y));
  if (points.length < 2) return "";
  const pad = { left: 44, right: 12, top: 10, bottom: 24 };
  const xs = points.map(({ x }) => x);
  const ys = points.map(({ y }) => y);
  const [x0, x1] = [Math.min(...xs), Math.max(...xs)];
  let [y0, y1] = [Math.min(...ys), Math.max(...ys)];
  /* A flat line sits on a zero-based axis rather than inventing a range. */
  if (y0 === y1) [y0, y1] = y0 > 0 ? [0, y0] : y0 < 0 ? [y0, 0] : [0, 1];
  const px = (x) => pad.left + ((x - x0) / (x1 - x0 || 1)) * (width - pad.left - pad.right);
  const py = (y) => height - pad.bottom - ((y - y0) / (y1 - y0)) * (height - pad.top - pad.bottom);
  const path = points.map(({ x, y }, i) => `${i ? "L" : "M"}${px(x).toFixed(1)},${py(y).toFixed(1)}`).join(" ");
  const label = (value) => value.toFixed(measure.digits);
  return `<svg class="sweep-chart" viewBox="0 0 ${width} ${height}" role="img" aria-label="${measure.label} against the value">
  <line x1="${pad.left}" y1="${height - pad.bottom}" x2="${width - pad.right}" y2="${height - pad.bottom}" stroke="var(--line)"/>
  <text x="${pad.left - 4}" y="${py(y1) + 4}" text-anchor="end">${label(y1)}</text>
  <text x="${pad.left - 4}" y="${py(y0) + 4}" text-anchor="end">${label(y0)}</text>
  ${points.map(({ x }) => `<text x="${px(x).toFixed(1)}" y="${height - 8}" text-anchor="middle">${x}</text>`).join("")}
  <path d="${path}" fill="none" stroke="var(--accent)" stroke-width="2"/>
  ${points.map(({ x, y }) => `<circle cx="${px(x).toFixed(1)}" cy="${py(y).toFixed(1)}" r="3" fill="var(--accent)"/>`).join("")}
</svg>`;
}

/* The sweep as plain labelled lines for history and copying. */
export function describeSweep(name, rows) {
  const cell = (value, digits, scale = 1) => (Number.isFinite(value) ? (value * scale).toFixed(digits) : "n/a");
  return [
    `Sweep of ${name} over ${rows.map(({ value }) => value).join(", ")}, played blind in a balanced order.`,
    ...rows.map((row) => `${name} ${row.value}: ${SWEEP_MEASURES.map((measure) => `${measure.label.toLowerCase()} ${cell(row[measure.key], measure.digits, measure.scale)}${measure.unit === "%" ? "%" : measure.unit ? ` ${measure.unit}` : ""}`).join(", ")}, hit ${cell(row.hitRate, 0, 100)}%, trials ${row.trials}.`),
    "The sweep doesn't pick a value. Look for where throughput stops rising while overshoot and re-rolls keep changing.",
  ];
}
