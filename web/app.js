import {
  FrameDecoder,
  MESSAGE,
  encodeFrame,
  parseAck,
  parseDescribe,
  parseSample,
} from "./protocol.js";

const USB_FILTERS = [{ usbVendorId: 0x16c0 }];
const COLORS = ["#78d6b0", "#e8c477", "#82aaff", "#ef8fa3", "#c099ff", "#79c7d9"];
const MAX_SAMPLES = 20_000;

const elements = Object.fromEntries(
  ["status", "connect", "telemetry", "simulate", "clear", "export", "notice", "trace", "streams", "sample-count"].map(
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
        <h3>${stream.label}</h3>
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
  if (frame.type === MESSAGE.DESCRIBE_RESPONSE) setDescription(parseDescribe(frame.payload));
  else if (frame.type === MESSAGE.ACK) {
    const ack = parseAck(frame.payload);
    telemetryEnabled = ack.enabled;
    elements.telemetry.textContent = ack.enabled ? "Stop telemetry" : "Start telemetry";
    if (ack.enabled && !heartbeat) heartbeat = setInterval(() => send(MESSAGE.PING), 2000);
    if (!ack.enabled && heartbeat) { clearInterval(heartbeat); heartbeat = undefined; }
    notice(`Telemetry ${ack.enabled ? "active" : "stopped"}; ${ack.dropped} device samples dropped.`);
  } else if (frame.type === MESSAGE.SAMPLE) addSample(parseSample(frame.payload));
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
  elements.telemetry.textContent = "Start telemetry";
  setStatus("Disconnected");
}

async function connect() {
  if (!("serial" in navigator)) {
    notice("Web Serial is unavailable. Use current desktop Chrome or the simulator.", true);
    return;
  }
  try {
    port = await navigator.serial.requestPort({ filters: USB_FILTERS });
    await port.open({ baudRate: 115200 });
    writer = port.writable.getWriter();
    readActive = true;
    readLoop();
    elements.connect.textContent = "Disconnect";
    elements.telemetry.disabled = false;
    setStatus("Connected", true);
    await send(MESSAGE.DESCRIBE_REQUEST);
  } catch (error) {
    notice(`Connection failed: ${error.message}`, true);
    await disconnect();
  }
}

function startSimulator() {
  if (simulator) {
    clearInterval(simulator);
    simulator = undefined;
    elements.simulate.textContent = "Simulate";
    setStatus(port ? "Connected" : "Disconnected", Boolean(port));
    return;
  }
  setDescription({ version: 1, streams: [
    { deviceId: 1, stage: 0, key: "1:0", label: "Simulated raw" },
    { deviceId: 1, stage: 1, key: "1:1", label: "Simulated output" },
  ] });
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

navigator.serial?.addEventListener("disconnect", (event) => { if (event.target === port) disconnect(); });
renderLoop();
