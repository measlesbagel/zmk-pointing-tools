import {
  FrameDecoder,
  MESSAGE,
  TUNING,
  encodeFrame,
  encodeTuningSet,
  parseAck,
  parseDescribe,
  parseSample,
  parseTuningDescription,
  parseTuningResult,
  parseTuningTargets,
} from "./protocol.js";

const USB_FILTERS = [{ usbVendorId: 0x16c0 }];
const COLORS = ["#78d6b0", "#e8c477", "#82aaff", "#ef8fa3", "#c099ff", "#79c7d9"];
const MAX_SAMPLES = 20_000;

const elements = Object.fromEntries(
  ["status", "connect", "telemetry", "simulate", "clear", "export", "notice", "trace", "streams", "sample-count", "tuning", "reset-all"].map(
    (id) => [id, document.getElementById(id)],
  ),
);

let port;
let reader;
let writer;
let readActive = false;
let telemetryEnabled = false;
let simulator;
let heartbeat;
let streams = new Map();
let tuningTargets = new Map();
let samples = [];
let decoder = new FrameDecoder();

function setStatus(text, live = false) {
  elements.status.textContent = text;
  elements.status.classList.toggle("live", live);
}

function notice(text, error = false) {
  elements.notice.textContent = text;
  elements.notice.classList.toggle("error", error);
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
  streams = new Map(
    description.streams.map((stream) => [stream.key, { ...stream, count: 0, lastTimestamp: undefined, totalDt: 0, distance: 0, latest: {} }]),
  );
  renderStreams();
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
  elements["reset-all"].disabled = tuningTargets.size === 0;
  if (!tuningTargets.size) {
    elements.tuning.innerHTML = '<p class="muted">Connect protocol v2 firmware to discover tunable processors.</p>';
    return;
  }

  elements.tuning.innerHTML = [...tuningTargets.values()]
    .map((target) => `<article class="tuning-target">
      <div class="tuning-target-heading">
        <div><h3>${escapeHtml(target.label)}</h3><span>Temporary preview values</span></div>
        <button data-reset-target="${target.id}" ${target.parameters.length ? "" : "disabled"}>Reset target</button>
      </div>
      ${target.parameters.length ? `<div class="parameters">${target.parameters.map((parameter) => {
        const changed = parameter.current !== parameter.compiled;
        const control = parameter.type === TUNING.BOOLEAN
          ? `<input type="checkbox" data-value ${parameter.current ? "checked" : ""}>`
          : `<input type="number" data-value value="${parameter.current}" min="${parameter.minimum}" max="${parameter.maximum}" step="${parameter.step}">`;
        return `<div class="parameter ${changed ? "changed" : ""}" data-target="${target.id}" data-parameter="${parameter.id}">
          <label>${escapeHtml(parameter.label)}${parameter.unit ? ` <span>(${escapeHtml(parameter.unit)})</span>` : ""}</label>
          <div class="parameter-control">${control}<button data-preview>Preview</button></div>
          <small>Compiled: ${parameter.compiled}${parameter.unit ? ` ${escapeHtml(parameter.unit)}` : ""}${changed ? " · modified" : ""}</small>
        </div>`;
      }).join("")}</div>` : '<p class="muted">Loading parameters…</p>'}
    </article>`)
    .join("");
}

function setTuningTargets(targets, requestDescriptions = true) {
  tuningTargets = new Map(targets.map((target) => [target.id, target]));
  renderTuning();
  if (requestDescriptions) {
    for (const target of targets) send(MESSAGE.TUNING_DESCRIBE_REQUEST, Uint8Array.of(target.id));
  }
}

function setTuningDescription(description) {
  const target = tuningTargets.get(description.targetId);
  if (!target) return;
  target.parameters = description.parameters;
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
  draw();
  requestAnimationFrame(renderLoop);
}

async function send(type, payload) {
  if (writer) await writer.write(encodeFrame(type, payload));
}

