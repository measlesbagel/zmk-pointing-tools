/* Still hands: how much the pointer slips while you click, how long it
 * takes to move again after a click, and how much it drifts with hands
 * off the ball. The first two pull against each other through
 * suppress-after-keypress, so they are shown together. Pure, so it runs
 * under node:test. */

/* Movement this far from where the button came up counts as moving
 * again. */
export const RESUME_DISTANCE = 3;

function median(values) {
  const finite = values.filter(Number.isFinite);
  if (!finite.length) return NaN;
  const sorted = [...finite].sort((a, b) => a - b);
  const middle = Math.floor(sorted.length / 2);
  return sorted.length % 2 ? sorted[middle] : (sorted[middle - 1] + sorted[middle]) / 2;
}

function pathLength(samples) {
  let length = 0;
  for (let i = 1; i < samples.length; i += 1) {
    length += Math.hypot(samples[i].x - samples[i - 1].x, samples[i].y - samples[i - 1].y);
  }
  return length;
}

/* click: { down: {t,x,y}, up: {t,x,y}, held: [{t,x,y}] between them,
 * after: [{t,x,y}] after the button came up }. Drift is how far the
 * pointer travelled while the button was down; resume is how long after
 * the button came up the pointer first got RESUME_DISTANCE away. */
export function measureClick(click) {
  const path = [click.down, ...click.held, click.up];
  const moved = click.after.find((sample) => Math.hypot(sample.x - click.up.x, sample.y - click.up.y) >= RESUME_DISTANCE);
  return {
    drift: pathLength(path),
    landedOff: Math.hypot(click.up.x - click.down.x, click.up.y - click.down.y),
    resume: moved ? moved.t - click.up.t : NaN,
  };
}

/* rest: [{t,x,y}] recorded with hands off the ball. */
export function measureRest(samples) {
  if (!samples.length) return { path: 0, net: 0 };
  const first = samples[0];
  const last = samples.at(-1);
  return { path: pathLength(samples), net: Math.hypot(last.x - first.x, last.y - first.y) };
}

export function analyseStillHands(clicks, rest) {
  const measured = clicks.map(measureClick);
  const result = {
    clicks: measured.length,
    drift: median(measured.map(({ drift }) => drift)),
    driftedClicks: measured.filter(({ drift }) => drift >= 1).length,
    resume: median(measured.map(({ resume }) => resume)),
    rest: measureRest(rest),
  };
  const notes = [];
  if (result.driftedClicks / Math.max(1, result.clicks) >= 0.25) {
    notes.push("The pointer slipped during a quarter or more of the clicks: suppress-after-keypress may be too short to cover a press.");
  }
  if (result.resume >= 450) {
    notes.push("Moving again after a click took 450 ms or more: suppress-after-keypress may be holding motion back longer than a press needs.");
  }
  if (result.rest.path >= 2) {
    notes.push("The pointer moved with hands off the ball: the motion gate's activation distance or coherence may let sensor noise through.");
  }
  return { stillHands: result, notes };
}

export function formatStillHands(result) {
  const cell = (value, digits) => (Number.isFinite(value) ? value.toFixed(digits) : "n/a");
  return [
    `Clicks: ${result.clicks}, slipped on ${result.driftedClicks}, median slip ${cell(result.drift, 1)} px, `
      + `median time to move again ${cell(result.resume, 0)} ms`,
    `Rest: moved ${cell(result.rest.path, 1)} px in all, ${cell(result.rest.net, 1)} px from where it started`,
  ];
}
