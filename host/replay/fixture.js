/* SPDX-License-Identifier: MIT */

export const FIXTURE_SCHEMA = "zmk-pointing-tools/trace-fixture";
export const FIXTURE_VERSIONS = [1, 2];
export const PROFILE_SCHEMA = "zmk-pointing-tools/tuning-profile";
export const PROFILE_VERSION = 1;

const MAX_TRACE_US = 0xffffffff;
const POLICIES = { free: 0, adaptive: 1, horizontal: 2, vertical: 3 };
const KINDS = ["composed-scroll", "composed-text", "composed-noise", "cursor-pipeline",
  "scroll-pipeline", "text-pipeline"];
export const CURSOR_STAGE_TYPES = ["orientation", "normalize", "gate", "acceleration",
  "transfer", "quantizer", "coalescer"];
/* Stage types of each stage-chain kind, and the stage it must end with.
 * The chain runner tells the kinds apart by that last stage. */
const INPUT_STAGE_TYPES = ["orientation", "normalize", "gate"];
const AXIS_STAGE_TYPES = ["axis-intent", "axis-constraint"];
const CHAIN_KINDS = {
  "cursor-pipeline": { types: CURSOR_STAGE_TYPES },
  "scroll-pipeline": { types: [...INPUT_STAGE_TYPES, ...AXIS_STAGE_TYPES, "scroll-batcher"],
    last: "scroll-batcher" },
  "text-pipeline": { types: [...INPUT_STAGE_TYPES, ...AXIS_STAGE_TYPES, "text-nav"],
    last: "text-nav" },
};

/* A version 2 fixture whose processor is a stage chain: cursor, scroll, or
 * text pipeline stages with their settings, replayed by the chain runner. */
export const isStageChain = (fixture) =>
  fixture.version === 2 && fixture.processor.kind in CHAIN_KINDS;
const STROKE_ACTIVITIES = ["slow-aim", "tremor", "flick", "aim", "scroll", "text",
  "post-typing"];

function requireInteger(value, name, minimum = Number.MIN_SAFE_INTEGER) {
  if (!Number.isSafeInteger(value) || value < minimum) {
    throw new Error(`${name} must be an integer >= ${minimum}`);
  }
}

/* How many milliseconds before its fixture time an event's stages saw it:
 * the last field of a recorded motion (sixth), keypress, or flush (third)
 * event. */
export const lateMs = (event) =>
  (event[0] === "motion" ? event[5] : event[2]) ?? 0;

/* Absolute event times in microseconds. Version 1 deltas are milliseconds,
 * version 2 deltas microseconds. */
export function eventTimesUs(fixture) {
  const scale = fixture.version === 1 ? 1000 : 1;
  let time = 0;
  return fixture.events.map((event) => {
    time += event[1] * scale;
    return time;
  });
}

/* The millisecond arrival time firmware stamps a frame with. */
export const toMilliseconds = (timeUs) => Math.floor(timeUs / 1000);

export function durationMs(fixture) {
  const times = eventTimesUs(fixture);
  return toMilliseconds(times[times.length - 1]);
}

function validateLegacySettings(kind, settings, policy, path) {
  if (kind === "composed-scroll") {
    for (const key of ["cpi", "stepsPerMeter", "reportIntervalMs", "idleTimeoutMs",
      "suppressAfterKeypressMs", "engageRatioPercent", "releaseRatioPercent",
      "activationDistanceMicrometers", "intentWindowMs"]) {
      requireInteger(settings?.[key], `${path}: settings.${key}`,
        key === "suppressAfterKeypressMs" ? 0 : 1);
    }
    if (typeof settings.discardUnclassified !== "boolean") {
      throw new Error(`${path}: discardUnclassified must be boolean`);
    }
    if (!(policy in POLICIES)) throw new Error(`${path}: unknown axis policy`);
  } else if (kind === "cursor-pipeline") {
    for (const key of ["cpi", "scaleMultiplier", "scaleDivisor"]) {
      requireInteger(settings?.[key], `${path}: settings.${key}`, 1);
    }
  } else if (kind === "composed-text") {
    for (const key of ["cpi", "engageRatioPercent", "releaseRatioPercent",
      "activationDistanceMicrometers", "intentWindowMs", "idleTimeoutMs",
      "horizontalThresholdMicrometers", "verticalThresholdMicrometers",
      "mapperIdleTimeoutMs"]) {
      requireInteger(settings?.[key], `${path}: settings.${key}`, 1);
    }
    if (typeof settings.discardUnclassified !== "boolean") {
      throw new Error(`${path}: discardUnclassified must be boolean`);
    }
  } else if (kind === "composed-noise") {
    if (typeof settings?.enabled !== "boolean") {
      throw new Error(`${path}: settings.enabled must be boolean`);
    }
    for (const key of ["cpi", "activationDistanceMicrometers", "qualificationTimeoutMs",
      "idleTimeoutMs"]) {
      requireInteger(settings?.[key], `${path}: settings.${key}`, 1);
    }
    requireInteger(settings?.coherencePercent, `${path}: settings.coherencePercent`, 0);
    requireInteger(settings?.suppressAfterKeypressMs,
      `${path}: settings.suppressAfterKeypressMs`, 0);
    if (settings.coherencePercent > 100) {
      throw new Error(`${path}: settings.coherencePercent must not exceed 100`);
    }
  }
}

