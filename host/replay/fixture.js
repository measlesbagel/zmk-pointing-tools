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
  if (!fixture.id || !["adaptive-scroll", "text-navigation"].includes(fixture.processor?.kind)) {
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
  } else {
    for (const key of ["horizontalThreshold", "verticalThreshold", "idleTimeoutMs",
      "activationDistance", "engageRatioPercent"]) {
      requireInteger(settings?.[key], `${path}: settings.${key}`, 1);
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
    : ["C", settings.horizontalThreshold, settings.verticalThreshold, settings.idleTimeoutMs,
      settings.activationDistance, settings.engageRatioPercent];

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

export function importCapture(capture, template, options) {
  const stream = capture.streams?.find(({ key }) => key === options.stream);
  if (!stream || !Array.isArray(capture.samples)) {
    throw new Error(`Stream ${options.stream} is absent from the export`);
  }
  const selected = capture.samples
    .filter(({ key, deviceId, stage }) =>
      key === options.stream || `${deviceId}:${stage}` === options.stream)
    .slice(options.start, options.count === undefined ? undefined : options.start + options.count);
  if (selected.length === 0) throw new Error("Selected frame range is empty");

  let previous;
  const fixture = structuredClone(template);
  fixture.events = selected.map((sample) => {
    const delta = previous === undefined ? 0 : (sample.timestamp - previous) >>> 0;
    previous = sample.timestamp;
    return ["motion", delta, sample.x, sample.y];
  });
  fixture.metadata = {
    ...fixture.metadata,
    capture: {
      source: options.source,
      exportedAt: capture.exportedAt,
      streamKey: options.stream,
      streamLabel: stream.label,
      startFrame: options.start,
      frameCount: selected.length,
    },
  };
  delete fixture.expect;
  return fixture;
}
