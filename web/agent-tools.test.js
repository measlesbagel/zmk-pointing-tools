import test from "node:test";
import assert from "node:assert/strict";

import { AgentSession } from "./agent-tools.js";

const makeTarget = () => ({
  id: 3,
  label: "cursor-acceleration",
  stableId: "right-cursor-acceleration",
  devicetreePath: "/zpt_right_cursor_acceleration",
  parameters: [
    { id: 0, key: "speed-rise-ms", label: "Speed rise", unit: "ms", minimum: 1, maximum: 200, step: 1,
      compiled: 10, current: 30, description: "Rise time constant.", devicetreeProperty: "speed-rise-ms" },
    { id: 1, label: "Speed fall", unit: "", minimum: 1, maximum: 200, step: 1, compiled: 5, current: 5 },
  ],
});

/* A connected keyboard: previews change values unless the firmware is set
 * to reject them, and every request is answered at once. */
function context(overrides = {}) {
  const target = makeTarget();
  const targets = new Map([[3, target]]);
  const devices = new Map([
    [0, { id: 0, label: "right-ball", location: 0, reachable: true, settable: true,
      description: { currentCpi: 2052, defaultCpi: 684, stale: false, devicetreePath: "/zpt_right_ball",
        discrete: false, range: { min: 608, max: 4826, step: 38 } } }],
    [1, { id: 1, label: "left-ball", location: 1, reachable: false, settable: true, description: undefined }],
  ]);
  /* Frozen: the tools must never reorder or trim the tuner's live timeline. */
  const events = Object.freeze(Array.from({ length: 8 }, (_, index) => ({
    targetId: index % 2 ? 3 : 4, timestamp: index * 10, sequence: index, values: [index],
  })));
  return {
    session: () => ({ connection: "simulator" }),
    targets: () => targets,
    devices: () => devices,
    stateEvents: () => events,
    stateStatus: () => ({ dropped: 2, levels: new Map([[3, 2], [4, 0]]), labels: new Map([[4, "right-scroll-intent"]]) }),
    describeEvent: (event) => `event ${event.sequence}`,
    precisionHistory: () => [],
    profile: () => ({ schema: "profile" }),
    devicetree: () => "/ { };",
    reject: false,
    batches: [],
    previewValues(targetId, values) {
      this.batches.push({ targetId, values });
      /* Like a refreshed description, new parameter objects replace the old. */
      if (!this.reject) {
        const item = targets.get(targetId);
        item.parameters = item.parameters.map((parameter) => {
          const update = values.find(({ parameterId }) => parameterId === parameter.id);
          return update ? { ...parameter, current: update.value } : parameter;
        });
      }
    },
    previewDevice: (deviceId, cpi) => { devices.get(deviceId).description.currentCpi = cpi; },
    resetTarget: (targetId) => targets.get(targetId).parameters.forEach((parameter) => { parameter.current = parameter.compiled; }),
    resetAll() { for (const id of targets.keys()) this.resetTarget(id); },
    settled: async () => true,
    ...overrides,
  };
}

const createAgentTools = (fake) => new AgentSession(fake).tools;
const call = async (tools, name, input) =>
  JSON.parse(await tools.find((tool) => tool.name === name).execute(input));
const values = (fake) => fake.targets().get(3).parameters.map(({ current }) => current);
const cpi = (fake) => fake.devices().get(0).description.currentCpi;

const CHANGING = ["set_tuning_values", "preview_device_cpi", "reset_previews", "save_snapshot", "apply_snapshot", "ask_user"];

test("only the tools that change previews or ask the user are marked as writing", async () => {
  const tools = createAgentTools(context());
  assert.equal(new Set(tools.map(({ name }) => name)).size, tools.length);
  for (const tool of tools) {
    assert.equal(tool.inputSchema.type, "object", tool.name);
    assert.equal(tool.annotations.readOnlyHint, !CHANGING.includes(tool.name), tool.name);
  }
  assert.deepEqual(tools.filter(({ annotations }) => annotations.consequentialHint).map(({ name }) => name), ["reset_previews"]);
});

test("tools without required input return a JSON object", async () => {
  for (const tool of createAgentTools(context()).filter(({ inputSchema }) => !inputSchema.required)) {
    /* The relay only passes plain objects on as structured content. */
    const output = JSON.parse(await tool.execute({}));
    assert.ok(output && typeof output === "object" && !Array.isArray(output), tool.name);
  }
});

test("tuning targets report preview state and missing metadata as null", async () => {
  const { targets: [described] } = await call(createAgentTools(context()), "list_tuning_targets");
  assert.equal(described.stableId, "right-cursor-acceleration");
  const [rise, fall] = described.parameters;
  assert.deepEqual([rise.modified, rise.current, rise.compiled, rise.unit], [true, 30, 10, "ms"]);
  assert.deepEqual([fall.modified, fall.key, fall.unit, fall.description], [false, null, null, null]);
});

