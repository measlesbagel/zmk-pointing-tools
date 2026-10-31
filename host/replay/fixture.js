/* SPDX-License-Identifier: MIT */

export const FIXTURE_SCHEMA = "zmk-pointing-tools/trace-fixture";
export const FIXTURE_VERSIONS = [1, 2];
export const PROFILE_SCHEMA = "zmk-pointing-tools/tuning-profile";
export const PROFILE_VERSION = 1;

const POLICIES = { free: 0, adaptive: 1, horizontal: 2, vertical: 3 };
const KINDS = ["composed-scroll", "composed-text", "composed-noise", "cursor-pipeline"];
export const CURSOR_STAGE_TYPES = ["orientation", "normalize", "gate", "acceleration",
  "transfer", "quantizer", "coalescer"];
const STROKE_ACTIVITIES = ["slow-aim", "tremor", "flick", "aim", "scroll", "text",
  "post-typing"];

function requireInteger(value, name, minimum = Number.MIN_SAFE_INTEGER) {
  if (!Number.isSafeInteger(value) || value < minimum) {
    throw new Error(`${name} must be an integer >= ${minimum}`);
  }
}

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
  for (const target of profile.targets) {
    if (typeof target?.stableId !== "string" || !Array.isArray(target.parameters)) {
      throw new Error(`${path}: malformed target`);
    }
    for (const parameter of target.parameters) {
      if (typeof parameter?.key !== "string") throw new Error(`${path}: malformed parameter`);
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

/* The cursor stages with every settings source merged: inline settings,
 * then the fixture's embedded profile, then an override profile. */
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

function validateCursorStages(processor, path) {
  if (!Array.isArray(processor.stages) || processor.stages.length === 0) {
    throw new Error(`${path}: cursor-pipeline v2 needs stages`);
  }
  if (!["usb", "ble"].includes(processor.link)) {
    throw new Error(`${path}: processor.link must be usb or ble`);
  }
  if (processor.profile !== undefined) validateProfile(processor.profile, `${path}: profile`);
  for (const [index, stage] of processor.stages.entries()) {
    if (!CURSOR_STAGE_TYPES.includes(stage?.type) || typeof stage.stableId !== "string" ||
        !/^[A-Za-z0-9_.-]{1,63}$/.test(stage.stableId)) {
      throw new Error(`${path}: stage ${index} needs a known type and a stable id`);
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
  const motionLengths = fixture.version === 1 ? [4] : [4, 5];
  for (const [index, event] of fixture.events.entries()) {
    if (!Array.isArray(event) || !["motion", "keypress"].includes(event[0])) {
      throw new Error(`${path}: invalid event ${index}`);
    }
    requireInteger(event[1], `${path}: event ${index} delta`, 0);
    if (event[0] === "motion") {
      if (!motionLengths.includes(event.length)) {
        throw new Error(`${path}: motion event ${index} needs delta, x, and y` +
          (fixture.version === 1 ? "" : ", and optionally a span"));
      }
      requireInteger(event[2], `${path}: event ${index} x`);
      requireInteger(event[3], `${path}: event ${index} y`);
      if (event.length === 5) requireInteger(event[4], `${path}: event ${index} span`, 0);
    } else if (event.length !== 2) {
      throw new Error(`${path}: keypress event ${index} only accepts delta`);
    }
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
    validateLegacySettings(kind, fixture.processor.settings, fixture.processor.policy, path);
  } else {
    validateSource(fixture.source, path);
    if (kind === "cursor-pipeline") {
      validateCursorStages(fixture.processor, path);
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
  const lines = [`R ${fixture.source.cpi}`, `L ${fixture.processor.link}`];
  for (const stage of resolveCursorStages(fixture, overrideProfile)) {
    lines.push(`S ${stage.type} ${stage.stableId}`);
    for (const [key, value] of Object.entries(stage.settings)) {
      lines.push(`P ${key} ${Number(value)}`);
    }
  }
  lines.push("E");
  return lines;
}

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

/* Runner input. The cursor runner takes microsecond times and sample spans;
 * the other runners take the millisecond arrival times firmware sees. */
export function encodeRunnerInput(fixture, overrideProfile = null) {
  const times = eventTimesUs(fixture);
  if (fixture.processor.kind === "cursor-pipeline") {
    const lines = cursorHeader(fixture, overrideProfile);
    const useSpan = fixture.version === 2 && fixture.source.span === "sensor";
    fixture.events.forEach((event, index) => {
      lines.push(event[0] === "motion"
        ? `M ${times[index]} ${event[2]} ${event[3]} ${useSpan ? (event[4] ?? 0) : 0}`
        : `K ${times[index]}`);
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
