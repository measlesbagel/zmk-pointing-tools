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
  parseSample,
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
const COLORS = ["#78d6b0", "#e8c477", "#82aaff", "#ef8fa3", "#c099ff", "#79c7d9"];
const MAX_SAMPLES = 20_000;
const MAX_STATE_EVENTS = 2_000;
const INTENT_LABELS = ["undecided", "free", "horizontal", "vertical"];
const DIRECTION_LABELS = ["left", "right", "up", "down"];

const elements = Object.fromEntries(
  ["status", "connect", "telemetry", "simulate", "clear", "export", "notice", "trace", "streams", "sample-count", "tuning", "reset-all", "profile-export", "profile-copy", "profile-import", "profile-file", "modified-count", "diagnostics", "state-count", "state-dropped"].map(
    (id) => [id, document.getElementById(id)],
  ),
);

let port;
let reader;
let writer;
let readActive = false;
let telemetryEnabled = false;
let protocolReady = false;
let traceDropped = 0;
let simulator;
let heartbeat;
let streams = new Map();
let tuningTargets = new Map();
let samples = [];
let stateEvents = [];
let stateStatus = { schemaVersion: 0, dropped: 0, queueCapacity: 0, levels: new Map() };
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
  const needed = Boolean(writer) && (telemetryEnabled || stateEnabled);
  if (needed && !heartbeat) heartbeat = setInterval(() => send(MESSAGE.PING), 2000);
  if (!needed && heartbeat) {
    clearInterval(heartbeat);
    heartbeat = undefined;
  }
}

function streamColor(key) {
  const keys = [...streams.keys()];
  return COLORS[Math.max(0, keys.indexOf(key)) % COLORS.length];
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
  streams = new Map(
    description.streams.map((stream) => [stream.key, { ...stream, count: 0, lastTimestamp: undefined, totalDt: 0, distance: 0, latest: {} }]),
  );
  renderStreams();
  stateDirty = true;
  notice(`Protocol v${description.version}; ${streams.size} trace streams available.`);
}

function addSample(sample) {
  if (!streams.has(sample.key)) {
    streams.set(sample.key, { key: sample.key, deviceId: sample.deviceId, stage: sample.stage, label: `Stream ${sample.key}`, count: 0, totalDt: 0, distance: 0, latest: {} });
  }
  const stream = streams.get(sample.key);
  if (stream.lastTimestamp !== undefined) stream.totalDt += (sample.timestamp - stream.lastTimestamp) >>> 0;
  stream.lastTimestamp = sample.timestamp;
  stream.count += 1;
  stream.distance += Math.hypot(sample.x, sample.y);
  stream.latest = sample;
  samples.push(sample);
  if (samples.length > MAX_SAMPLES) samples.splice(0, samples.length - MAX_SAMPLES);
  if ([...stateStatus.levels.values()].some((level) => level !== STATE.OFF)) stateDirty = true;
  elements.export.disabled = samples.length === 0;
}

function renderStreams() {
  if (!streams.size) {
    elements.streams.innerHTML = '<p class="muted">No stream description received.</p>';
    return;
  }
  elements.streams.innerHTML = [...streams.values()]
    .map((stream) => {
      const average = stream.count > 1 ? stream.totalDt / (stream.count - 1) : 0;
      const latest = stream.latest;
      return `<article class="stream" style="--stream-color:${streamColor(stream.key)}">
        <h3>${escapeHtml(stream.label)}</h3>
        <div class="metrics">
          <span>Frames</span><strong>${stream.count}</strong>
          <span>Mean interval</span><strong>${average.toFixed(1)} ms</strong>
          <span>Distance</span><strong>${stream.distance.toFixed(0)}</strong>
          <span>X / Y</span><strong>${latest.x ?? 0} / ${latest.y ?? 0}</strong>
          <span>Wheel H / V</span><strong>${latest.hWheel ?? 0} / ${latest.wheel ?? 0}</strong>
        </div>
      </article>`;
    })
    .join("");
  elements["sample-count"].textContent = `${samples.length.toLocaleString()} samples`;
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
    [STATE.FLAG_IDLE_RESET, "idle reset"],
    [STATE.FLAG_INTENT_CHANGED, "intent changed"],
    [STATE.FLAG_SUPPRESSED, "suppressed"],
    [STATE.FLAG_SUPPRESSION_CHANGED, "guard transition"],
    [STATE.FLAG_DISCARDED, "discarded"],
    [STATE.FLAG_OUTPUT, "output"],
    [STATE.FLAG_CLIPPED_HORIZONTAL, "H clipped"],
    [STATE.FLAG_CLIPPED_VERTICAL, "V clipped"],
    [STATE.FLAG_QUALIFIED, "qualified"],
    [STATE.FLAG_PENDING_DISCARDED, "pending discarded"],
  ].filter(([flag]) => flags & flag).map(([, label]) => label);
}

