/* Compass start: works out from the first few pixels of each movement
 * whether the cursor goes the way the ball was rolled. Pure, so it runs
 * under node:test.
 *
 * Vectors are screen vectors (x right, y down), the same frame the firmware
 * reports in. The orientation stage swaps X and Y first, then inverts each
 * axis, so every one of the 8 axis flips and quarter turns is one setting
 * combination. */

/* The 8 cue directions, starting right and going anticlockwise on screen. */
export function cueDirections(count = 8) {
  return Array.from({ length: count }, (_, i) => {
    const angle = (2 * Math.PI * i) / count;
    return { x: Math.cos(angle), y: -Math.sin(angle) };
  });
}

/* The direction of the first `distance` pixels of movement away from
 * `start`, or undefined if the path never got that far. */
export function initialMovement(samples, start, distance = 20) {
  const away = samples.find((sample) => Math.hypot(sample.x - start.x, sample.y - start.y) >= distance);
  if (!away) return undefined;
  const length = Math.hypot(away.x - start.x, away.y - start.y);
  return { x: (away.x - start.x) / length, y: (away.y - start.y) / length };
}

/* Unsigned angle between two unit vectors, in degrees. */
export function angleBetween(a, b) {
  const dot = Math.max(-1, Math.min(1, a.x * b.x + a.y * b.y));
  return (Math.acos(dot) * 180) / Math.PI;
}

/* The 2x2 matrix [[a, b], [c, d]] of an orientation setting. */
export function orientationMatrix({ swapXY = false, invertX = false, invertY = false }) {
  const sx = invertX ? -1 : 1;
  const sy = invertY ? -1 : 1;
  return swapXY ? [[0, sx], [sy, 0]] : [[sx, 0], [0, sy]];
}

function apply(matrix, vector) {
  return { x: matrix[0][0] * vector.x + matrix[0][1] * vector.y, y: matrix[1][0] * vector.x + matrix[1][1] * vector.y };
}

function multiply(m, n) {
  return [
    [m[0][0] * n[0][0] + m[0][1] * n[1][0], m[0][0] * n[0][1] + m[0][1] * n[1][1]],
    [m[1][0] * n[0][0] + m[1][1] * n[1][0], m[1][0] * n[0][1] + m[1][1] * n[1][1]],
  ];
}

/* These matrices are orthogonal, so the inverse is the transpose. */
function transpose(m) {
  return [[m[0][0], m[1][0]], [m[0][1], m[1][1]]];
}

function settingsOf(matrix) {
  return matrix[0][0] === 0
    ? { swapXY: true, invertX: matrix[0][1] < 0, invertY: matrix[1][0] < 0 }
    : { swapXY: false, invertX: matrix[0][0] < 0, invertY: matrix[1][1] < 0 };
}

export const ORIENTATIONS = Object.freeze([false, true].flatMap((swapXY) => [false, true].flatMap(
  (invertX) => [false, true].map((invertY) => Object.freeze({ swapXY, invertX, invertY })))));

function median(values) {
  const sorted = [...values].sort((a, b) => a - b);
  const middle = Math.floor(sorted.length / 2);
  return sorted.length % 2 ? sorted[middle] : (sorted[middle - 1] + sorted[middle]) / 2;
}

/* trials: [{ cue: {x, y}, movement: {x, y} | undefined }]. Finds the flip
 * or turn that best maps each cue onto where the cursor actually went.
 * `agreement` is the share of trials that flip explains within 45 degrees;
 * `angleError` is the median error once it is accounted for. */
export function fitOrientation(trials) {
  const usable = trials.filter(({ movement }) => movement);
  if (!usable.length) return undefined;
  const scored = ORIENTATIONS.map((orientation) => {
    const matrix = orientationMatrix(orientation);
    const errors = usable.map(({ cue, movement }) => angleBetween(apply(matrix, cue), movement));
    return { orientation, matrix, errors, angleError: median(errors) };
  }).sort((a, b) => a.angleError - b.angleError);
  const best = scored[0];
  return {
    mismatch: best.orientation,
    matrix: best.matrix,
    correct: !best.orientation.swapXY && !best.orientation.invertX && !best.orientation.invertY,
    trials: usable.length,
    skipped: trials.length - usable.length,
    angleError: best.angleError,
    agreement: best.errors.filter((error) => error <= 45).length / usable.length,
  };
}

/* The setting that undoes an observed mismatch on top of the current one:
 * the cursor did mismatch(current(ball)), so the fix is mismatch⁻¹ · current. */
export function correctedOrientation(current, mismatch) {
  return settingsOf(multiply(transpose(orientationMatrix(mismatch)), orientationMatrix(current)));
}

/* Plain-language description of what the cursor did. */
export function describeMismatch({ swapXY, invertX, invertY }) {
  if (!swapXY && !invertX && !invertY) return "The cursor follows the ball.";
  if (!swapXY) {
    if (invertX && invertY) return "The cursor moves opposite to the ball on both axes.";
    return invertX ? "Left and right are mirrored." : "Up and down are mirrored.";
  }
  if (!invertX && !invertY) return "Horizontal and vertical are swapped.";
  if (invertX && invertY) return "Horizontal and vertical are swapped and both mirrored.";
  /* A swap with one flip is a quarter turn. */
  return invertY ? "The motion is turned a quarter turn anticlockwise." : "The motion is turned a quarter turn clockwise.";
}

/* Scores a compass run against the orientation it was played with. */
export function analyseCompass(trials, current) {
  const fit = fitOrientation(trials.map(({ cue, start, samples }) => ({ cue, movement: initialMovement(samples, start) })));
  if (!fit) return { notes: ["No movement was recorded."] };
  const notes = [describeMismatch(fit.mismatch)];
  if (fit.agreement < 0.75) {
    notes.push("The movements were not consistent enough to be sure; play it again rolling straight toward each target.");
  } else if (!fit.correct && current) {
    const fixed = correctedOrientation(current, fit.mismatch);
    const onOff = (value) => (value ? "on" : "off");
    notes.push(`To fix it, set swap-xy ${onOff(fixed.swapXY)}, invert-x ${onOff(fixed.invertX)}, invert-y ${onOff(fixed.invertY)}.`);
  }
  return {
    compass: { ...fit.mismatch, correct: fit.correct, agreement: fit.agreement, angleError: fit.angleError },
    notes: [...notes, `Median start-angle error ${fit.angleError.toFixed(0)}° over ${fit.trials} cues; ${Math.round(fit.agreement * 100)}% agree.`],
  };
}
