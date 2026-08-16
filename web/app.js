import {
  FrameDecoder,
  MESSAGE,
  PROTOCOL_VERSION,
  STATE,
  TUNING,
  encodeFrame,
  encodeStateControl,
  encodeTuningSet,
  encodeTuningSetMany,
  parseAck,
  parseDescribe,
  parseStateSample,
  parseStateStatus,
  parseTuningDescription,
  parseTuningHelp,
  parseTuningParameterMetadata,
  parseTuningResult,
  parseTuningTargetMetadata,
  parseTuningTargets,
} from "./protocol.js";
import {
  createTuningProfile,
  prepareProfileImport,
  renderDevicetreeSnippet,
} from "./profile.js";
import { ResponseRequestQueue } from "./request-queue.js";
import { PointingPlayground } from "./playground.js";

const USB_FILTERS = [{ usbVendorId: 0x16c0 }];
const MAX_STATE_EVENTS = 2_000;
const INTENT_LABELS = ["undecided", "free", "horizontal", "vertical"];
const STAGE_EVENT_LABELS = ["suppressed", "discarded", "qualified", "intent-changed", "flushed", "action"];
const STAGE_EVENT_INTENT_CHANGED = 3;

const elements = Object.fromEntries(
  ["status", "connect", "simulate", "clear", "notice", "tuning", "reset-all", "profile-export", "profile-copy", "profile-import", "profile-file", "modified-count", "diagnostics", "state-count", "state-dropped"].map(
    (id) => [id, document.getElementById(id)],
  ),
);

let port;
let reader;
let writer;
let readActive = false;
let protocolReady = false;
let simulator;
let heartbeat;
let tuningTargets = new Map();
let stateEvents = [];
let stateStatus = { schemaVersion: 0, dropped: 0, queueCapacity: 0, levels: new Map(), labels: new Map() };
let stateDirty = true;
let decoder = new FrameDecoder();
const tuningRequests = new ResponseRequestQueue(
  (frame) => {
    if (!writer) throw new Error("Serial writer is unavailable");
    return writer.write(frame);
  },
  (error) => notice(`Tuning request failed: ${error.message}`, true),
);

function setStatus(text, live = false) {
  elements.status.textContent = text;
  elements.status.classList.toggle("live", live);
}

function notice(text, error = false) {
  elements.notice.textContent = text;
  elements.notice.classList.toggle("error", error);
}

function updateHeartbeat() {
  const stateEnabled = [...stateStatus.levels.values()].some((level) => level !== STATE.OFF);
  const needed = Boolean(writer) && stateEnabled;
  if (needed && !heartbeat) heartbeat = setInterval(() => send(MESSAGE.PING), 2000);
  if (!needed && heartbeat) {
    clearInterval(heartbeat);
    heartbeat = undefined;
  }
}

function escapeHtml(value) {
  return String(value)
    .replaceAll("&", "&amp;")
    .replaceAll("<", "&lt;")
    .replaceAll(">", "&gt;")
    .replaceAll('"', "&quot;");
}

function setDescription(description) {
  protocolReady = true;
  stateDirty = true;
  notice(`Protocol v${description.version} connected.`);
}