function describeStateEvent(event) {
  const intent = INTENT_LABELS[event.intent] ?? `intent ${event.intent}`;
  const flags = stateFlagLabels(event.flags);
  if (event.targetKind === 1 && event.event === STATE.EVENT_FRAME) {
    return `input ${event.values[0]}/${event.values[1]} · ${intent} · energy ${event.values[2]}/${event.values[3]} · pending ${event.values[6]}/${event.values[7]}${flags.length ? ` · ${flags.join(", ")}` : ""}`;
  }
  if (event.targetKind === 1 && event.event === STATE.EVENT_FLUSH) {
    return `wheel H/V ${event.values[0]}/${event.values[1]} · ${intent} · remainder ${event.values[8]}/${event.values[9]}${flags.length ? ` · ${flags.join(", ")}` : ""}`;
  }
  if (event.targetKind === 2) {
    const direction = event.values[4] >= 0 ? DIRECTION_LABELS[event.values[4]] : "none";
    return `input ${event.values[0]}/${event.values[1]} · ${intent} · accumulated ${event.values[2]}/${event.values[3]} · step ${direction}${flags.length ? ` · ${flags.join(", ")}` : ""}`;
  }
  if (event.targetKind === 3) {
    const phase = ["idle", "pending", "active", "bypass"][event.intent] ?? `phase ${event.intent}`;
    return `input ${event.values[0]}/${event.values[1]} · output ${event.values[2]}/${event.values[3]} · ${phase} · pending ${event.values[4]}/${event.values[5]} (${event.values[6]} frames)${flags.length ? ` · ${flags.join(", ")}` : ""}`;
  }
  return `${intent}${flags.length ? ` · ${flags.join(", ")}` : ""} · values ${event.values.join("/")}`;
}