function handleFrame(frame) {
  if (frame.type === MESSAGE.DESCRIBE_RESPONSE) {
    const description = parseDescribe(frame.payload);
    setDescription(description);
    if (description.version >= 2) send(MESSAGE.TUNING_TARGETS_REQUEST);
  }
  else if (frame.type === MESSAGE.ACK) {
    const ack = parseAck(frame.payload);
    telemetryEnabled = ack.enabled;
    elements.telemetry.textContent = ack.enabled ? "Stop telemetry" : "Start telemetry";
    if (ack.enabled && !heartbeat) heartbeat = setInterval(() => send(MESSAGE.PING), 2000);
    if (!ack.enabled && heartbeat) { clearInterval(heartbeat); heartbeat = undefined; }
    notice(`Telemetry ${ack.enabled ? "active" : "stopped"}; ${ack.dropped} device samples dropped.`);
  } else if (frame.type === MESSAGE.SAMPLE) addSample(parseSample(frame.payload));
  else if (frame.type === MESSAGE.TUNING_TARGETS_RESPONSE) {
    setTuningTargets(parseTuningTargets(frame.payload));
  } else if (frame.type === MESSAGE.TUNING_DESCRIBE_RESPONSE) {
    setTuningDescription(parseTuningDescription(frame.payload));
  } else if (frame.type === MESSAGE.TUNING_RESULT) {
    const result = parseTuningResult(frame.payload);
    const target = tuningTargets.get(result.targetId);
    if (result.status !== TUNING.STATUS_OK) {
      const messages = ["success", "unknown target", "unknown parameter", "invalid value", "internal error"];
      notice(`Preview rejected: ${messages[result.status] ?? `status ${result.status}`}.`, true);
      if (target) send(MESSAGE.TUNING_DESCRIBE_REQUEST, Uint8Array.of(target.id));
    } else if (result.requestType === MESSAGE.TUNING_SET_REQUEST && target) {
      const parameter = target.parameters.find(({ id }) => id === result.parameterId);
      if (parameter) parameter.current = result.value;
      renderTuning();
      notice(`${target.label} preview updated. Changes remain temporary until reboot.`);
    } else if (result.requestType === MESSAGE.TUNING_RESET_REQUEST) {
      const resetTargets = result.targetId === TUNING.ALL_TARGETS ? [...tuningTargets.values()] : [target].filter(Boolean);
      for (const item of resetTargets) send(MESSAGE.TUNING_DESCRIBE_REQUEST, Uint8Array.of(item.id));
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
  try { if (writer && telemetryEnabled) await send(MESSAGE.TELEMETRY_CONTROL, Uint8Array.of(0)); } catch {}
  readActive = false;
  try { if (reader) await reader.cancel(); } catch {}
  try { if (writer) writer.releaseLock(); } catch {}
  try { if (port) await port.close(); } catch {}
  port = writer = undefined;
  telemetryEnabled = false;
  if (heartbeat) clearInterval(heartbeat);
  heartbeat = undefined;
  elements.connect.textContent = "Connect keyboard";
  elements.telemetry.disabled = true;
  elements.simulate.disabled = false;
  elements.telemetry.textContent = "Start telemetry";
  tuningTargets = new Map();
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
    readActive = true;
    readLoop();
    elements.connect.textContent = "Disconnect";
    elements.telemetry.disabled = false;
    elements.simulate.disabled = true;
    streams = new Map();
    tuningTargets = new Map();
    samples = [];
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
    renderTuning();
    return;
  }
  setDescription({ version: 2, streams: [
    { deviceId: 1, stage: 0, key: "1:0", label: "Simulated raw" },
    { deviceId: 1, stage: 1, key: "1:1", label: "Simulated output" },
  ] });
  setTuningTargets([{ id: 0, kind: 1, label: "Simulated adaptive scroll", parameters: [
    { id: 6, type: TUNING.INTEGER, minimum: 1, maximum: 10000, step: 1, compiled: 16, current: 16, label: "Activation distance", unit: "counts" },
    { id: 9, type: TUNING.INTEGER, minimum: 0, maximum: 500, step: 1, compiled: 75, current: 75, label: "Physical keypress guard", unit: "ms" },
    { id: 10, type: TUNING.BOOLEAN, minimum: 0, maximum: 1, step: 1, compiled: 0, current: 0, label: "Discard unclassified motion", unit: "" },
  ] }], false);
  let tick = 0;
  simulator = setInterval(() => {
    const raw = { deviceId: 1, stage: 0, key: "1:0", timestamp: tick * 8, sequence: tick * 2, x: Math.round(4 * Math.cos(tick / 14) + Math.random() * 2 - 1), y: Math.round(4 * Math.sin(tick / 14) + Math.random() * 2 - 1), wheel: 0, hWheel: 0 };
    addSample(raw);
    addSample({ ...raw, stage: 1, key: "1:1", sequence: tick * 2 + 1, x: Math.abs(raw.x) < 2 ? 0 : raw.x, y: Math.abs(raw.y) < 2 ? 0 : raw.y });
    tick += 1;
  }, 8);
  elements.simulate.textContent = "Stop simulation";
  setStatus("Simulating", true);
}

elements.connect.addEventListener("click", () => (port ? disconnect() : connect()));
elements.telemetry.addEventListener("click", () => send(MESSAGE.TELEMETRY_CONTROL, Uint8Array.of(telemetryEnabled ? 0 : 1)));
elements.simulate.addEventListener("click", startSimulator);
elements.clear.addEventListener("click", () => { samples = []; for (const stream of streams.values()) Object.assign(stream, { count: 0, lastTimestamp: undefined, totalDt: 0, distance: 0, latest: {} }); elements.export.disabled = true; });
elements.export.addEventListener("click", () => {
  const payload = JSON.stringify({ exportedAt: new Date().toISOString(), streams: [...streams.values()].map(({ count, lastTimestamp, totalDt, distance, latest, ...descriptor }) => descriptor), samples }, null, 2);
  const link = document.createElement("a");
  link.href = URL.createObjectURL(new Blob([payload], { type: "application/json" }));
  link.download = `zmk-pointing-trace-${new Date().toISOString().replaceAll(":", "-")}.json`;
  link.click();
  URL.revokeObjectURL(link.href);
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
      await writer.write(encodeTuningSet(targetId, parameterId, value));
    }
  } else if (reset) {
    const targetId = Number(reset.dataset.resetTarget);
    if (simulator) {
      const target = tuningTargets.get(targetId);
      for (const parameter of target.parameters) parameter.current = parameter.compiled;
      renderTuning();
      notice(`${target.label} simulator defaults restored.`);
    } else {
      await send(MESSAGE.TUNING_RESET_REQUEST, Uint8Array.of(targetId));
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
    await send(MESSAGE.TUNING_RESET_REQUEST, Uint8Array.of(TUNING.ALL_TARGETS));
  }
});

navigator.serial?.addEventListener("disconnect", (event) => { if (event.target === port) disconnect(); });
renderLoop();
renderTuning();
