/* Rail and corner: scroll along a narrow corridor, straight on one axis or
 * round an L corner that needs an axis switch. Shows how axis lock
 * handles intent: wobble leaking onto the other axis, a lock onto the
 * wrong axis, and the delay switching axis at a corner. No published
 * research covers axis lock, so the measures are plain and the thresholds
 * are starting points. Pure, so it runs under node:test. */

export const RAIL_WIDTH = 60;
export const RAIL_TIMEOUT_MS = 20000;
const LEG = 1200;
const CORNER_LEG = 700;
const EFFECTIVE_WIDTH_FACTOR = 4.133;

/* A course is a list of legs, each a unit direction and a length, in page
 * pixels from the start. */
export const RAIL_COURSES = Object.freeze([
  { name: "down", legs: [{ dx: 0, dy: 1, length: LEG }] },
  { name: "right", legs: [{ dx: 1, dy: 0, length: LEG }] },
  { name: "up", legs: [{ dx: 0, dy: -1, length: LEG }] },
  { name: "left", legs: [{ dx: -1, dy: 0, length: LEG }] },
  { name: "down then right", legs: [{ dx: 0, dy: 1, length: CORNER_LEG }, { dx: 1, dy: 0, length: CORNER_LEG }] },
  { name: "right then down", legs: [{ dx: 1, dy: 0, length: CORNER_LEG }, { dx: 0, dy: 1, length: CORNER_LEG }] },
]);

const axisOf = (leg) => (leg.dx ? "x" : "y");
const other = (axis) => (axis === "x" ? "y" : "x");

/* Where a page position sits on a course: which leg, how far along it,
 * and how far off its centre line. Past the last leg counts as the end. */
export function locate(course, point) {
  let start = { x: 0, y: 0 };
  let best;
  course.legs.forEach((leg, index) => {
    const along = (point.x - start.x) * leg.dx + (point.y - start.y) * leg.dy;
    const across = axisOf(leg) === "x" ? point.y - start.y : point.x - start.x;
    const clamped = Math.max(0, Math.min(leg.length, along));
    const distance = Math.abs(across) + Math.abs(along - clamped);
    if (!best || distance < best.distance) best = { leg: index, along, across, distance };
    start = { x: start.x + leg.dx * leg.length, y: start.y + leg.dy * leg.length };
  });
  return best;
}

export function finished(course, point) {
  const last = course.legs.length - 1;
  const where = locate(course, point);
  return where.leg === last && where.along >= course.legs[last].length - RAIL_WIDTH / 2;
}

function median(values) {
  const finite = values.filter(Number.isFinite);
  if (!finite.length) return NaN;
  const sorted = [...finite].sort((a, b) => a - b);
  const middle = Math.floor(sorted.length / 2);
  return sorted.length % 2 ? sorted[middle] : (sorted[middle - 1] + sorted[middle]) / 2;
}

function standardDeviation(values) {
  if (values.length < 2) return NaN;
  const mean = values.reduce((a, b) => a + b, 0) / values.length;
  return Math.sqrt(values.reduce((sum, value) => sum + (value - mean) ** 2, 0) / (values.length - 1));
}

/* run: { course, startedAt, endedAt | undefined,
 * events: [{ t, dx, dy, x, y }] } with x, y the page position after each
 * wheel event. */
