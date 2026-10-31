/* Tools an AI agent can call to work with the tuner (#115), exposed through
 * WebMCP by agent-bridge.js. They describe the connected keyboard and what
 * the tuner has measured, and change nothing: no previews, no telemetry
 * levels. Everything comes from the keyboard and this browser, so the tools
 * know nothing about any particular board or config repository. */

const TELEMETRY_DEFAULT = 100;
const TELEMETRY_LIMIT = 500;
const LEVELS = ["off", "decisions", "every-frame"];

/* Every result is a JSON object: the relay widget passes an object on as
 * structured content, but leaves anything else, such as an array, as text
 * the agent has to parse again. */
const json = (value) => JSON.stringify(value, null, 2);
const noInput = { type: "object", properties: {} };

function describeParameter(parameter) {
  return {
    id: parameter.id,
    key: parameter.key ?? null,
    label: parameter.label,
    unit: parameter.unit || null,
    minimum: parameter.minimum,
    maximum: parameter.maximum,
    step: parameter.step,
    compiled: parameter.compiled,
    current: parameter.current,
    modified: parameter.current !== parameter.compiled,
    description: parameter.description ?? null,
    devicetreeProperty: parameter.devicetreeProperty ?? null,
  };
}

function describeTarget(target) {
  return {
    id: target.id,
    label: target.label,
    stableId: target.stableId ?? null,
    devicetreePath: target.devicetreePath ?? null,
    parameters: target.parameters.map(describeParameter),
  };
}

function describeDevice(device) {
  const { description } = device;
  return {
    id: device.id,
    stableId: device.label,
    location: device.location === 0 ? "local" : "peripheral",
    reachable: device.reachable,
    settable: device.settable,
    resolution: description
      ? {
        currentCpi: description.currentCpi,
        compiledCpi: description.defaultCpi,
        stale: description.stale,
        devicetreePath: description.devicetreePath,
        ...(description.discrete ? { supportedCpi: description.values } : {}),
        ...(description.range ? { cpiRange: description.range } : {}),
      }
      : null,
  };
}

/* The newest telemetry events, oldest first, optionally for one stage
 * matched by label or stable id. */
function recentTelemetry(context, { stage, limit } = {}) {
  const count = Math.min(Math.max(Math.trunc(Number(limit) || TELEMETRY_DEFAULT), 1), TELEMETRY_LIMIT);
  const status = context.stateStatus();
  const stableIds = new Map([...context.targets().values()].map(({ id, stableId }) => [id, stableId]));
  const name = (targetId) => status.labels.get(targetId) || stableIds.get(targetId) || `target ${targetId}`;
  const matches = (targetId) => !stage || stage === name(targetId) || stage === stableIds.get(targetId);
  const events = context.stateEvents().filter(({ targetId }) => matches(targetId)).slice(-count);
  return {
    capturedEvents: context.stateEvents().length,
    droppedByFirmware: status.dropped,
    stages: [...status.levels.entries()]
      .filter(([targetId]) => matches(targetId))
      .map(([targetId, level]) => ({ stage: name(targetId), level: LEVELS[level] ?? `level ${level}` })),
    events: events.map((event) => ({
      stage: name(event.targetId),
      timestampMs: event.timestamp,
      sequence: event.sequence,
      summary: context.describeEvent(event),
      values: event.values,
    })),
  };
}

export function createAgentTools(context) {
  return [
    {
      name: "get_session",
      description: "Start here. Says whether a keyboard or the simulator is connected, over which transport and protocol version, and how many tuning targets, pointing devices, modified previews and captured telemetry events the tuner has. Every value the tuner shows is a temporary preview until the user commits it to their own keyboard config.",
      inputSchema: noInput,
      annotations: { readOnlyHint: true },
      execute: async () => json(context.session()),
    },
    {
      name: "list_tuning_targets",
      description: "List the keyboard's tunable pipeline stages. Each has a stable id that matches its devicetree node, and parameters with label, unit, range, step, compiled value, current preview value, help text and devicetree property.",
      inputSchema: noInput,
      annotations: { readOnlyHint: true },
      execute: async () => json({ targets: [...context.targets().values()].map(describeTarget) }),
    },
    {
      name: "list_pointing_devices",
      description: "List the keyboard's pointing sensors with their location in the split, whether they're reachable, and their current, compiled and supported resolution (CPI).",
      inputSchema: noInput,
      annotations: { readOnlyHint: true },
      execute: async () => json({ devices: [...context.devices().values()].map(describeDevice) }),
    },
    {
      name: "get_recent_telemetry",
      description: `Read the stage telemetry the tuner has already captured, oldest first, with a readable summary of each event. Each stage's detail level (off, decisions, every-frame) is set by the user in the Stage decisions panel; this tool doesn't change it. The tuner keeps the latest 2000 events. Returns at most ${TELEMETRY_LIMIT}.`,
      inputSchema: {
        type: "object",
        properties: {
          stage: { type: "string", description: "Only this stage, by label or stable id." },
          limit: { type: "integer", minimum: 1, maximum: TELEMETRY_LIMIT, description: `Newest events to return, default ${TELEMETRY_DEFAULT}.` },
        },
      },
      annotations: { readOnlyHint: true },
      execute: async (input) => json(recentTelemetry(context, input)),
    },
    {
      name: "get_precision_results",
      description: "Read the saved Small-target precision runs from the playground, newest last: each run's label, acquire mode, time, Fitts throughput, and per target size the median time, corrections, overshoot and misses. Run labels are typed by the user.",
      inputSchema: noInput,
      annotations: { readOnlyHint: true, untrustedContentHint: true },
      execute: async () => json({ runs: context.precisionHistory() }),
    },
    {
      name: "get_config_handoff",
      description: "Get the current values as a tuning profile and as a devicetree snippet, to put tested values into the user's own ZMK config repository. Nodes match by stable id; check them against that repository's keymap before committing.",
      inputSchema: noInput,
      annotations: { readOnlyHint: true },
      execute: async () => json({ profile: context.profile(), devicetreeSnippet: context.devicetree() }),
    },
  ];
}
