/* SPDX-License-Identifier: MIT */

export const FIXTURE_SCHEMA = "zmk-pointing-tools/trace-fixture";
export const FIXTURE_VERSION = 1;

const POLICIES = { free: 0, adaptive: 1, horizontal: 2, vertical: 3 };

function requireInteger(value, name, minimum = Number.MIN_SAFE_INTEGER) {
  if (!Number.isSafeInteger(value) || value < minimum) {
    throw new Error(`${name} must be an integer >= ${minimum}`);
  }
}

export function validateFixture(fixture, path = "fixture") {
  if (fixture.schema !== FIXTURE_SCHEMA || fixture.version !== FIXTURE_VERSION) {
    throw new Error(`${path}: expected ${FIXTURE_SCHEMA} v${FIXTURE_VERSION}`);
  }
  if (!fixture.id || !["adaptive-scroll", "text-navigation", "noise-filter",
    "composed-scroll", "composed-text", "composed-noise",
    "cursor-pipeline"].includes(fixture.processor?.kind)) {
    throw new Error(`${path}: id and a supported processor are required`);
  }

  const settings = fixture.processor.settings;
  if (fixture.processor.kind === "adaptive-scroll") {
    for (const key of ["scaleMultiplier", "scaleDivisor", "reportIntervalMs", "idleTimeoutMs",
      "suppressAfterKeypressMs", "engageRatioPercent", "releaseRatioPercent",
      "activationDistance", "intentWindowMs"]) {
      requireInteger(settings?.[key], `${path}: settings.${key}`,
        key === "suppressAfterKeypressMs" ? 0 : 1);
    }
    if (typeof settings.discardUnclassified !== "boolean") {
      throw new Error(`${path}: discardUnclassified must be boolean`);
    }
    if (!(fixture.processor.policy in POLICIES)) throw new Error(`${path}: unknown axis policy`);
  } else if (fixture.processor.kind === "composed-scroll") {
    for (const key of ["cpi", "stepsPerMeter", "reportIntervalMs", "idleTimeoutMs",
      "suppressAfterKeypressMs", "engageRatioPercent", "releaseRatioPercent",
      "activationDistanceMicrometers", "intentWindowMs"]) {
      requireInteger(settings?.[key], `${path}: settings.${key}`,
        key === "suppressAfterKeypressMs" ? 0 : 1);
    }
    if (typeof settings.discardUnclassified !== "boolean") {
      throw new Error(`${path}: discardUnclassified must be boolean`);
    }
    if (!(fixture.processor.policy in POLICIES)) throw new Error(`${path}: unknown axis policy`);
  } else if (fixture.processor.kind === "cursor-pipeline") {
    for (const key of ["cpi", "scaleMultiplier", "scaleDivisor", "unitsPerMeter"]) {
      requireInteger(settings?.[key], `${path}: settings.${key}`, 1);
    }
  } else if (fixture.processor.kind === "text-navigation") {
    for (const key of ["horizontalThreshold", "verticalThreshold", "idleTimeoutMs",
      "activationDistance", "engageRatioPercent"]) {
      requireInteger(settings?.[key], `${path}: settings.${key}`, 1);
    }
  } else if (fixture.processor.kind === "composed-text") {
    for (const key of ["cpi", "horizontalThresholdMicrometers", "verticalThresholdMicrometers",
      "idleTimeoutMs", "activationDistanceMicrometers", "engageRatioPercent"]) {
      requireInteger(settings?.[key], `${path}: settings.${key}`, 1);
    }
  } else if (fixture.processor.kind === "composed-noise") {
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
  } else {
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

  if (!Array.isArray(fixture.events) || fixture.events.length === 0) {
    throw new Error(`${path}: events are required`);
  }
  for (const [index, event] of fixture.events.entries()) {
    if (!Array.isArray(event) || !["motion", "keypress"].includes(event[0])) {
      throw new Error(`${path}: invalid event ${index}`);
    }
    requireInteger(event[1], `${path}: event ${index} delta`, 0);
    if (event[0] === "motion") {
      if (event.length !== 4) {
        throw new Error(`${path}: motion event ${index} needs delta, x, and y`);
      }
      requireInteger(event[2], `${path}: event ${index} x`);
      requireInteger(event[3], `${path}: event ${index} y`);
    } else if (event.length !== 2) {
      throw new Error(`${path}: keypress event ${index} only accepts delta`);
    }
    if (fixture.processor.kind === "text-navigation" && event[0] !== "motion") {
      throw new Error(`${path}: text navigation fixtures only support motion events`);
    }
  }
  return fixture;
}

export function encodeRunnerInput(fixture) {
  const settings = fixture.processor.settings;
  const configuration = fixture.processor.kind === "adaptive-scroll"
    ? ["C", settings.scaleMultiplier, settings.scaleDivisor, settings.reportIntervalMs,
      settings.idleTimeoutMs, settings.suppressAfterKeypressMs,
      Number(settings.discardUnclassified), settings.engageRatioPercent,
      settings.releaseRatioPercent, settings.activationDistance, settings.intentWindowMs,
      POLICIES[fixture.processor.policy]]
    : fixture.processor.kind === "composed-scroll"
      ? ["C", settings.cpi, settings.stepsPerMeter, settings.reportIntervalMs,
        settings.idleTimeoutMs, settings.suppressAfterKeypressMs,
        Number(settings.discardUnclassified), settings.engageRatioPercent,
        settings.releaseRatioPercent, settings.activationDistanceMicrometers,
        settings.intentWindowMs, POLICIES[fixture.processor.policy]]
    : fixture.processor.kind === "text-navigation"
      ? ["C", settings.horizontalThreshold, settings.verticalThreshold, settings.idleTimeoutMs,
        settings.activationDistance, settings.engageRatioPercent]
      : fixture.processor.kind === "composed-text"
        ? ["C", settings.cpi, settings.horizontalThresholdMicrometers,
          settings.verticalThresholdMicrometers, settings.idleTimeoutMs,
          settings.activationDistanceMicrometers, settings.engageRatioPercent]
        : fixture.processor.kind === "cursor-pipeline"
          ? ["C", settings.cpi, settings.scaleMultiplier, settings.scaleDivisor,
            settings.unitsPerMeter]
          : ["C", Number(settings.enabled), settings.cpi, settings.activationDistanceMicrometers,
            settings.coherencePercent, settings.qualificationTimeoutMs, settings.idleTimeoutMs,
            settings.suppressAfterKeypressMs];

  const lines = [configuration.join(" ")];
  let timestamp = 0;
  for (const event of fixture.events) {
    timestamp += event[1];
    lines.push(event[0] === "motion"
      ? `M ${timestamp} ${event[2]} ${event[3]}`
      : `K ${timestamp}`);
  }
  return `${lines.join("\n")}\n`;
}