function renderTuning() {
  const targets = [...tuningTargets.values()];
  const modified = targets.reduce(
    (count, target) => count + target.parameters.filter((parameter) => parameter.current !== parameter.compiled).length,
    0,
  );
  const profileReady = targets.length > 0 && targets.every(
    (target) => target.stableId && target.devicetreePath && target.parameters.length > 0 &&
      target.parameters.every((parameter) => parameter.key && parameter.devicetreeProperty),
  );
  elements["reset-all"].disabled = tuningTargets.size === 0;
  elements["profile-export"].disabled = !profileReady;
  elements["profile-copy"].disabled = !profileReady;
  elements["profile-import"].disabled = !profileReady;
  elements["modified-count"].textContent = `${modified} modified`;
  if (!tuningTargets.size) {
    elements.tuning.innerHTML = '<p class="muted">Connect current firmware to discover tunable processors.</p>';
    return;
  }

  elements.tuning.innerHTML = [...tuningTargets.values()]
    .map((target) => `<article class="tuning-target">
      <div class="tuning-target-heading">
        <div><h3>${escapeHtml(target.label)}</h3><span>${target.stableId ? `<code>${escapeHtml(target.stableId)}</code> · ` : ""}Temporary preview values</span></div>
        <button data-reset-target="${target.id}" ${target.parameters.length ? "" : "disabled"}>Reset target</button>
      </div>
      ${target.parameters.length ? `<div class="parameters">${target.parameters.map((parameter) => {
        const changed = parameter.current !== parameter.compiled;
        const control = parameter.type === TUNING.BOOLEAN
          ? `<input type="checkbox" data-value ${parameter.current ? "checked" : ""}>`
          : `<input type="number" data-value value="${parameter.current}" min="${parameter.minimum}" max="${parameter.maximum}" step="${parameter.step}">`;
        const help = parameter.description
          ? `<span class="parameter-help" tabindex="0" role="img" aria-label="${escapeHtml(parameter.description)}" data-tooltip="${escapeHtml(parameter.description)}">?</span>`
          : "";
        return `<div class="parameter ${changed ? "changed" : ""}" data-target="${target.id}" data-parameter="${parameter.id}">
          <label><span>${escapeHtml(parameter.label)}${parameter.unit ? ` <small>(${escapeHtml(parameter.unit)})</small>` : ""}</span>${help}</label>
          <div class="parameter-control">${control}<button data-preview>Preview</button></div>
          <small>Compiled: ${parameter.compiled}${parameter.unit ? ` ${escapeHtml(parameter.unit)}` : ""}${changed ? " · modified" : ""}</small>
        </div>`;
      }).join("")}</div>` : '<p class="muted">Loading parameters…</p>'}
    </article>`)
    .join("");
}

function stateFlagLabels(flags) {
  return [
    [STATE.FLAG_INTENT_CHANGED, "intent changed"],
    [STATE.FLAG_SUPPRESSED, "suppressed"],
    [STATE.FLAG_DISCARDED, "discarded"],
    [STATE.FLAG_OUTPUT, "output"],
    [STATE.FLAG_QUALIFIED, "qualified"],
  ].filter(([flag]) => flags & flag).map(([, label]) => label);
}

function describeStateEvent(event) {
  if (event.targetKind === 4) {
    const label = STAGE_EVENT_LABELS[event.values[0]] ?? `stage-event ${event.values[0]}`;
    const quantity = event.values[0] === STAGE_EVENT_INTENT_CHANGED
      ? (INTENT_LABELS[event.values[1]] ?? `intent ${event.values[1]}`)
      : (event.values[1] !== 0 ? String(event.values[1]) : "");
    const flags = stateFlagLabels(event.flags);
    return `${label}${quantity ? ` · ${quantity}` : ""}${flags.length ? ` · ${flags.join(", ")}` : ""}`;
  }
  return `values ${event.values.join("/")}`;
}

function renderDiagnostics() {
  if (!stateDirty) return;
  stateDirty = false;
  const supported = protocolReady || simulator;
  elements["state-count"].textContent = `${stateEvents.length.toLocaleString()} state events`;
  elements["state-dropped"].textContent = supported
    ? `${stateStatus.dropped.toLocaleString()} dropped · queue capacity ${stateStatus.queueCapacity || "?"}`
    : "Requires protocol v6";
  if (!supported) {
    elements.diagnostics.innerHTML = '<p class="muted">Connect current firmware to inspect processor decisions.</p>';
    return;
  }

  const targets = [...stateStatus.levels.entries()];
  if (targets.length === 0) {
    elements.diagnostics.innerHTML = '<p class="muted">No stage targets registered.</p>';
    return;
  }

  const controls = targets.map(([targetId, level]) => {
    const label = stateStatus.labels.get(targetId) || `Stage ${targetId}`;
    const latest = [...stateEvents].reverse().find((event) => event.targetId === targetId);
    return `<article class="diagnostic-target">
      <div><h3>${escapeHtml(label)}</h3><span>${latest ? escapeHtml(describeStateEvent(latest)) : "No state received"}</span></div>
      <label>Detail
        <select data-state-target="${targetId}">
          <option value="0" ${level === STATE.OFF ? "selected" : ""}>Off</option>
          <option value="1" ${level === STATE.DECISIONS ? "selected" : ""}>Decisions</option>
          <option value="2" ${level === STATE.VERBOSE ? "selected" : ""}>Every frame</option>
        </select>
      </label>
    </article>`;
  }).join("");
  const timeline = stateEvents.slice(-30).reverse().map((event) => {
    const label = stateStatus.labels.get(event.targetId) || `Target ${event.targetId}`;
    return `<li><code>${event.timestamp} · #${event.sequence}</code><strong>${escapeHtml(label)}</strong><span>${escapeHtml(describeStateEvent(event))}</span></li>`;
  }).join("");
  elements.diagnostics.innerHTML = `${controls}<ol class="state-timeline">${timeline || '<li class="muted">Enable a target and move a trackball.</li>'}</ol>`;
}