test("pointing devices report split location and resolution capabilities", async () => {
  const { devices: [right, left] } = await call(createAgentTools(context()), "list_pointing_devices");
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

test("set_tuning_values checks values before sending one batch and reports what the keyboard took", async () => {
  const fake = context();
  const tools = createAgentTools(fake);
  await assert.rejects(call(tools, "set_tuning_values", { target: "right-cursor-acceleration", values: { "speed-rise-ms": 201 } }), /1 to 200/);
  await assert.rejects(call(tools, "set_tuning_values", { target: "right-cursor-acceleration", values: { nope: 2 } }), /no parameter "nope"/);
  await assert.rejects(call(tools, "set_tuning_values", { target: "left-cursor", values: { 0: 2 } }), /No tuning target/);
  assert.equal(fake.batches.length, 0);
  /* By key and by numeric id, in one atomic batch. */
  const result = await call(tools, "set_tuning_values", { target: "cursor-acceleration", values: { "speed-rise-ms": 50, 1: 8 } });
  assert.deepEqual(fake.batches, [{ targetId: 3, values: [{ parameterId: 0, value: 50 }, { parameterId: 1, value: 8 }] }]);
  assert.deepEqual(result.values.map(({ previous, current }) => [previous, current]), [[30, 50], [5, 8]]);
  assert.match(result.summary, /^Set right-cursor-acceleration speed-rise-ms 30 → 50, right-cursor-acceleration Speed fall 5 → 8$/);
});

test("a rejected batch fails and leaves nothing to revert", async () => {
  const fake = context({ reject: true });
  const session = new AgentSession(fake);
  await assert.rejects(call(session.tools, "set_tuning_values", { target: "right-cursor-acceleration", values: { "speed-rise-ms": 50 } }), /rejected/);
  assert.equal(session.changed, false);
});

test("revert restores the values from before the agent's first change", async () => {
  const fake = context();
  const session = new AgentSession(fake);
  await call(session.tools, "set_tuning_values", { target: "right-cursor-acceleration", values: { "speed-rise-ms": 50 } });
  await call(session.tools, "preview_device_cpi", { device: "right-ball", cpi: 1216 });
  await call(session.tools, "set_tuning_values", { target: "right-cursor-acceleration", values: { "speed-rise-ms": 70, 1: 9 } });
  assert.deepEqual([values(fake), cpi(fake), session.changed], [[70, 9], 1216, true]);
  const { changes, remaining } = await session.revert();
  assert.deepEqual([values(fake), cpi(fake), session.changed, remaining], [[30, 5], 2052, false, []]);
  assert.equal(changes.length, 3);
  /* Changing back by hand also leaves nothing to revert. */
  await call(session.tools, "preview_device_cpi", { device: "right-ball", cpi: 1216 });
  await call(session.tools, "preview_device_cpi", { device: "right-ball", cpi: 2052 });
  assert.equal(session.changed, false);
});

test("snapshots switch between saved values", async () => {
  const fake = context();
  const session = new AgentSession(fake);
  await call(session.tools, "save_snapshot", { name: "A" });
  await call(session.tools, "set_tuning_values", { target: "right-cursor-acceleration", values: { 1: 12 } });
  await call(session.tools, "preview_device_cpi", { device: "right-ball", cpi: 1216 });
  await call(session.tools, "save_snapshot", { name: "B" });
  const applied = await call(session.tools, "apply_snapshot", { name: "A" });
  assert.deepEqual([values(fake), cpi(fake)], [[30, 5], 2052]);
  /* Only the parameter that differs is sent. */
  assert.deepEqual(fake.batches.at(-1), { targetId: 3, values: [{ parameterId: 1, value: 5 }] });
  assert.equal(applied.changes.length, 2);
  const { snapshots } = await call(session.tools, "list_snapshots");
  assert.deepEqual(snapshots, [{ name: "A", matchesCurrent: true }, { name: "B", matchesCurrent: false }]);
  await session.applySnapshot("B");
  assert.deepEqual([values(fake), cpi(fake)], [[30, 12], 1216]);
  await assert.rejects(call(session.tools, "apply_snapshot", { name: "C" }), /No snapshot "C"/);
});

test("reset_previews restores compiled values on every target and device", async () => {
  const fake = context();
  const session = new AgentSession(fake);
  await assert.rejects(call(session.tools, "preview_device_cpi", { device: "right-ball", cpi: 5000 }), /608 to 4826/);
  await assert.rejects(call(session.tools, "preview_device_cpi", { device: "left-ball", cpi: 800 }), /can't be previewed/);
  const reset = await call(session.tools, "reset_previews", {});
  assert.deepEqual([values(fake), cpi(fake)], [[10, 5], 684]);
  assert.match(reset.summary, /^Reset everything: right-cursor-acceleration speed-rise-ms 30 → 10, right-ball cpi 2052 → 684$/);
  await session.revert();
  assert.deepEqual([values(fake), cpi(fake)], [[30, 5], 2052]);
});

test("ask_user returns at once and the answer arrives through polling", async () => {
  const session = new AgentSession(context());
  const asked = await call(session.tools, "ask_user", { question: "Rate smoothness 1-5", choices: ["1", "5"] });
  assert.equal(asked.status, "pending");
  const pending = await call(session.tools, "get_user_answer", { questionId: asked.questionId, waitSeconds: 0 });
  assert.equal(pending.status, "pending");
  const waiting = call(session.tools, "get_user_answer", { questionId: asked.questionId, waitSeconds: 30 });
  session.answer(asked.questionId, "4, a bit floaty");
  assert.deepEqual(await waiting, { questionId: asked.questionId, question: "Rate smoothness 1-5", status: "answered", answer: "4, a bit floaty" });
  const second = await call(session.tools, "ask_user", { question: "Again?" });
  session.close();
  assert.equal((await call(session.tools, "get_user_answer", { questionId: second.questionId })).status, "cancelled");
});
