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

const makeDevices = () => [
  [0, { id: 0, label: "right-ball", location: 0, reachable: true, settable: true,
    description: { currentCpi: 2052, defaultCpi: 684, stale: false, devicetreePath: "/zpt_right_ball",
      discrete: false, range: { min: 608, max: 4826, step: 38 } } }],
  [1, { id: 1, label: "left-ball", location: 1, reachable: false, settable: true, description: undefined }],
];

/* A connected keyboard: previews change values unless the firmware is set
 * to reject them, and every request is answered at once unless it's set to
 * answer late. disconnect() and connect() clear and refill the tuner like
 * the app does. */
function context(overrides = {}) {
  const targets = new Map([[3, makeTarget()]]);
  const devices = new Map(makeDevices());
  /* Frozen: the tools must never reorder or trim the tuner's live timeline. */
  const events = Object.freeze(Array.from({ length: 8 }, (_, index) => ({
    targetId: index % 2 ? 3 : 4, timestamp: index * 10, sequence: index, values: [index],
  })));
  return {
    session: () => ({ connection: "simulator" }),
    targets: () => targets,
    devices: () => devices,
    stateEvents: () => events,
    stateStatus() { return { dropped: this.status.dropped, levels: new Map([[3, 2], [4, 0]]), labels: new Map([[4, "right-scroll-intent"]]) }; },
    describeEvent: (event) => `event ${event.sequence}`,
    eventKind: (event) => (event.values[0] === 128 ? "frame" : "qualified"),
    status: { dropped: 2 },
    async refreshTelemetryStatus() { return this.status.dropped; },
    precisionHistory: () => [],
    profile: () => ({ schema: "profile" }),
    devicetree: () => "/ { };",
    link: "keyboard-1",
    connection() { return this.link; },
    disconnect() { this.link = null; targets.clear(); devices.clear(); },
    connect(link) {
      this.link = link;
      targets.set(3, makeTarget());
      for (const [id, device] of makeDevices()) devices.set(id, device);
    },
    reject: false,
    late: false,
    unanswered: [],
    batches: [],
    answer(apply) { if (this.late) this.unanswered.push(apply); else apply(); },
    answerLate() { for (const apply of this.unanswered.splice(0)) apply(); },
    previewValues(targetId, values) {
      this.batches.push({ targetId, values });
      /* Like a refreshed description, new parameter objects replace the old. */
      this.answer(() => {
        const item = targets.get(targetId);
        if (this.reject || !item) return;
        item.parameters = item.parameters.map((parameter) => {
          const update = values.find(({ parameterId }) => parameterId === parameter.id);
          return update ? { ...parameter, current: update.value } : parameter;
        });
      });
    },
    previewDevice(deviceId, cpi) { this.answer(() => { devices.get(deviceId).description.currentCpi = cpi; }); },
    resetTarget: (targetId) => targets.get(targetId).parameters.forEach((parameter) => { parameter.current = parameter.compiled; }),
    resetAll() { for (const id of targets.keys()) this.resetTarget(id); },
    async settled() { return !this.late; },
    ...overrides,
  };
}

const createAgentTools = (fake) => new AgentSession(fake).tools;
const call = async (tools, name, input) =>
  JSON.parse(await tools.find((tool) => tool.name === name).execute(input));
const values = (fake) => fake.targets().get(3).parameters.map(({ current }) => current);
const cpi = (fake) => fake.devices().get(0).description.currentCpi;

const CHANGING = ["set_tuning_values", "preview_device_cpi", "reset_previews", "save_snapshot", "apply_snapshot", "ask_user", "request_run"];

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
});

test("values set back by hand leave nothing to revert", async () => {
  const fake = context();
  const session = new AgentSession(fake);
  await call(session.tools, "preview_device_cpi", { device: "right-ball", cpi: 1216 });
  assert.equal(session.changed, true);
  /* The user's own Preview, outside the agent tools. */
  fake.devices().get(0).description.currentCpi = 2052;
  assert.equal(session.changed, false);
  assert.equal((await call(session.tools, "get_session")).agent.changedValues, false);
});