function setStateStatus(status) {
  stateStatus = status;
  stateDirty = true;
  updateHeartbeat();
}

function addStateEvent(event) {
  stateEvents.push(event);
  if (stateEvents.length > MAX_STATE_EVENTS) stateEvents.splice(0, stateEvents.length - MAX_STATE_EVENTS);
  stateDirty = true;
}

function setTuningTargets(targets, requestDescriptions = true) {
  tuningTargets = new Map(targets.map((target) => [target.id, target]));
  renderTuning();
  stateDirty = true;
  if (requestDescriptions) {
    for (const target of targets) {
      queueTuningRequest(MESSAGE.TUNING_DESCRIBE_REQUEST, Uint8Array.of(target.id));
      queueTuningRequest(MESSAGE.TUNING_TARGET_METADATA_REQUEST, Uint8Array.of(target.id));
    }
  }
}

function setTuningDescription(description) {
  const target = tuningTargets.get(description.targetId);
  if (!target) return;
  target.parameters = description.parameters;
  renderTuning();
  stateDirty = true;
  for (const parameter of target.parameters) {
    queueTuningRequest(MESSAGE.TUNING_HELP_REQUEST, Uint8Array.of(target.id, parameter.id));
  }
  for (const parameter of target.parameters) {
    queueTuningRequest(MESSAGE.TUNING_PARAMETER_METADATA_REQUEST,
      Uint8Array.of(target.id, parameter.id));
  }
}

function setTuningHelp(help) {
  const target = tuningTargets.get(help.targetId);
  const parameter = target?.parameters.find(({ id }) => id === help.parameterId);
  if (!parameter) return;
  parameter.description = help.description;
  renderTuning();
}

function setTuningTargetMetadata(metadata) {
  const target = tuningTargets.get(metadata.targetId);
  if (!target) return;
  Object.assign(target, metadata);
  renderTuning();
}

function setTuningParameterMetadata(metadata) {
  const target = tuningTargets.get(metadata.targetId);
  const parameter = target?.parameters.find(({ id }) => id === metadata.parameterId);
  if (!parameter) return;
  Object.assign(parameter, metadata);
  renderTuning();
}


function renderLoop() {
  renderDiagnostics();
  requestAnimationFrame(renderLoop);
}

async function send(type, payload) {
  if (writer) await writer.write(encodeFrame(type, payload));
}

function queueTuningRequest(type, payload) {
  tuningRequests.enqueue(type, encodeFrame(type, payload));
}

function downloadJson(payload, filename) {
  const link = document.createElement("a");
  link.href = URL.createObjectURL(new Blob([JSON.stringify(payload, null, 2)], { type: "application/json" }));
  link.download = filename;
  link.click();
  URL.revokeObjectURL(link.href);
}

function applyProfile(profile) {
  const batches = prepareProfileImport(profile, tuningTargets);
  if (simulator) {
    for (const batch of batches) {
      const target = tuningTargets.get(batch.targetId);
      for (const update of batch.values) {
        target.parameters.find(({ id }) => id === update.parameterId).current = update.value;
      }
    }
    renderTuning();
    notice(`Applied ${batches.length} simulated profile targets.`);
    return;
  }

  for (const batch of batches) {
    tuningRequests.enqueue(MESSAGE.TUNING_SET_MANY_REQUEST,
      encodeTuningSetMany(batch.targetId, batch.values));
  }
  notice(`Validated profile; applying ${batches.length} targets as temporary previews.`);
}