export function validateProfile(profile, path = "profile") {
  if (profile?.schema !== PROFILE_SCHEMA || profile?.version !== PROFILE_VERSION ||
      !Array.isArray(profile.targets)) {
    throw new Error(`${path}: expected ${PROFILE_SCHEMA} v${PROFILE_VERSION}`);
  }
  const targets = new Set();
  for (const target of profile.targets) {
    if (typeof target?.stableId !== "string" || !Array.isArray(target.parameters)) {
      throw new Error(`${path}: malformed target`);
    }
    if (targets.has(target.stableId)) {
      throw new Error(`${path}: duplicate target ${target.stableId}`);
    }
    targets.add(target.stableId);
    const keys = new Set();
    for (const parameter of target.parameters) {
      if (typeof parameter?.key !== "string") throw new Error(`${path}: malformed parameter`);
      if (keys.has(parameter.key)) {
        throw new Error(`${path}: duplicate parameter ${target.stableId}.${parameter.key}`);
      }
      keys.add(parameter.key);
      requireInteger(parameter.value, `${path}: ${target.stableId}.${parameter.key}`);
    }
  }
  return profile;
}

/* Values a profile holds for one stage, by tuning key. */
function profileValues(profile, stableId) {
  const target = profile?.targets.find((candidate) => candidate.stableId === stableId);
  return Object.fromEntries((target?.parameters ?? []).map(({ key, value }) => [key, value]));
}

/* A stage chain's stages with every settings source merged: inline
 * settings, then the fixture's embedded profile, then an override profile,
 * each matched by stage stable id. */
export function resolveCursorStages(fixture, overrideProfile = null) {
  return fixture.processor.stages.map((stage) => ({
    ...stage,
    settings: {
      ...stage.settings,
      ...profileValues(fixture.processor.profile, stage.stableId),
      ...profileValues(overrideProfile, stage.stableId),
    },
  }));
}

function validateChainStages(processor, path) {
  const { types, last } = CHAIN_KINDS[processor.kind];
  if (!Array.isArray(processor.stages) || processor.stages.length === 0) {
    throw new Error(`${path}: ${processor.kind} v2 needs stages`);
  }
  /* Only the cursor coalescer reads the link; other chains may leave it out. */
  if (!["usb", "ble"].includes(processor.link) &&
      !(processor.kind !== "cursor-pipeline" && processor.link === undefined)) {
    throw new Error(`${path}: processor.link must be usb or ble`);
  }
  if (processor.profile !== undefined) validateProfile(processor.profile, `${path}: profile`);
  if (last !== undefined && processor.stages.at(-1)?.type !== last) {
    throw new Error(`${path}: a ${processor.kind} ends with a ${last} stage`);
  }
  for (const [index, stage] of processor.stages.entries()) {
    if (!types.includes(stage?.type) || typeof stage.stableId !== "string" ||
        !/^[A-Za-z0-9_.-]{1,63}$/.test(stage.stableId)) {
      throw new Error(`${path}: stage ${index} needs a type a ${processor.kind} takes ` +
        `(${types.join(", ")}) and a stable id`);
    }
    for (const [key, value] of Object.entries(stage.settings ?? {})) {
      if (!Number.isSafeInteger(value) && typeof value !== "boolean") {
        throw new Error(`${path}: ${stage.stableId}.${key} must be an integer or boolean`);
      }
    }
  }
}

function validateSource(source, path) {
  if (!source || !["central", "peripheral"].includes(source.location) ||
      typeof source.device !== "string") {
    throw new Error(`${path}: v2 source needs a device and a central or peripheral location`);
  }
  requireInteger(source.cpi, `${path}: source.cpi`, 1);
  if (!["none", "sensor"].includes(source.span)) {
    throw new Error(`${path}: source.span must be none or sensor`);
  }
}

