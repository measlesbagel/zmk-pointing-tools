import test from "node:test";
import assert from "node:assert/strict";

import { createAgentTools } from "./agent-tools.js";

const target = {
  id: 3,
  label: "cursor-acceleration",
  stableId: "right-cursor-acceleration",
  devicetreePath: "/zpt_right_cursor_acceleration",
  parameters: [
    { id: 0, key: "speed-rise-ms", label: "Speed rise", unit: "ms", minimum: 1, maximum: 200, step: 1,
      compiled: 10, current: 30, description: "Rise time constant.", devicetreeProperty: "speed-rise-ms" },
    { id: 1, label: "Speed fall", unit: "", minimum: 1, maximum: 200, step: 1, compiled: 5, current: 5 },
  ],
};

function context(overrides = {}) {
  /* Frozen: the tools must never reorder or trim the tuner's live timeline. */
  const events = Object.freeze(Array.from({ length: 8 }, (_, index) => ({
    targetId: index % 2 ? 3 : 4, timestamp: index * 10, sequence: index, values: [index],
  })));
  return {
    session: () => ({ connection: "simulator" }),
    targets: () => new Map([[3, target]]),
    devices: () => new Map([
      [0, { id: 0, label: "right-ball", location: 0, reachable: true, settable: true,
        description: { currentCpi: 2052, defaultCpi: 684, stale: false, devicetreePath: "/zpt_right_ball",
          discrete: false, range: { min: 608, max: 4826, step: 38 } } }],
      [1, { id: 1, label: "left-ball", location: 1, reachable: false, settable: true, description: undefined }],
    ]),
    stateEvents: () => events,
    stateStatus: () => ({ dropped: 2, levels: new Map([[3, 2], [4, 0]]), labels: new Map([[4, "right-scroll-intent"]]) }),
    describeEvent: (event) => `event ${event.sequence}`,
    precisionHistory: () => [],
    profile: () => ({ schema: "profile" }),
    devicetree: () => "/ { };",
    ...overrides,
  };
}

const call = async (tools, name, input) =>
  JSON.parse(await tools.find((tool) => tool.name === name).execute(input));

test("every agent tool is a uniquely named read-only tool with an object schema", () => {
  const tools = createAgentTools(context());
  assert.equal(new Set(tools.map(({ name }) => name)).size, tools.length);
  for (const tool of tools) {
    assert.equal(tool.annotations.readOnlyHint, true, tool.name);
    assert.equal(tool.inputSchema.type, "object", tool.name);
  }
});

test("tuning targets report preview state and missing metadata as null", async () => {
  const [described] = await call(createAgentTools(context()), "list_tuning_targets");
  assert.equal(described.stableId, "right-cursor-acceleration");
  const [rise, fall] = described.parameters;
  assert.deepEqual([rise.modified, rise.current, rise.compiled, rise.unit], [true, 30, 10, "ms"]);
  assert.deepEqual([fall.modified, fall.key, fall.unit, fall.description], [false, null, null, null]);
});

test("pointing devices report split location and resolution capabilities", async () => {
  const [right, left] = await call(createAgentTools(context()), "list_pointing_devices");
  assert.equal(right.location, "local");
  assert.deepEqual(right.resolution.cpiRange, { min: 608, max: 4826, step: 38 });
  assert.equal(right.resolution.compiledCpi, 684);
  assert.equal(left.location, "peripheral");
  assert.equal(left.resolution, null);
});

test("telemetry returns the newest events for a stage, oldest first", async () => {
  const tools = createAgentTools(context());
  const byStableId = await call(tools, "get_recent_telemetry", { stage: "right-cursor-acceleration", limit: 2 });
  assert.deepEqual(byStableId.events.map(({ sequence }) => sequence), [5, 7]);
  /* Without a telemetry label a stage is named by its stable id. */
  assert.deepEqual(byStableId.stages, [{ stage: "right-cursor-acceleration", level: "every-frame" }]);
  const byLabel = await call(tools, "get_recent_telemetry", { stage: "right-scroll-intent" });
  assert.deepEqual(byLabel.events.map(({ sequence }) => sequence), [0, 2, 4, 6]);
  assert.equal(byLabel.events[0].summary, "event 0");
  const all = await call(tools, "get_recent_telemetry", { limit: 9999 });
  assert.equal(all.events.length, 8);
  assert.equal(all.droppedByFirmware, 2);
});

test("the config handoff fails with the profile's reason when it isn't ready", async () => {
  const tools = createAgentTools(context({ profile: () => { throw new Error("metadata missing"); } }));
  await assert.rejects(tools.find(({ name }) => name === "get_config_handoff").execute(), /metadata missing/);
  const ready = await call(createAgentTools(context()), "get_config_handoff");
  assert.equal(ready.devicetreeSnippet, "/ { };");
});