function handleFrame(frame) {
  if (frame.type === MESSAGE.DESCRIBE_RESPONSE) {
    const description = parseDescribe(frame.payload);
    setDescription(description);
    queueTuningRequest(MESSAGE.TUNING_TARGETS_REQUEST);
  }
  else if (frame.type === MESSAGE.ACK) {
    const ack = parseAck(frame.payload);
    updateHeartbeat();
    stateStatus.dropped = ack.stateDropped;
    stateDirty = true;
    notice(`${ack.stateDropped} state samples dropped.`);
  } else if (frame.type === MESSAGE.STATE_SAMPLE) addStateEvent(parseStateSample(frame.payload));
  else if (frame.type === MESSAGE.TUNING_TARGETS_RESPONSE) {
    tuningRequests.complete(MESSAGE.TUNING_TARGETS_REQUEST);
    setTuningTargets(parseTuningTargets(frame.payload));
    queueTuningRequest(MESSAGE.STATE_CONTROL_REQUEST);
  } else if (frame.type === MESSAGE.TUNING_DESCRIBE_RESPONSE) {
    tuningRequests.complete(MESSAGE.TUNING_DESCRIBE_REQUEST);
    setTuningDescription(parseTuningDescription(frame.payload));
  } else if (frame.type === MESSAGE.TUNING_HELP_RESPONSE) {
    tuningRequests.complete(MESSAGE.TUNING_HELP_REQUEST);
    setTuningHelp(parseTuningHelp(frame.payload));
  } else if (frame.type === MESSAGE.TUNING_TARGET_METADATA_RESPONSE) {
    tuningRequests.complete(MESSAGE.TUNING_TARGET_METADATA_REQUEST);
    setTuningTargetMetadata(parseTuningTargetMetadata(frame.payload));
  } else if (frame.type === MESSAGE.TUNING_PARAMETER_METADATA_RESPONSE) {
    tuningRequests.complete(MESSAGE.TUNING_PARAMETER_METADATA_REQUEST);
    setTuningParameterMetadata(parseTuningParameterMetadata(frame.payload));
  } else if (frame.type === MESSAGE.STATE_STATUS_RESPONSE) {
    tuningRequests.complete(MESSAGE.STATE_CONTROL_REQUEST);
    setStateStatus(parseStateStatus(frame.payload));
  } else if (frame.type === MESSAGE.TUNING_RESULT) {
    const result = parseTuningResult(frame.payload);
    tuningRequests.complete(result.requestType);
    const target = tuningTargets.get(result.targetId);
    if (result.status !== TUNING.STATUS_OK) {
      const messages = ["success", "unknown target", "unknown parameter", "invalid value", "internal error"];
      notice(`Preview rejected: ${messages[result.status] ?? `status ${result.status}`}.`, true);
      if (target && [MESSAGE.TUNING_SET_REQUEST, MESSAGE.TUNING_SET_MANY_REQUEST,
        MESSAGE.TUNING_RESET_REQUEST].includes(result.requestType)) {
        queueTuningRequest(MESSAGE.TUNING_DESCRIBE_REQUEST, Uint8Array.of(target.id));
      }
    } else if (result.requestType === MESSAGE.TUNING_SET_REQUEST && target) {
      const parameter = target.parameters.find(({ id }) => id === result.parameterId);
      if (parameter) parameter.current = result.value;
      renderTuning();
      notice(`${target.label} preview updated. Changes remain temporary until reboot.`);
    } else if (result.requestType === MESSAGE.TUNING_SET_MANY_REQUEST && target) {
      queueTuningRequest(MESSAGE.TUNING_DESCRIBE_REQUEST, Uint8Array.of(target.id));
      notice(`${target.label} profile values applied atomically as a temporary preview.`);
    } else if (result.requestType === MESSAGE.TUNING_RESET_REQUEST) {
      const resetTargets = result.targetId === TUNING.ALL_TARGETS ? [...tuningTargets.values()] : [target].filter(Boolean);
      for (const item of resetTargets) queueTuningRequest(MESSAGE.TUNING_DESCRIBE_REQUEST, Uint8Array.of(item.id));
      notice(result.targetId === TUNING.ALL_TARGETS ? "All compiled defaults restored." : `${target?.label ?? "Target"} defaults restored.`);
    }
  }
}

