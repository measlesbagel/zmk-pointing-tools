/* SPDX-License-Identifier: MIT */

/* The cursor acceleration curve as the tuner sees it: eight control points
 * of ball speed (mm/s) and sensitivity (percent), the monotone cubic the
 * firmware draws through them, its gain, and preset shapes that sample into
 * points. Shapes live here, not in firmware, so firmware stays one small
 * integer path while the tuner offers any curve. */

export const CURVE_POINTS = 8;

/* Speeds the preset shapes are sampled at, roughly doubling so each point
 * covers a similar share of a trackball's speed range. */
export const SHAPE_SPEEDS = [0, 8, 20, 45, 90, 160, 260, 400];

const SPEED_KEY = /^curve-speed-(\d+)$/;
const SENSITIVITY_KEY = /^curve-sensitivity-(\d+)$/;

/* The curve parameters of a tuning target, in point order, or null when the
 * target has no complete curve. Parameter keys come from metadata, so a
 * target is only recognized once its metadata has arrived. */
export function findCurve(target) {
  const speeds = new Array(CURVE_POINTS);
  const sensitivities = new Array(CURVE_POINTS);
  for (const parameter of target.parameters ?? []) {
    const speed = SPEED_KEY.exec(parameter.key ?? "");
    const sensitivity = SENSITIVITY_KEY.exec(parameter.key ?? "");
    if (speed) speeds[Number(speed[1]) - 1] = parameter;
    if (sensitivity) sensitivities[Number(sensitivity[1]) - 1] = parameter;
  }
  for (let i = 0; i < CURVE_POINTS; i += 1) {
    if (!speeds[i] || !sensitivities[i]) return null;
  }
  return {
    speeds,
    sensitivities,
    ids: new Set([...speeds, ...sensitivities].map(({ id }) => id)),
  };
}

/* Points from the curve's compiled or current values. */
export function curvePoints(curve, field = "current") {
  return curve.speeds.map((speed, i) => ({
    speed: speed[field],
    percent: curve.sensitivities[i][field],
  }));
}

export function samePoints(left, right) {
  return left.length === right.length &&
    left.every((point, i) => point.speed === right[i].speed && point.percent === right[i].percent);
}

/* Fritsch-Carlson tangents: the weighted harmonic mean of the neighbouring
 * secants, zero at an extremum and at both ends. */
function tangents(points) {
  const result = new Array(points.length).fill(0);
  for (let i = 1; i < points.length - 1; i += 1) {
    const h0 = points[i].speed - points[i - 1].speed;
    const h1 = points[i + 1].speed - points[i].speed;
    const s0 = (points[i].percent - points[i - 1].percent) / h0;
    const s1 = (points[i + 1].percent - points[i].percent) / h1;
    if (s0 * s1 > 0) {
      const w1 = 2 * h1 + h0;
      const w2 = h1 + 2 * h0;
      result[i] = (w1 + w2) / (w1 / s0 + w2 / s1);
    }
  }
  return result;
}

/* Sensitivity in percent at a speed, exactly the shape firmware evaluates:
 * a monotone cubic through the points, flat before the first and after the
 * last. Points must have strictly increasing speeds. */
export function sensitivityFunction(points) {
  const m = tangents(points);
  const last = points.length - 1;
  return (speed) => {
    if (speed <= points[0].speed) return points[0].percent;
    if (speed >= points[last].speed) return points[last].percent;
    let i = 0;
    while (speed >= points[i + 1].speed) i += 1;
    const h = points[i + 1].speed - points[i].speed;
    const t = (speed - points[i].speed) / h;
    const t2 = t * t;
    const t3 = t2 * t;
    return (2 * t3 - 3 * t2 + 1) * points[i].percent + (t3 - 2 * t2 + t) * h * m[i] +
      (-2 * t3 + 3 * t2) * points[i + 1].percent + (t3 - t2) * h * m[i + 1];
  };
}

/* Gain in percent: how fast cursor speed grows with ball speed,
 * d(v·s(v))/dv. A sensitivity curve without corners has a gain without
 * jumps, which is what makes acceleration feel continuous. */
export function gainFunction(points) {
  const sensitivity = sensitivityFunction(points);
  const h = 0.05;
  return (speed) => {
    const low = Math.max(speed - h, 0);
    const high = speed + h;
    return (high * sensitivity(high) - low * sensitivity(low)) / (high - low);
  };
}

/* A reason the points can't be sent, or null. */
export function validatePoints(points, curve) {
  for (let i = 0; i < CURVE_POINTS; i += 1) {
    const { speed, percent } = points[i];
    const speedParameter = curve.speeds[i];
    const sensitivityParameter = curve.sensitivities[i];
    if (!Number.isInteger(speed) || speed < speedParameter.minimum || speed > speedParameter.maximum) {
      return `Point ${i + 1} speed must be a whole number from ${speedParameter.minimum} to ${speedParameter.maximum} mm/s.`;
    }
    if (!Number.isInteger(percent) || percent < sensitivityParameter.minimum ||
        percent > sensitivityParameter.maximum) {
      return `Point ${i + 1} sensitivity must be a whole number from ${sensitivityParameter.minimum} to ${sensitivityParameter.maximum} %.`;
    }
    if (i > 0 && speed <= points[i - 1].speed) {
      return `Point ${i + 1} must be faster than point ${i}.`;
    }
  }
  return null;
}

