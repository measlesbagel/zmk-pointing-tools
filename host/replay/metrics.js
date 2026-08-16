/* SPDX-License-Identifier: MIT */

const INTENTS = ["undecided", "free", "horizontal", "vertical"];

function axisMetrics(outputs, index) {
  const values = outputs.map((output) => output[index]);
  const nonzero = values.filter((value) => value !== 0);
  let directionChanges = 0;
  for (let position = 1; position < nonzero.length; position += 1) {
    if (Math.sign(nonzero[position]) !== Math.sign(nonzero[position - 1])) directionChanges += 1;
  }
  return {
    signedDistance: values.reduce((total, value) => total + value, 0),
    absoluteDistance: values.reduce((total, value) => total + Math.abs(value), 0),
    nonzeroFrames: nonzero.length,
    directionChanges,
  };
}

function outputCadence(outputs) {
  const intervals = outputs.slice(1).map((output, index) => output[0] - outputs[index][0]);
  return {
    meanIntervalMs: intervals.length
      ? Number((intervals.reduce((left, right) => left + right, 0) / intervals.length).toFixed(3))
      : 0,
    maximumIntervalMs: intervals.length ? Math.max(...intervals) : 0,
  };
}

function parseLines(output, fixture, decisionLength, outputLength) {
  const decisions = [];
  const outputs = [];
  for (const line of output.trim().split("\n")) {
    if (!line) continue;
    const fields = line.split("\t");
    const values = fields.slice(1).map(Number);
    if (fields[0] === "D" && values.length === decisionLength) decisions.push(values);
    else if (fields[0] === "O" && values.length === outputLength) outputs.push(values);
    else throw new Error(`${fixture.id}: malformed runner output: ${line}`);
  }
  return { decisions, outputs };
}

export function parseScrollMetrics(output, fixture) {
  const { decisions, outputs } = parseLines(output, fixture, 12, 3);
  const intentFrames = Object.fromEntries(INTENTS.map((intent) => [intent, 0]));
  let intentTransitions = 0;
  for (let index = 0; index < decisions.length; index += 1) {
    intentFrames[INTENTS[decisions[index][1]]] += 1;
    if (index > 0 && decisions[index][1] !== decisions[index - 1][1]) intentTransitions += 1;
  }
  return {
    inputFrames: decisions.length,
    durationMs: fixture.events.reduce((total, event) => total + event[1], 0),
    outputFrames: outputs.length,
    outputCadence: outputCadence(outputs),
    horizontal: axisMetrics(outputs, 1),
    vertical: axisMetrics(outputs, 2),
    intentFrames,
    intentTransitions,
    idleResets: decisions.filter((decision) => decision[10] !== 0).length,
    suppressedFrames: decisions.filter((decision) => decision[11] !== 0).length,
  };
}

export function parseTextMetrics(output, fixture) {
  const { decisions, outputs } = parseLines(output, fixture, 5, 2);
  const intentFrames = Object.fromEntries(INTENTS.map((intent) => [intent, 0]));
  for (const decision of decisions) intentFrames[INTENTS[decision[1]]] += 1;
  const stepNames = ["left", "right", "up", "down"];
  const steps = Object.fromEntries(stepNames.map((name) => [name, 0]));
  for (const emitted of outputs) steps[stepNames[emitted[1]]] += 1;
  steps.total = outputs.length;
  return {
    inputFrames: decisions.length,
    durationMs: fixture.events.reduce((total, event) => total + event[1], 0),
    outputFrames: outputs.length,
    outputCadence: outputCadence(outputs),
    steps,
    intentFrames,
    intentTransitions: decisions.slice(1)
      .filter((decision, index) => decision[1] !== decisions[index][1]).length,
  };
}

export function parseCursorMetrics(output, fixture) {
  const { decisions, outputs } = parseLines(output, fixture, 6, 3);
  return {
    inputFrames: decisions.length,
    durationMs: fixture.events.reduce((total, event) => total + event[1], 0),
    outputFrames: outputs.length,
    outputCadence: outputCadence(outputs),
    horizontal: axisMetrics(outputs, 1),
    vertical: axisMetrics(outputs, 2),
    clippedFrames: decisions.filter((decision) => decision[5] !== 0).length,
  };
}

export function parseNoiseFilterMetrics(output, fixture) {
  const { decisions, outputs } = parseLines(output, fixture, 13, 3);
  const phaseNames = ["idle", "pending", "active", "bypass"];
  const phaseFrames = Object.fromEntries(phaseNames.map((phase) => [phase, 0]));
  for (const decision of decisions) phaseFrames[phaseNames[decision[1]]] += 1;
  return {
    inputFrames: decisions.length,
    durationMs: fixture.events.reduce((total, event) => total + event[1], 0),
    outputFrames: outputs.length,
    outputCadence: outputCadence(outputs),
    horizontal: axisMetrics(outputs, 1),
    vertical: axisMetrics(outputs, 2),
    phaseFrames,
    idleResets: decisions.filter((decision) => decision[6] !== 0).length,
    qualificationResets: decisions.filter((decision) => decision[7] !== 0).length,
    suppressedFrames: decisions.filter((decision) => decision[8] !== 0).length,
    qualifications: decisions.filter((decision) => decision[9] !== 0).length,
    discardedFrames: decisions.filter((decision) => decision[10] !== 0).length,
  };
}

export function compareMetrics(expected, actual, prefix = "") {
  const differences = [];
  for (const [key, value] of Object.entries(expected ?? {})) {
    const path = prefix ? `${prefix}.${key}` : key;
    if (value && typeof value === "object" && !Array.isArray(value)) {
      differences.push(...compareMetrics(value, actual?.[key], path));
    } else if (actual?.[key] !== value) {
      differences.push(
        `${path}: expected ${JSON.stringify(value)}, got ${JSON.stringify(actual?.[key])}`,
      );
    }
  }
  return differences;
}