async function readLoop() {
  while (readActive && port?.readable) {
    reader = port.readable.getReader();
    try {
      for (;;) {
        const { value, done } = await reader.read();
        if (done) break;
        for (const frame of decoder.push(value)) handleFrame(frame);
      }
    } catch (error) {
      if (readActive) notice(`Serial read failed: ${error.message}`, true);
    } finally {
      reader.releaseLock();
      reader = undefined;
    }
  }
}

async function disconnect() {
  try { if (writer && protocolReady) await writer.write(encodeStateControl(STATE.ALL_TARGETS, STATE.OFF)); } catch {}
  readActive = false;
  try { if (reader) await reader.cancel(); } catch {}
  try { if (writer) writer.releaseLock(); } catch {}
  try { if (port) await port.close(); } catch {}
  port = writer = undefined;
  protocolReady = false;
  tuningRequests.clear();
  if (heartbeat) clearInterval(heartbeat);
  heartbeat = undefined;
  elements.connect.textContent = "Connect keyboard";
  elements.simulate.disabled = false;
  tuningTargets = new Map();
  stateEvents = [];
  stateStatus = { schemaVersion: 0, dropped: 0, queueCapacity: 0, levels: new Map(), labels: new Map() };
  stateDirty = true;
  renderTuning();
  setStatus("Disconnected");
}

async function connect() {
  if (!("serial" in navigator)) {
    notice("Web Serial is unavailable. Use current desktop Chrome or the simulator.", true);
    return;
  }
  try {
    if (simulator) startSimulator();
    port = await navigator.serial.requestPort({ filters: USB_FILTERS });
    await port.open({ baudRate: 115200 });
    writer = port.writable.getWriter();
    protocolReady = false;
    readActive = true;
    readLoop();
    elements.connect.textContent = "Disconnect";
    elements.simulate.disabled = true;
    tuningTargets = new Map();
    stateEvents = [];
    stateStatus = { schemaVersion: 0, dropped: 0, queueCapacity: 0, levels: new Map(), labels: new Map() };
    stateDirty = true;
    tuningRequests.clear();
    decoder = new FrameDecoder();
    renderTuning();
    setStatus("Connected", true);
    await send(MESSAGE.DESCRIBE_REQUEST);
  } catch (error) {
    notice(`Connection failed: ${error.message}`, true);
    await disconnect();
  }
}