/* The tuning batch that sets the whole curve at once. */
export function curveBatch(curve, points) {
  return [
    ...curve.speeds.map((parameter, i) => ({ parameterId: parameter.id, value: points[i].speed })),
    ...curve.sensitivities.map((parameter, i) => ({ parameterId: parameter.id, value: points[i].percent })),
  ];
}

const option = (id, label, unit, min, max, step, value, help) => ({ id, label, unit, min, max, step, value, help });

/* Preset shapes, from the curve families of libinput, RawAccel, and the
 * proposed trackball default. Each maps speed in mm/s to percent. */
export const SHAPES = [
  {
    id: "s-curve",
    label: "Smooth S-curve",
    summary: "A precision floor, a smooth rise through normal speeds, and a ceiling for flicks. The default curve.",
    options: [
      option("slow", "Slow", "%", 20, 200, 1, 65, "Floor for fine aiming."),
      option("fast", "Fast", "%", 100, 600, 5, 300, "Ceiling for flicks."),
      option("mid", "Midpoint", "mm/s", 10, 300, 1, 110, "Speed halfway between slow and fast."),
      option("steep", "Steepness", "×", 0.5, 5, 0.1, 2, "How quickly it moves from slow to fast."),
    ],
    percent: (v, p) => (v <= 0 ? p.slow : p.slow + (p.fast - p.slow) / (1 + (p.mid / v) ** p.steep)),
  },
  {
    id: "natural",
    label: "Natural",
    summary: "Rises from the base and eases into a limit it never passes (RawAccel Natural).",
    options: [
      option("base", "Base", "%", 20, 200, 1, 70, "Sensitivity until acceleration starts."),
      option("offset", "Offset", "mm/s", 0, 100, 1, 10, "Speed where acceleration starts."),
      option("limit", "Limit", "%", 100, 600, 10, 280, "The sensitivity it approaches."),
      option("half", "Halfway", "mm/s", 5, 300, 1, 60, "Speed above the offset where it is halfway to the limit."),
    ],
    percent: (v, p) => (v <= p.offset
      ? p.base
      : p.base + (p.limit - p.base) * (1 - 2 ** (-(v - p.offset) / p.half))),
  },
  {
    id: "power",
    label: "Power",
    summary: "Every doubling of speed multiplies sensitivity by the same amount, above a floor and up to a cap (RawAccel Power).",
    options: [
      option("base", "Floor", "%", 20, 200, 1, 70, "Sensitivity below the start speed."),
      option("start", "Start", "mm/s", 1, 100, 1, 15, "Speed where the power law takes over."),
      option("exponent", "Exponent", "", 0.05, 1.2, 0.05, 0.6, "Each doubling multiplies sensitivity by 2^exponent."),
      option("cap", "Cap", "%", 100, 800, 10, 320, "Upper limit."),
    ],
    percent: (v, p) => Math.min(p.base * Math.max(1, (Math.max(v, 1e-6) / p.start) ** p.exponent), p.cap),
  },
  {
    id: "classic",
    label: "Classic",
    summary: "Sensitivity rises with a power of the speed above an offset, up to a cap (RawAccel Classic; exponent 2 is Linear).",
    options: [
      option("base", "Base", "%", 20, 200, 1, 70, "Sensitivity until acceleration starts."),
      option("offset", "Offset", "mm/s", 0, 100, 1, 10, "Speed where acceleration starts."),
      option("accel", "Acceleration", "", 0.001, 0.05, 0.001, 0.012, "How quickly sensitivity rises above the offset."),
      option("exponent", "Exponent", "", 1.2, 4, 0.1, 2, "Bend of the rise."),
      option("cap", "Cap", "%", 100, 800, 10, 300, "Upper limit."),
    ],
    percent: (v, p) => (v <= p.offset
      ? p.base
      : Math.min(p.base * (1 + (p.accel ** (p.exponent - 1) * (v - p.offset) ** p.exponent) / v), p.cap)),
  },
  {
    id: "synchronous",
    label: "Synchronous",
    summary: "Slower than your cruising speed is slowed and faster is sped up, symmetric in log speed (RawAccel Synchronous).",
    options: [
      option("base", "At sync speed", "%", 30, 300, 5, 120, "Sensitivity at the sync speed."),
      option("sync", "Sync speed", "mm/s", 5, 300, 1, 60, "Your natural cruising speed."),
      option("motivity", "Motivity", "×", 1.1, 4, 0.05, 2.2, "Range from base ÷ motivity to base × motivity."),
      option("gamma", "Gamma", "", 0.2, 3, 0.05, 1, "How fast the change happens around the sync speed."),
    ],
    percent: (v, p) => {
      const logMotivity = Math.log(p.motivity);
      const difference = Math.log(Math.max(v, 1e-6)) - Math.log(p.sync);
      return p.base * Math.exp(Math.tanh((p.gamma / logMotivity) * difference) * logMotivity);
    },
  },
  {
    id: "flat",
    label: "Flat",
    summary: "One sensitivity at every speed: no acceleration.",
    options: [option("base", "Sensitivity", "%", 20, 400, 5, 100, "Sensitivity at every speed.")],
    percent: (v, p) => p.base,
  },
];

export function shapeDefaults(shape) {
  return Object.fromEntries(shape.options.map(({ id, value }) => [id, value]));
}

/* Points sampling a shape, rounded and held to the firmware's range. */
export function sampleShape(shape, values, maximumPercent = 1000) {
  return SHAPE_SPEEDS.map((speed) => ({
    speed,
    percent: Math.min(Math.max(Math.round(shape.percent(speed, values)), 1), maximumPercent),
  }));
}