test("revert after a disconnect fails instead of reporting success", async () => {
  const fake = context();
  const session = new AgentSession(fake);
  await call(session.tools, "set_tuning_values", { target: "right-cursor-acceleration", values: { "speed-rise-ms": 50 } });
  fake.disconnect();
  assert.equal(session.changed, false);
  await assert.rejects(session.revert(), /Connect the keyboard/);
  await assert.rejects(call(session.tools, "set_tuning_values", { target: "x", values: { a: 1 } }), /Connect the keyboard/);
});

test("a change whose connection goes away while waiting is unconfirmed", async () => {
  const fake = context({ async settled() { this.disconnect(); return true; } });
  const result = await call(new AgentSession(fake).tools, "set_tuning_values",
    { target: "right-cursor-acceleration", values: { "speed-rise-ms": 50 } });
  assert.match(result.warning, /hasn't confirmed/);
  assert.deepEqual(result.values.map(({ previous, current }) => [previous, current]), [[30, null]]);
});

test("a change the keyboard confirms late can still be reverted", async () => {
  const fake = context({ late: true });
  const session = new AgentSession(fake);
  const result = await call(session.tools, "set_tuning_values", { target: "right-cursor-acceleration", values: { "speed-rise-ms": 50 } });
  assert.match(result.warning, /hasn't confirmed/);
  fake.answerLate();
  assert.deepEqual([values(fake), session.changed], [[50, 5], true]);
  /* An unconfirmed revert keeps the revert point too. */
  const unconfirmed = await session.revert();
  assert.equal(unconfirmed.confirmed, false);
  fake.late = false;
  const reverted = await session.revert();
  assert.deepEqual([values(fake), reverted.confirmed, session.changed], [[30, 5], true, false]);
});

test("revert keeps its point while a target's values are still loading", async () => {
  const fake = context();
  const session = new AgentSession(fake);
  await call(session.tools, "set_tuning_values", { target: "right-cursor-acceleration", values: { "speed-rise-ms": 50 } });
  const target = fake.targets().get(3);
  const loaded = target.parameters;
  target.parameters = [];
  await session.revert();
  target.parameters = loaded;
  assert.equal(session.changed, true);
  await session.revert();
  assert.deepEqual([values(fake), session.changed], [[30, 5], false]);
});

test("revert restores a device that was described after the agent's first change", async () => {
  const fake = context();
  /* The peripheral's description hasn't arrived yet. */
  const peripheral = fake.devices().get(1);
  const session = new AgentSession(fake);
  await call(session.tools, "set_tuning_values", { target: "right-cursor-acceleration", values: { "speed-rise-ms": 50 } });
  peripheral.description = { currentCpi: 800, defaultCpi: 800, stale: false, discrete: true, values: [400, 800, 1600] };
  await call(session.tools, "preview_device_cpi", { device: "left-ball", cpi: 1600 });
  const { remaining } = await session.revert();
  assert.deepEqual([values(fake), peripheral.description.currentCpi, remaining, session.changed], [[30, 5], 800, [], false]);
});

test("a queued change fails if the connection changed while it waited", async () => {
  /* The first change waits until released; later ones are answered at once. */
  let release;
  const blocked = new Promise((resolve) => { release = () => resolve(true); });
  let waits = 0;
  const fake = context({ settled: () => (waits++ ? Promise.resolve(true) : blocked) });
  const tools = createAgentTools(fake);
  const first = call(tools, "set_tuning_values", { target: "right-cursor-acceleration", values: { "speed-rise-ms": 50 } });
  const second = call(tools, "set_tuning_values", { target: "right-cursor-acceleration", values: { "speed-rise-ms": 70 } });
  await new Promise((resolve) => setTimeout(resolve));
  fake.connect("simulator-9");
  release();
  await first;
  await assert.rejects(second, /connection changed while this call waited/);
  assert.equal(fake.batches.length, 1);
});

test("a revert point and snapshots never reach another connection", async () => {
  const fake = context();
  const session = new AgentSession(fake);
  await call(session.tools, "save_snapshot", { name: "A" });
  await call(session.tools, "set_tuning_values", { target: "right-cursor-acceleration", values: { "speed-rise-ms": 50 } });
  /* Same ids, other keyboard (or the simulator, or a restart). */
  fake.connect("simulator-7");
  fake.targets().get(3).parameters[0].current = 120;
  const sent = fake.batches.length;
  const reverted = await session.revert();
  assert.deepEqual([reverted.changes, fake.batches.length, values(fake), session.changed], [[], sent, [120, 5], false]);
  await assert.rejects(call(session.tools, "apply_snapshot", { name: "A" }), /No snapshot "A"/);
  assert.deepEqual((await call(session.tools, "list_snapshots")).snapshots, []);
});

test("parallel changes report the values each one replaced", async () => {
  const fake = context();
  const tools = createAgentTools(fake);
  const [first, second] = await Promise.all([50, 70].map((value) =>
    call(tools, "set_tuning_values", { target: "right-cursor-acceleration", values: { "speed-rise-ms": value } })));
  assert.deepEqual([first.values[0].previous, second.values[0].previous, values(fake)[0]], [30, 50, 70]);
});

test("set_tuning_values rejects arrays and parameters named twice", async () => {
  const fake = context();
  const tools = createAgentTools(fake);
  await assert.rejects(call(tools, "set_tuning_values", { target: "right-cursor-acceleration", values: [20] }), /object keyed/);
  await assert.rejects(call(tools, "set_tuning_values", { target: "right-cursor-acceleration", values: { "speed-rise-ms": 50, 0: 70 } }), /twice/);
  /* Parameters without a key yet are listed by id. */
  await assert.rejects(call(tools, "set_tuning_values", { target: "right-cursor-acceleration", values: { x: 1 } }), /speed-rise-ms, 1 \(Speed fall\)/);
  assert.equal(fake.batches.length, 0);
});

test("an earlier device error only fails a confirmed preview", async () => {
  const fake = context({ late: true });
  fake.devices().get(0).error = "Preview rejected: internal error.";
  const result = await call(createAgentTools(fake), "preview_device_cpi", { device: "right-ball", cpi: 1216 });
  assert.match(result.warning, /hasn't confirmed/);
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
  const open = [];
  for (const question of ["Again?", "And?", "More?"]) open.push(await call(session.tools, "ask_user", { question }));
  await assert.rejects(call(session.tools, "ask_user", { question: "One too many?" }), /3 questions are already waiting/);
  session.close();
  assert.equal((await call(session.tools, "get_user_answer", { questionId: open[0].questionId })).status, "cancelled");
});

test("a requested run attaches to the asked activity only and reports the values it ran with", async () => {
  const fake = context();
  const session = new AgentSession(fake);
  const { runId } = await call(session.tools, "request_run", { kind: "precision", label: "A" });
  await assert.rejects(call(session.tools, "request_run", { kind: "calibration", label: "B" }), /still requested/);
  /* Runs before the user accepts, or of another activity, aren't the agent's. */
  session.runs.started("playground", "cursor-precision");
  session.runs.accept(runId);
  session.runs.started("playground", "cursor-path");
  assert.equal(session.runs.ended("playground", "cursor-path", { completed: true, result: {} }), undefined);
  assert.equal((await call(session.tools, "get_run_result", { runId, waitSeconds: 0 })).status, "accepted");
  session.runs.started("playground", "cursor-precision");
  fake.targets().get(3).parameters[1].current = 9;
  const waiting = call(session.tools, "get_run_result", { runId, waitSeconds: 30 });
  session.runs.ended("playground", "cursor-precision", { completed: true, result: { durationMs: 1000, metrics: { errors: 1, pointerDistance: 10 } } });
  const run = await waiting;
  assert.equal(run.status, "finished");
  assert.deepEqual(run.settings.modified, [{ target: "right-cursor-acceleration", parameter: "speed-rise-ms", value: 30, compiled: 10 },
    { device: "right-ball", parameter: "cpi", value: 2052, compiled: 684 }]);
  assert.deepEqual(run.settingsChangedDuringRun, [{ target: "right-cursor-acceleration", parameter: "Speed fall", from: 5, to: 9 }]);
});

test("compare_runs lines up metrics and their change from the first run", async () => {
  const session = new AgentSession(context());
  const ids = [];
  for (const [label, throughput, time] of [["A", 2, 900], ["B", 2.5, 700]]) {
    const { runId } = await call(session.tools, "request_run", { kind: "precision", label });
    session.runs.accept(runId);
    session.runs.started("playground", "cursor-precision");
    session.runs.ended("playground", "cursor-precision", { completed: true, result: { durationMs: 1, metrics: { errors: 0, pointerDistance: 0 },
      precision: { summary: { totalTimeMs: time, throughput, sizes: [] } } } });
    ids.push(runId);
  }
  const { metrics } = await call(session.tools, "compare_runs", { runIds: ids });
  assert.deepEqual(metrics.find(({ metric }) => metric === "throughput bit/s"), { metric: "throughput bit/s", values: [2, 2.5], changeFromFirst: [0, 0.5] });
  assert.deepEqual(metrics.find(({ metric }) => metric === "total time ms").changeFromFirst, [0, -200]);
  const { runId: declined } = await call(session.tools, "request_run", { kind: "playground", label: "C", activity: "click-drag" });
  session.runs.decline(declined);
  assert.equal((await call(session.tools, "get_run_result", { runId: declined })).status, "declined");
  await assert.rejects(call(session.tools, "compare_runs", { runIds: [ids[0], declined] }), /same activity/);
  const { runId: open } = await call(session.tools, "request_run", { kind: "calibration", label: "D" });
  session.close();
  assert.equal((await call(session.tools, "get_run_result", { runId: open })).status, "cancelled");
});

const sample = (targetId, sequence, values = [0]) => ({ targetId, targetKind: 4, event: 1, timestamp: sequence * 8, sequence, values });

test("a telemetry window counts drops from the firmware counter or sequence gaps, whichever is larger", async () => {
  const fake = context({ session: () => ({ connection: "keyboard", transport: "Bluetooth" }) });
  const session = new AgentSession(fake);
  const { windowId } = await call(session.tools, "start_telemetry_window", { label: "slow aims", stages: ["right-cursor-acceleration"] });
  /* Samples of other stages fill the sequence; only 13 → 15 is a gap. */
  for (const sequence of [10, 11, 12, 13, 15]) session.windows.observe(sample(sequence % 2 ? 3 : 4, sequence));
  fake.status.dropped = 5;
  let result = await call(session.tools, "stop_telemetry_window", { windowId });
  assert.deepEqual(result.dropped, { samples: 3, firmwareCounter: 3, sequenceGaps: 1 });
  assert.equal(result.stages[0].samples, 3);
  assert.match(result.warnings.join(" "), /3 samples were dropped over Bluetooth/);
  session.windows.observe(sample(3, 17));
  result = await call(session.tools, "get_telemetry_window", { windowId, includeSamples: true, limit: 2, offset: 1 });
  assert.deepEqual(result.rawSamples.items.map(({ sequence }) => sequence), [13, 15]);
  assert.equal(result.rawSamples.nextOffset, null);
});

test("a telemetry window reports every-frame motion and ends with its connection", async () => {
  const fake = context();
  const session = new AgentSession(fake);
  await assert.rejects(call(session.tools, "start_telemetry_window", { label: "x", stages: ["nope"] }), /No telemetry stage/);
  const { windowId, warnings } = await call(session.tools, "start_telemetry_window", { label: "frames" });
  assert.equal(warnings, undefined);
  const half = 32768;
  session.windows.observe(sample(3, 1, [128, 2, 3 * 65536, 4 * 65536, 800]));
  session.windows.observe(sample(3, 2, [128, 2, -half, 0, 800]));
  fake.disconnect();
  session.windows.observe(sample(3, 3, [128, 2, 65536, 0, 800]));
  const result = await call(session.tools, "get_telemetry_window", { windowId });
  assert.equal(result.stopReason, "the connection ended");
  const { frames } = result.stages.find(({ stage }) => stage === "right-cursor-acceleration");
  assert.deepEqual(frames.bySignal["normalized-motion"], { frames: 2, unit: "mm", sumX: 2.5, sumY: 4, travel: 5.5, maxStep: 5, cpi: [800] });
  assert.equal(frames.intervalMs.median, 8);
});