function startSimulator() {
  if (port) {
    notice("Disconnect the keyboard before starting the simulator.", true);
    return;
  }
  if (simulator) {
    clearInterval(simulator);
    simulator = undefined;
    elements.simulate.textContent = "Simulate";
    setStatus(port ? "Connected" : "Disconnected", Boolean(port));
    tuningTargets = new Map();
    protocolReady = false;
    stateEvents = [];
    stateStatus = { schemaVersion: 0, dropped: 0, queueCapacity: 0, levels: new Map(), labels: new Map() };
    stateDirty = true;
    renderTuning();
    return;
  }
  setDescription({ version: PROTOCOL_VERSION });
  setTuningTargets([{ id: 0, stableId: "simulated-scroll", kind: 1, label: "Simulated adaptive scroll", devicetreePath: "/simulated_scroll", parameters: [
    { id: 6, key: "activation-distance", devicetreeProperty: "activation-distance", type: TUNING.INTEGER, minimum: 1, maximum: 10000, step: 1, compiled: 16, current: 16, label: "Activation distance", unit: "counts", description: "Accumulated motion required before adaptive axis classification." },
    { id: 9, key: "suppress-after-keypress-ms", devicetreeProperty: "suppress-after-keypress-ms", type: TUNING.INTEGER, minimum: 0, maximum: 500, step: 1, compiled: 40, current: 40, label: "Physical keypress guard", unit: "ms", description: "Ignores movement briefly after a physical key press to reject typing vibration." },
    { id: 10, key: "discard-unclassified", devicetreeProperty: "discard-unclassified", type: TUNING.BOOLEAN, minimum: 0, maximum: 1, step: 1, compiled: 0, current: 0, label: "Discard unclassified motion", unit: "", description: "Drops motion that ends before adaptive classification." },
  ] }, { id: 1, stableId: "simulated-text-navigation", kind: 2, label: "Simulated text navigation", devicetreePath: "/simulated_text_navigation", parameters: [
    { id: 1, key: "horizontal-threshold", devicetreeProperty: "horizontal-threshold", type: TUNING.INTEGER, minimum: 1, maximum: 10000, step: 1, compiled: 25, current: 25, label: "Horizontal step distance", unit: "counts", description: "Horizontal movement required for one left or right key tap." },
    { id: 2, key: "vertical-threshold", devicetreeProperty: "vertical-threshold", type: TUNING.INTEGER, minimum: 1, maximum: 10000, step: 1, compiled: 50, current: 50, label: "Vertical step distance", unit: "counts", description: "Vertical movement required for one up or down key tap." },
    { id: 3, key: "activation-distance", devicetreeProperty: "activation-distance", type: TUNING.INTEGER, minimum: 1, maximum: 10000, step: 1, compiled: 12, current: 12, label: "Activation distance", unit: "counts", description: "Accumulated movement required before choosing the gesture's locked axis." },
    { id: 4, key: "engage-ratio-percent", devicetreeProperty: "engage-ratio-percent", type: TUNING.INTEGER, minimum: 101, maximum: 1000, step: 1, compiled: 150, current: 150, label: "Axis engage ratio", unit: "%", description: "Dominant-to-minor movement ratio required to choose an axis." },
    { id: 5, key: "idle-timeout-ms", devicetreeProperty: "idle-timeout-ms", type: TUNING.INTEGER, minimum: 10, maximum: 2000, step: 1, compiled: 120, current: 120, label: "Gesture idle timeout", unit: "ms", description: "The motion-free gap that ends the current gesture." },
  ] }], false);
  setStateStatus({
    schemaVersion: 2,
    dropped: 0,
    queueCapacity: 64,
    levels: new Map([[0, STATE.OFF], [1, STATE.OFF]]),
    labels: new Map([[0, "Simulated adaptive scroll"], [1, "Simulated text navigation"]]),
  });
  let tick = 0;
  simulator = setInterval(() => {
    const raw = { x: Math.round(4 * Math.cos(tick / 14) + Math.random() * 2 - 1), y: Math.round(4 * Math.sin(tick / 14) + Math.random() * 2 - 1) };
    const level = stateStatus.levels.get(0) ?? STATE.OFF;
    if (level === STATE.VERBOSE || (level === STATE.DECISIONS && tick % 20 === 0)) {
      const intent = Math.abs(raw.x) >= Math.abs(raw.y) ? 2 : 3;
      addStateEvent({ targetId: 0, targetKind: 4, event: STATE.EVENT_FRAME,
        intent: tick % 20 === 0 ? intent : 0,
        flags: tick % 20 === 0 ? STATE.FLAG_INTENT_CHANGED : 0,
        timestamp: tick * 8, sequence: tick + 2,
        values: [STAGE_EVENT_INTENT_CHANGED, intent, 0, 0, 0, 0, 0, 0, 0, 0] });
    }
    tick += 1;
  }, 8);
  elements.simulate.textContent = "Stop simulation";
  setStatus("Simulating", true);
}

elements.connect.addEventListener("click", () => (port ? disconnect() : connect()));
elements.simulate.addEventListener("click", startSimulator);
elements.clear.addEventListener("click", () => {
  stateEvents = [];
  stateDirty = true;
});

elements.diagnostics.addEventListener("change", (event) => {
  const select = event.target.closest("[data-state-target]");
  if (!select) return;
  const targetId = Number(select.dataset.stateTarget);
  const level = Number(select.value);
  if (simulator) {
    stateStatus.levels.set(targetId, level);
    stateDirty = true;
  } else if (writer && protocolReady) {
    tuningRequests.enqueue(MESSAGE.STATE_CONTROL_REQUEST, encodeStateControl(targetId, level));
  }
});