function validateEvents(fixture, path) {
  if (!Array.isArray(fixture.events) || fixture.events.length === 0) {
    throw new Error(`${path}: events are required`);
  }
  /* Recorded deadline flushes and late stage times replay only through the
   * chain runner. */
  const recorded = isStageChain(fixture);
  const motionLengths = fixture.version === 1 ? [4] : recorded ? [4, 5, 6] : [4, 5];
  const types = recorded ? ["motion", "keypress", "flush"] : ["motion", "keypress"];
  const times = eventTimesUs(fixture);
  for (const [index, event] of fixture.events.entries()) {
    if (!Array.isArray(event) || !types.includes(event[0])) {
      throw new Error(`${path}: invalid event ${index}`);
    }
    requireInteger(event[1], `${path}: event ${index} delta`, 0);
    if (event[0] === "motion") {
      if (!motionLengths.includes(event.length)) {
        throw new Error(`${path}: motion event ${index} needs delta, x, and y` +
          (fixture.version === 1 ? "" : ", and optionally a span" +
            (recorded ? " and a late time" : "")));
      }
      requireInteger(event[2], `${path}: event ${index} x`);
      requireInteger(event[3], `${path}: event ${index} y`);
      if (event.length >= 5) requireInteger(event[4], `${path}: event ${index} span`, 0);
    } else if (event.length !== 2 && !(recorded && event.length === 3)) {
      throw new Error(`${path}: ${event[0]} event ${index} only accepts delta` +
        (recorded ? " and a late time" : ""));
    }
    const late = lateMs(event);
    requireInteger(late, `${path}: event ${index} late time`, 0);
    if (late > toMilliseconds(times[index])) {
      throw new Error(`${path}: event ${index} is late by more than its time since the trace start`);
    }
  }
  /* Runners read times as 32-bit microseconds: about 71.6 minutes. */
  if (times[times.length - 1] > MAX_TRACE_US) {
    throw new Error(`${path}: traces must be shorter than ${MAX_TRACE_US} µs; split the recording`);
  }
}

function validateTruth(truth, path) {
  if (truth === undefined) return;
  if (!Array.isArray(truth.strokes)) throw new Error(`${path}: truth.strokes must be a list`);
  for (const [index, stroke] of truth.strokes.entries()) {
    const name = `${path}: stroke ${stroke?.id ?? index}`;
    if (typeof stroke?.id !== "string" || !STROKE_ACTIVITIES.includes(stroke.activity) ||
        typeof stroke.intended !== "boolean") {
      throw new Error(`${name} needs an id, a known activity, and intended`);
    }
    requireInteger(stroke.startUs, `${name} startUs`, 0);
    requireInteger(stroke.endUs, `${name} endUs`, stroke.startUs + 1);
    if (stroke.speedMmPerS !== undefined &&
        !(typeof stroke.speedMmPerS === "number" && stroke.speedMmPerS > 0)) {
      throw new Error(`${name} speedMmPerS must be positive`);
    }
    if (stroke.axis !== undefined && !["horizontal", "vertical"].includes(stroke.axis)) {
      throw new Error(`${name} axis must be horizontal or vertical`);
    }
    if (stroke.path !== undefined) {
      const { intervalUs, points } = stroke.path;
      requireInteger(intervalUs, `${name} path.intervalUs`, 1);
      if (!Array.isArray(points) || points.length < 2 || !points.every((point) =>
        Array.isArray(point) && point.length === 2 && point.every(Number.isFinite))) {
        throw new Error(`${name} path.points must be at least two [x, y] positions`);
      }
    }
    if (stroke.intendedOutput !== undefined && (!Array.isArray(stroke.intendedOutput) ||
        stroke.intendedOutput.length !== 2 ||
        !stroke.intendedOutput.every((value) => typeof value === "number"))) {
      throw new Error(`${name} intendedOutput must be [x, y]`);
    }
  }
}

export function validateFixture(fixture, path = "fixture") {
  if (fixture.schema !== FIXTURE_SCHEMA || !FIXTURE_VERSIONS.includes(fixture.version)) {
    throw new Error(`${path}: expected ${FIXTURE_SCHEMA} v${FIXTURE_VERSIONS.join(" or v")}`);
  }
  const kind = fixture.processor?.kind;
  if (!fixture.id || !KINDS.includes(kind)) {
    throw new Error(`${path}: id and a supported pipeline kind are required`);
  }
  if (fixture.version === 1) {
    if (kind in CHAIN_KINDS && kind !== "cursor-pipeline") {
      throw new Error(`${path}: ${kind} needs fixture version 2`);
    }
    validateLegacySettings(kind, fixture.processor.settings, fixture.processor.policy, path);
  } else {
    validateSource(fixture.source, path);
    if (kind in CHAIN_KINDS) {
      validateChainStages(fixture.processor, path);
    } else {
      validateLegacySettings(kind, { ...fixture.processor.settings, cpi: fixture.source.cpi },
        fixture.processor.policy, path);
    }
  }
  validateEvents(fixture, path);
  validateTruth(fixture.truth, path);
  return fixture;
}