function renderDiagnostics() {
  if (!stateDirty) return;
  stateDirty = false;
  const supported = protocolReady || simulator;
  elements["state-count"].textContent = `${stateEvents.length.toLocaleString()} state events`;
  elements["state-dropped"].textContent = supported
    ? `${stateStatus.dropped.toLocaleString()} dropped · queue capacity ${stateStatus.queueCapacity || "?"}`
    : "Requires protocol v5";
  if (!supported || tuningTargets.size === 0) {
    elements.diagnostics.innerHTML = '<p class="muted">Connect current firmware to inspect processor decisions.</p>';
    return;
  }

  const controls = [...tuningTargets.values()].map((target) => {
    const level = stateStatus.levels.get(target.id) ?? STATE.OFF;
    const latest = [...stateEvents].reverse().find((event) => event.targetId === target.id);
    return `<article class="diagnostic-target">
      <div><h3>${escapeHtml(target.label)}</h3><span>${latest ? escapeHtml(describeStateEvent(latest)) : "No state received"}</span></div>
      <label>Detail
        <select data-state-target="${target.id}">
          <option value="0" ${level === STATE.OFF ? "selected" : ""}>Off</option>
          <option value="1" ${level === STATE.DECISIONS ? "selected" : ""}>Decisions</option>
          <option value="2" ${level === STATE.VERBOSE ? "selected" : ""}>Every frame</option>
        </select>
      </label>
    </article>`;
  }).join("");
  const timeline = [
    ...samples.slice(-40).map((sample) => ({ ...sample, recordType: "trace" })),
    ...stateEvents.slice(-40).map((event) => ({ ...event, recordType: "state" })),
  ].sort((left, right) => left.sequence - right.sequence).slice(-30).reverse().map((event) => {
    if (event.recordType === "trace") {
      const stream = streams.get(event.key);
      const detail = `relative ${event.x}/${event.y} · wheel H/V ${event.hWheel}/${event.wheel}`;
      return `<li><code>${event.timestamp} · #${event.sequence}</code><strong>${escapeHtml(stream?.label ?? `Stream ${event.key}`)}</strong><span>${escapeHtml(detail)}</span></li>`;
    }
    const target = tuningTargets.get(event.targetId);
    return `<li><code>${event.timestamp} · #${event.sequence}</code><strong>${escapeHtml(target?.label ?? `Target ${event.targetId}`)}</strong><span>${escapeHtml(describeStateEvent(event))}</span></li>`;
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
  elements.export.disabled = false;
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

function draw() {
  const canvas = elements.trace;
  const context = canvas.getContext("2d");
  const ratio = devicePixelRatio || 1;
  const width = canvas.clientWidth;
  const height = canvas.clientHeight;
  if (canvas.width !== Math.floor(width * ratio) || canvas.height !== Math.floor(height * ratio)) {
    canvas.width = Math.floor(width * ratio);
    canvas.height = Math.floor(height * ratio);
  }
  context.setTransform(ratio, 0, 0, ratio, 0, 0);
  context.clearRect(0, 0, width, height);
  context.strokeStyle = "#252b34";
  context.beginPath();
  context.moveTo(width / 2, 0); context.lineTo(width / 2, height);
  context.moveTo(0, height / 2); context.lineTo(width, height / 2);
  context.stroke();

  for (const stream of streams.values()) {
    const selected = samples.slice(-3000).filter((sample) => sample.key === stream.key && (sample.x || sample.y));
    let x = width / 2;
    let y = height / 2;
    context.beginPath();
    context.moveTo(x, y);
    for (const sample of selected) {
      x += sample.x * 0.35;
      y += sample.y * 0.35;
      context.lineTo(x, y);
    }
    context.strokeStyle = streamColor(stream.key);
    context.lineWidth = 1.5;
    context.stroke();
  }
}

function renderLoop() {
  renderStreams();
  renderDiagnostics();
  draw();
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
    traceDropped = ack.dropped;
    telemetryEnabled = ack.enabled;
    elements.telemetry.textContent = ack.enabled ? "Stop telemetry" : "Start telemetry";
    updateHeartbeat();
    stateStatus.dropped = ack.stateDropped;
    stateDirty = true;
    notice(`Telemetry ${ack.enabled ? "active" : "stopped"}; ${ack.dropped} trace and ${ack.stateDropped} state samples dropped.`);
  } else if (frame.type === MESSAGE.SAMPLE) addSample(parseSample(frame.payload));
  else if (frame.type === MESSAGE.STATE_SAMPLE) addStateEvent(parseStateSample(frame.payload));
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
  try { if (writer && telemetryEnabled) await send(MESSAGE.TELEMETRY_CONTROL, Uint8Array.of(0)); } catch {}
  readActive = false;
  try { if (reader) await reader.cancel(); } catch {}
  try { if (writer) writer.releaseLock(); } catch {}
  try { if (port) await port.close(); } catch {}
  port = writer = undefined;
  telemetryEnabled = false;
  protocolReady = false;
  traceDropped = 0;
  tuningRequests.clear();
  if (heartbeat) clearInterval(heartbeat);
  heartbeat = undefined;
  elements.connect.textContent = "Connect keyboard";
  elements.telemetry.disabled = true;
  elements.simulate.disabled = false;
  elements.telemetry.textContent = "Start telemetry";
  tuningTargets = new Map();
  stateEvents = [];
  stateStatus = { schemaVersion: 0, dropped: 0, queueCapacity: 0, levels: new Map() };
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
    elements.telemetry.disabled = false;
    elements.simulate.disabled = true;
    streams = new Map();
    tuningTargets = new Map();
    samples = [];
    traceDropped = 0;
    stateEvents = [];
    stateStatus = { schemaVersion: 0, dropped: 0, queueCapacity: 0, levels: new Map() };
    stateDirty = true;
    tuningRequests.clear();
    decoder = new FrameDecoder();
    renderStreams();
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
    stateStatus = { schemaVersion: 0, dropped: 0, queueCapacity: 0, levels: new Map() };
    stateDirty = true;
    renderTuning();
    return;
  }
  setDescription({ version: PROTOCOL_VERSION, streams: [
    { deviceId: 1, stage: 0, key: "1:0", label: "Simulated raw" },
    { deviceId: 1, stage: 1, key: "1:1", label: "Simulated output" },
  ] });
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
  setStateStatus({ schemaVersion: 1, dropped: 0, queueCapacity: 64, levels: new Map([[0, STATE.OFF], [1, STATE.OFF]]) });
  let tick = 0;
  simulator = setInterval(() => {
    const raw = { deviceId: 1, stage: 0, key: "1:0", timestamp: tick * 8, sequence: tick * 2, x: Math.round(4 * Math.cos(tick / 14) + Math.random() * 2 - 1), y: Math.round(4 * Math.sin(tick / 14) + Math.random() * 2 - 1), wheel: 0, hWheel: 0 };
    addSample(raw);
    addSample({ ...raw, stage: 1, key: "1:1", sequence: tick * 2 + 1, x: Math.abs(raw.x) < 2 ? 0 : raw.x, y: Math.abs(raw.y) < 2 ? 0 : raw.y });
    const level = stateStatus.levels.get(0) ?? STATE.OFF;
    if (level === STATE.VERBOSE || (level === STATE.DECISIONS && tick % 20 === 0)) {
      addStateEvent({ targetId: 0, targetKind: 1, event: STATE.EVENT_FRAME,
        intent: Math.abs(raw.x) >= Math.abs(raw.y) ? 2 : 3,
        flags: tick % 20 === 0 ? STATE.FLAG_INTENT_CHANGED : 0,
        timestamp: raw.timestamp, sequence: tick * 3 + 2,
        values: [raw.x, raw.y, Math.abs(raw.x) * 4, Math.abs(raw.y) * 4, 0, 0, raw.x, raw.y, 0, 0] });
    }
    tick += 1;
  }, 8);
  elements.simulate.textContent = "Stop simulation";
  setStatus("Simulating", true);
}

elements.connect.addEventListener("click", () => (port ? disconnect() : connect()));
elements.telemetry.addEventListener("click", () => send(MESSAGE.TELEMETRY_CONTROL, Uint8Array.of(telemetryEnabled ? 0 : 1)));
elements.simulate.addEventListener("click", startSimulator);
elements.clear.addEventListener("click", () => {
  samples = [];
  stateEvents = [];
  stateDirty = true;
  for (const stream of streams.values()) Object.assign(stream, { count: 0, lastTimestamp: undefined, totalDt: 0, distance: 0, latest: {} });
  elements.export.disabled = true;
});
elements.export.addEventListener("click", () => {
  const payload = JSON.stringify({
    exportedAt: new Date().toISOString(),
    streams: [...streams.values()].map(({ count, lastTimestamp, totalDt, distance, latest, ...descriptor }) => descriptor),
    samples,
    stateTelemetry: protocolReady ? {
      schemaVersion: stateStatus.schemaVersion,
      dropped: stateStatus.dropped,
      queueCapacity: stateStatus.queueCapacity,
      events: stateEvents,
    } : undefined,
  }, null, 2);
  const link = document.createElement("a");
  link.href = URL.createObjectURL(new Blob([payload], { type: "application/json" }));
  link.download = `zmk-pointing-trace-${new Date().toISOString().replaceAll(":", "-")}.json`;
  link.click();
  URL.revokeObjectURL(link.href);
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
  const latestSequence = [...samples.slice(-1), ...stateEvents.slice(-1)]
    .reduce((latest, record) => Math.max(latest, record.sequence ?? 0), -1);
  return {
    nextSequence: latestSequence + 1,
    tuningProfile,
    streams: [...streams.values()].map(
      ({ count, lastTimestamp, totalDt, distance, latest, ...descriptor }) => descriptor,
    ),
    samples,
    stateEvents,
    stateSchemaVersion: stateStatus.schemaVersion,
    traceDropped,
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