elements["profile-export"].addEventListener("click", () => {
  try {
    const profile = createTuningProfile(tuningTargets);
    downloadJson(profile, `zmk-pointing-profile-${new Date().toISOString().replaceAll(":", "-")}.json`);
    notice("Exported the current runtime tuning profile.");
  } catch (error) {
    notice(`Profile export failed: ${error.message}`, true);
  }
});

elements["profile-copy"].addEventListener("click", async () => {
  try {
    await navigator.clipboard.writeText(renderDevicetreeSnippet(tuningTargets));
    notice("Copied a devicetree overlay with the current values. Review it before committing.");
  } catch (error) {
    notice(`Could not copy configuration: ${error.message}`, true);
  }
});

elements["profile-import"].addEventListener("click", () => elements["profile-file"].click());
elements["profile-file"].addEventListener("change", async () => {
  const [file] = elements["profile-file"].files;
  if (!file) return;
  try {
    applyProfile(JSON.parse(await file.text()));
  } catch (error) {
    notice(`Profile import rejected before applying changes: ${error.message}`, true);
  } finally {
    elements["profile-file"].value = "";
  }
});

elements.tuning.addEventListener("click", async (event) => {
  const preview = event.target.closest("[data-preview]");
  const reset = event.target.closest("[data-reset-target]");
  if (preview) {
    const row = preview.closest("[data-target][data-parameter]");
    const targetId = Number(row.dataset.target);
    const parameterId = Number(row.dataset.parameter);
    const target = tuningTargets.get(targetId);
    const parameter = target?.parameters.find(({ id }) => id === parameterId);
    const input = row.querySelector("[data-value]");
    const value = parameter?.type === TUNING.BOOLEAN ? Number(input.checked) : Number(input.value);
    if (!parameter || !Number.isInteger(value) || value < parameter.minimum || value > parameter.maximum) {
      notice("Enter a whole value within the advertised range.", true);
      return;
    }
    if (simulator) {
      parameter.current = value;
      renderTuning();
      notice(`${target.label} simulator preview updated.`);
    } else if (writer) {
      tuningRequests.enqueue(MESSAGE.TUNING_SET_REQUEST, encodeTuningSet(targetId, parameterId, value));
    }
  } else if (reset) {
    const targetId = Number(reset.dataset.resetTarget);
    if (simulator) {
      const target = tuningTargets.get(targetId);
      for (const parameter of target.parameters) parameter.current = parameter.compiled;
      renderTuning();
      notice(`${target.label} simulator defaults restored.`);
    } else {
      queueTuningRequest(MESSAGE.TUNING_RESET_REQUEST, Uint8Array.of(targetId));
    }
  }
});

elements["reset-all"].addEventListener("click", async () => {
  if (simulator) {
    for (const target of tuningTargets.values()) {
      for (const parameter of target.parameters) parameter.current = parameter.compiled;
    }
    renderTuning();
    notice("All simulator defaults restored.");
  } else {
    queueTuningRequest(MESSAGE.TUNING_RESET_REQUEST, Uint8Array.of(TUNING.ALL_TARGETS));
  }
});

function playgroundContext() {
  let tuningProfile;
  try {
    if (tuningTargets.size) tuningProfile = createTuningProfile(tuningTargets);
  } catch {
    tuningProfile = undefined;
  }
  const latestSequence = stateEvents.slice(-1).reduce((latest, record) => Math.max(latest, record.sequence ?? 0), -1);
  return {
    nextSequence: latestSequence + 1,
    tuningProfile,
    stateEvents,
    stateSchemaVersion: stateStatus.schemaVersion,
    stateDropped: stateStatus.dropped,
    queueCapacity: stateStatus.queueCapacity,
  };
}

new PointingPlayground(document.getElementById("playground"), {
  getContext: playgroundContext,
  onExport: (result) => {
    downloadJson(
      result,
      `zmk-pointing-playground-${result.activity.id}-${new Date().toISOString().replaceAll(":", "-")}.json`,
    );
    notice(`Exported ${result.activity.label} playground results.`);
  },
});

navigator.serial?.addEventListener("disconnect", (event) => { if (event.target === port) disconnect(); });
renderLoop();
renderTuning();