function cursorHeader(fixture, overrideProfile) {
  if (fixture.version === 1) {
    const { cpi, scaleMultiplier, scaleDivisor } = fixture.processor.settings;
    return [`R ${cpi}`, "L usb", "S normalize normalize",
      "S transfer transfer", `P scale-multiplier ${scaleMultiplier}`,
      `P scale-divisor ${scaleDivisor}`, "S quantizer quantizer", "E"];
  }
  const lines = [`R ${fixture.source.cpi}`, `L ${fixture.processor.link ?? "usb"}`];
  for (const stage of resolveCursorStages(fixture, overrideProfile)) {
    lines.push(`S ${stage.type} ${stage.stableId}`);
    for (const [key, value] of Object.entries(stage.settings)) {
      lines.push(`P ${key} ${Number(value)}`);
    }
  }
  if (hasRecordedFlushes(fixture)) lines.push("F");
  lines.push("E");
  return lines;
}

/* A recording's flush events say where firmware ran stage deadlines; replay
 * then runs them only there. */
export const hasRecordedFlushes = (fixture) =>
  fixture.events.some((event) => event[0] === "flush");

function legacyConfiguration(fixture) {
  const settings = fixture.version === 1
    ? fixture.processor.settings
    : { ...fixture.processor.settings, cpi: fixture.source.cpi };
  const kind = fixture.processor.kind;
  if (kind === "composed-scroll") {
    return ["C", settings.cpi, settings.stepsPerMeter, settings.reportIntervalMs,
      settings.idleTimeoutMs, settings.suppressAfterKeypressMs,
      Number(settings.discardUnclassified), settings.engageRatioPercent,
      settings.releaseRatioPercent, settings.activationDistanceMicrometers,
      settings.intentWindowMs, POLICIES[fixture.processor.policy]];
  }
  if (kind === "composed-text") {
    return ["C", settings.cpi, settings.engageRatioPercent, settings.releaseRatioPercent,
      settings.activationDistanceMicrometers, settings.intentWindowMs, settings.idleTimeoutMs,
      Number(settings.discardUnclassified), settings.horizontalThresholdMicrometers,
      settings.verticalThresholdMicrometers, settings.mapperIdleTimeoutMs];
  }
  return ["C", Number(settings.enabled), settings.cpi, settings.activationDistanceMicrometers,
    settings.coherencePercent, settings.qualificationTimeoutMs, settings.idleTimeoutMs,
    settings.suppressAfterKeypressMs];
}

/* Which runner replays a fixture: noisePipeline, scrollPipeline,
 * textPipeline, or cursorPipeline. The cursor runner is the stage-chain
 * runner: it replays cursor, scroll, and text pipeline fixtures. */
export const runnerFor = (fixture) => ({
  "composed-scroll": "scrollPipeline",
  "composed-text": "textPipeline",
  "cursor-pipeline": "cursorPipeline",
  "scroll-pipeline": "cursorPipeline",
  "text-pipeline": "cursorPipeline",
}[fixture.processor.kind] ?? "noisePipeline");

/* Runner input. The chain runner takes microsecond times and sample spans;
 * the other runners take the millisecond arrival times firmware sees. */
export function encodeRunnerInput(fixture, overrideProfile = null) {
  const times = eventTimesUs(fixture);
  if (fixture.processor.kind in CHAIN_KINDS) {
    const lines = cursorHeader(fixture, overrideProfile);
    const useSpan = fixture.version === 2 && fixture.source.span === "sensor";
    fixture.events.forEach((event, index) => {
      const late = lateMs(event) > 0 ? ` ${lateMs(event)}` : "";
      lines.push(event[0] === "motion"
        ? `M ${times[index]} ${event[2]} ${event[3]} ${useSpan ? (event[4] ?? 0) : 0}${late}`
        : `${event[0] === "flush" ? "F" : "K"} ${times[index]}${late}`);
    });
    return `${lines.join("\n")}\n`;
  }
  const lines = [legacyConfiguration(fixture).join(" ")];
  fixture.events.forEach((event, index) => {
    const time = toMilliseconds(times[index]);
    lines.push(event[0] === "motion" ? `M ${time} ${event[2]} ${event[3]}` : `K ${time}`);
  });
  return `${lines.join("\n")}\n`;
}
