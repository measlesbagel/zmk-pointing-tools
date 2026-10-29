import test from "node:test";
import assert from "node:assert/strict";

import {
  CURVE_POINTS,
  SHAPES,
  SHAPE_SPEEDS,
  curveBatch,
  curvePoints,
  findCurve,
  gainFunction,
  sampleShape,
  sensitivityFunction,
  shapeDefaults,
  validatePoints,
} from "./acceleration-curve.js";
import { buildSimulatorTuningTargets } from "./simulator-model.js";

const [accelerationTarget] = buildSimulatorTuningTargets();
const DEFAULT_PERCENTS = [65, 66, 73, 99, 159, 225, 264, 283];

test("recognizes a curve target by its parameter keys", () => {
  const curve = findCurve(accelerationTarget);
  assert.ok(curve);
  assert.deepEqual(curve.speeds.map(({ id }) => id), [0, 1, 2, 3, 4, 5, 6, 7]);
  assert.deepEqual(curve.sensitivities.map(({ id }) => id), [8, 9, 10, 11, 12, 13, 14, 15]);
  assert.equal(curve.ids.size, 2 * CURVE_POINTS);
  assert.deepEqual(curvePoints(curve).map(({ speed }) => speed), SHAPE_SPEEDS);

  /* Until metadata arrives the keys are unknown, and so is the curve. */
  const pending = { parameters: accelerationTarget.parameters.map(({ key, ...rest }) => rest) };
  assert.equal(findCurve(pending), null);
  const partial = { parameters: accelerationTarget.parameters.filter(({ id }) => id !== 15) };
  assert.equal(findCurve(partial), null);
});

test("draws the firmware's monotone cubic through every point", () => {
  const points = curvePoints(findCurve(accelerationTarget));
  const sensitivity = sensitivityFunction(points);
  for (const { speed, percent } of points) assert.equal(sensitivity(speed), percent);
  assert.equal(sensitivity(-5), 65);
  assert.equal(sensitivity(5000), 283);
  /* What the firmware's Q16 curve returns at these speeds. */
  const firmware = [[4, 65.3030], [14, 68.7103], [30, 81.9855], [60, 118.5272], [120, 191.7770],
    [200, 244.8853], [330, 277.1210]];
  for (const [speed, percent] of firmware) assert.ok(Math.abs(sensitivity(speed) - percent) < 0.005);
  let previous = 0;
  for (let speed = 0; speed <= 450; speed += 0.5) {
    assert.ok(sensitivity(speed) >= previous);
    previous = sensitivity(speed);
  }
});

test("gain is the slope of cursor speed and equals sensitivity when flat", () => {
  const flat = SHAPE_SPEEDS.map((speed) => ({ speed, percent: 100 }));
  const gain = gainFunction(flat);
  for (const speed of [0, 10, 100, 1000]) assert.ok(Math.abs(gain(speed) - 100) < 1e-9);

  const points = curvePoints(findCurve(accelerationTarget));
  const sensitivity = sensitivityFunction(points);
  const rising = gainFunction(points);
  /* Rising sensitivity means gain runs ahead of it, and past the last
   * point the two meet again. */
  assert.ok(rising(90) > sensitivity(90));
  assert.ok(Math.abs(rising(600) - sensitivity(600)) < 1e-6);
});

test("validates points the way firmware will", () => {
  const curve = findCurve(accelerationTarget);
  const points = curvePoints(curve);
  assert.equal(validatePoints(points, curve), null);
  const crossing = points.map((point, i) => (i === 3 ? { ...point, speed: 20 } : point));
  assert.match(validatePoints(crossing, curve), /Point 4 must be faster than point 3/);
  const tooHigh = points.map((point, i) => (i === 7 ? { ...point, percent: 1001 } : point));
  assert.match(validatePoints(tooHigh, curve), /Point 8 sensitivity/);
  const fractional = points.map((point, i) => (i === 1 ? { ...point, speed: 7.5 } : point));
  assert.match(validatePoints(fractional, curve), /Point 2 speed/);
});

test("sends the whole curve as one batch of sixteen values", () => {
  const curve = findCurve(accelerationTarget);
  const batch = curveBatch(curve, curvePoints(curve));
  assert.equal(batch.length, 16);
  assert.deepEqual(batch.slice(0, 2), [{ parameterId: 0, value: 0 }, { parameterId: 1, value: 8 }]);
  assert.deepEqual(batch.at(-1), { parameterId: 15, value: 283 });
});

test("every preset samples into a valid curve", () => {
  const curve = findCurve(accelerationTarget);
  for (const shape of SHAPES) {
    const points = sampleShape(shape, shapeDefaults(shape));
    assert.equal(points.length, CURVE_POINTS, shape.id);
    assert.equal(validatePoints(points, curve), null, shape.id);
    for (const option of shape.options) {
      assert.ok(option.value >= option.min && option.value <= option.max, `${shape.id}.${option.id}`);
    }
  }
  /* The S-curve's defaults are the binding's default curve. */
  const scurve = SHAPES.find(({ id }) => id === "s-curve");
  assert.deepEqual(sampleShape(scurve, shapeDefaults(scurve)).map(({ percent }) => percent), DEFAULT_PERCENTS);
  const flat = SHAPES.find(({ id }) => id === "flat");
  assert.ok(sampleShape(flat, shapeDefaults(flat)).every(({ percent }) => percent === 100));
  /* Samples stay within the firmware's range even for extreme options. */
  const power = SHAPES.find(({ id }) => id === "power");
  const extreme = sampleShape(power, { base: 200, start: 1, exponent: 1.2, cap: 800 }, 500);
  assert.ok(extreme.every(({ percent }) => percent >= 1 && percent <= 500));
});

test("the precision preset dips below 1:1 for slow aiming and keeps fast gain", () => {
  const curve = findCurve(accelerationTarget);
  const precision = SHAPES.find(({ id }) => id === "precision");
  const points = sampleShape(precision, shapeDefaults(precision));
  assert.deepEqual(points.map(({ speed }) => speed), [0, 4, 10, 25, 54, 116, 250, 400]);
  assert.deepEqual(points.map(({ percent }) => percent), [35, 35, 68, 100, 126, 174, 200, 200]);
  const sensitivity = sensitivityFunction(points);
  assert.ok(sensitivity(3) < 40);
  assert.equal(sensitivity(25), 100);
  /* Corners squeezed together still sample into a valid curve. */
  for (const values of [
    { floor: 15, floorUntil: 20, unity: 5, fast: 100, fastFrom: 50 },
    { floor: 100, floorUntil: 1, unity: 100, fast: 600, fastFrom: 800 },
  ]) {
    assert.equal(validatePoints(sampleShape(precision, values), curve), null);
  }
});