export function measureRail(run) {
  const { course, events } = run;
  let onAxis = 0;
  let offAxis = 0;
  let wrongLocks = 0;
  let outside = 0;
  let wasOutside = false;
  const acrossByAxis = { x: [], y: [] };
  let cornerReached;
  let switched;
  for (const event of events) {
    const where = locate(course, event);
    const leg = course.legs[where.leg];
    const axis = axisOf(leg);
    const along = axis === "x" ? Math.abs(event.dx) : Math.abs(event.dy);
    const across = axis === "x" ? Math.abs(event.dy) : Math.abs(event.dx);
    onAxis += along;
    offAxis += across;
    if (across > 0 && along === 0) wrongLocks += 1;
    const out = Math.abs(where.across) > RAIL_WIDTH / 2;
    if (out && !wasOutside) outside += 1;
    wasOutside = out;
    acrossByAxis[axis].push(where.across);
    if (course.legs.length > 1) {
      const first = course.legs[0];
      /* The corner counts as reached near the end of the first leg, or
       * once the position is nearer the second leg, whichever comes first. */
      const atCorner = where.leg > 0 || where.along >= first.length - RAIL_WIDTH / 2;
      if (cornerReached === undefined && atCorner) cornerReached = event.t;
      const nextAxis = other(axisOf(first));
      const nextMove = nextAxis === "x" ? Math.abs(event.dx) : Math.abs(event.dy);
      const firstMove = nextAxis === "x" ? Math.abs(event.dy) : Math.abs(event.dx);
      if (cornerReached !== undefined && switched === undefined && nextMove > firstMove) switched = event.t;
    }
  }
  const reached = run.endedAt !== undefined;
  const time = reached && events.length ? run.endedAt - events[0].t : NaN;
  /* Effective-width steering: path length over 4.133 SD of the sideways
   * position, per second. One number per axis. */
  const steering = {};
  for (const axis of ["x", "y"]) {
    const length = course.legs.filter((leg) => axisOf(leg) === axis).reduce((sum, leg) => sum + leg.length, 0);
    const width = EFFECTIVE_WIDTH_FACTOR * standardDeviation(acrossByAxis[axis]);
    steering[axis] = length && width > 0 && reached ? length / width / (time / 1000) : NaN;
  }
  return {
    course: course.name,
    corner: course.legs.length > 1,
    reached,
    time,
    leakage: onAxis ? offAxis / onAxis : NaN,
    wrongLocks: events.length ? wrongLocks / events.length : NaN,
    exits: outside,
    switchDelay: switched !== undefined && cornerReached !== undefined ? switched - cornerReached : NaN,
    steering,
  };
}

export function analyseRail(runs) {
  const measured = runs.map(measureRail);
  const straight = measured.filter(({ corner }) => !corner);
  const corners = measured.filter(({ corner }) => corner);
  const steeringFor = (axis) => median(measured.map(({ steering }) => steering[axis]));
  const result = {
    straight: {
      reached: straight.length ? straight.filter(({ reached }) => reached).length / straight.length : NaN,
      leakage: median(straight.map(({ leakage }) => leakage)),
      wrongLocks: median(straight.map(({ wrongLocks }) => wrongLocks)),
      exits: median(straight.map(({ exits }) => exits)),
      time: median(straight.map(({ time }) => time)),
    },
    corners: {
      reached: corners.length ? corners.filter(({ reached }) => reached).length / corners.length : NaN,
      switchDelay: median(corners.map(({ switchDelay }) => switchDelay)),
      exits: median(corners.map(({ exits }) => exits)),
      time: median(corners.map(({ time }) => time)),
    },
    steering: { x: steeringFor("x"), y: steeringFor("y") },
  };
  const notes = [];
  if (result.straight.leakage >= 0.1) {
    notes.push("Straight scrolls leaked onto the other axis: engage-ratio may be high, or the lock may release too easily (release-ratio).");
  }
  if (result.straight.wrongLocks >= 0.1) {
    notes.push("Some straight scrolls moved only on the wrong axis: the lock may be choosing too early (activation-distance, window).");
  }
  if (result.corners.switchDelay >= 400) {
    notes.push("Turning a corner took 400 ms or more to switch axis: release-ratio, window or idle-timeout may be holding the old axis.");
  }
  return { rail: result, notes, trials: measured };
}

export function formatRail(result) {
  const cell = (value, digits, scale = 1) => (Number.isFinite(value) ? (value * scale).toFixed(digits) : "n/a");
  return [
    `Straight rails: reached ${cell(result.straight.reached, 0, 100)}%, leakage ${cell(result.straight.leakage, 1, 100)}%, `
      + `wrong-axis events ${cell(result.straight.wrongLocks, 1, 100)}%, exits ${cell(result.straight.exits, 1)}, median time ${cell(result.straight.time, 0)} ms`,
    `Corners: reached ${cell(result.corners.reached, 0, 100)}%, axis switch ${cell(result.corners.switchDelay, 0)} ms, `
      + `exits ${cell(result.corners.exits, 1)}, median time ${cell(result.corners.time, 0)} ms`,
    `Steering: vertical ${cell(result.steering.y, 2)}, horizontal ${cell(result.steering.x, 2)} per second`,
  ];
}
