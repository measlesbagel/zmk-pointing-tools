import {
  FrameDecoder,
  DEVICE,
  MESSAGE,
  PROTOCOL_VERSION,
  STATE,
  TUNING,
  encodeDeviceClearSaved,
  encodeDevicePreview,
  encodeDeviceSave,
  encodeFrame,
  encodeStateControl,
  encodeTuningClearSaved,
  encodeTuningSave,
  encodeTuningSet,
  encodeTuningSetMany,
  parseAck,
  parseDescribe,
  parseDeviceDescription,
  parseDeviceList,
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
  SIMULATED_STAGES,
  buildSimulatorEvent,
  buildSimulatorFrame,
  buildSimulatorState,
  buildSimulatorTuningTargets,
} from "./simulator-model.js";
import { findCurve } from "./acceleration-curve.js";
import { CurveEditors } from "./curve-editor.js";
import {
  createTuningProfile,
  prepareProfileImport,
  renderDevicetreeSnippet,
} from "./profile.js";
import { ResponseRequestQueue } from "./request-queue.js";
import { applicableGames, gamesForSetting } from "./games.js";
import {
  CompassRunner, PixelRunner, PursuitRunner, RailRunner, ScrollBandRunner, StillRunner, TargetGameRunner, analyseFlicks,
} from "./game-runner.js";
import { RAIL_COURSES, RAIL_TIMEOUT_MS, RAIL_WIDTH, analyseRail, finished as railFinished, formatRail } from "./rail.js";
import { BAND_HOLD_MS, SCROLL_TIMEOUT_MS, analyseScrollBand, formatScrollBand, scrollTrials } from "./scroll-band.js";
import { analyseStillHands, formatStillHands } from "./still-hands.js";
import { PURSUIT_PHASES, analysePursuit, formatPursuit, measurePursuitPhase, pursuitPosition } from "./pursuit.js";
import { HOLD_MS, PIXEL_TIMEOUT_MS, analysePixel, formatPixel, onTarget, pixelTrials } from "./pixel.js";
import { analyseCompass, cueDirections } from "./compass.js";
import { compareConditions, describeComparison, describePreference, hiddenLabels, planBlocks } from "./comparison.js";
import { SWEEP_MEASURES, describeSweep, planSweep, summarizeSweep, sweepChart, sweepValues } from "./sweep.js";
import { seededRandom, summarizeBy } from "./measure.js";
import { clearHistory, formatRun, loadHistory, saveRun } from "./game-history.js";
import { flickFlags, flickTrials, ringLevels, ringSequence } from "./target-games.js";
import { PointingPlayground } from "./playground.js";
import { CalibrationPanel } from "./calibration.js";
import { SIGNAL_KIND, STAGE_FRAME } from "./calibration-model.js";
import { BluetoothTransport, SerialTransport } from "./transport.js";

const MAX_STATE_EVENTS = 2_000;
const INTENT_LABELS = ["undecided", "free", "horizontal", "vertical"];
const STAGE_EVENT_LABELS = ["suppressed", "discarded", "qualified", "intent-changed", "flushed", "action"];
const STAGE_EVENT_INTENT_CHANGED = 3;
const SIGNAL_KIND_LABELS = ["invalid", "raw", "normalized", "pointer", "scroll", "action"];

const elements = Object.fromEntries(
  ["status", "connect", "connect-bluetooth", "simulate", "clear", "notice", "tuning", "devices", "reset-all", "save-all", "profile-export", "profile-copy", "profile-import", "profile-file", "modified-count", "diagnostics", "state-count", "state-dropped"].map(
    (id) => [id, document.getElementById(id)],
  ),
);

let transport;
let protocolReady = false;
/* Whether the firmware can keep values across a reboot. */
let savingSupported = false;
let simulator;
let heartbeat;
let tuningTargets = new Map();
let devices = new Map();
let stateEvents = [];
let stateStatus = { schemaVersion: 0, dropped: 0, queueCapacity: 0, levels: new Map(), labels: new Map() };
let stateDirty = true;
let decoder = new FrameDecoder();
let calibration;
const tuningRequests = new ResponseRequestQueue(
  (frame) => {
    if (!transport) throw new Error("The keyboard connection is unavailable");
    return transport.write(frame);
  },
  (error) => notice(`Tuning request failed: ${error.message}`, true),
  {
    /* Longer than the slowest real response: a peripheral relay waits
     * 500 ms and a large description over Bluetooth takes several
     * connection events. After a timeout the tuner can't tell which
     * response answers which request, so the session ends. */
    timeoutMs: 5_000,
    onTimeout: () => {
      if (!transport) return;
      notice("The keyboard stopped responding. Reconnect to continue.", true);
      disconnect();
    },
  },
);

const curveEditors = new CurveEditors({
  escapeHtml,
  getCurve: (targetId) => {
    const target = tuningTargets.get(targetId);
    return target ? findCurve(target) : null;
  },
  onPreview: (targetId, values) => previewValues(targetId, values, "curve"),
  onRender: renderTuning,
});
curveEditors.bind(elements.tuning);

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
  const needed = Boolean(transport) && stateEnabled;
  if (needed && !heartbeat) heartbeat = setInterval(() => send(MESSAGE.PING).catch(() => {}), 2000);
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
  setSavingSupported(description.saving);
  stateDirty = true;
  notice(`Protocol v${description.version} connected${savingSupported ? "" : "; this build can't save values on the device"}.`);
}

function setSavingSupported(supported) {
  savingSupported = supported;
  elements["save-all"].hidden = !supported;
  /* With saving, resetting returns to what a reboot would run. */
  elements["reset-all"].textContent = supported ? "Discard all" : "Reset all defaults";
}

/* What a value runs on after a reboot: the saved one, or compiled. */
function bootValue(parameter) {
  return parameter.saved ?? parameter.compiled;
}

function unitSuffix(parameter) {
  return parameter.unit ? ` ${escapeHtml(parameter.unit)}` : "";
}

/* "Compiled 75 ms · saved 90 ms · previewing", leaving out what doesn't
 * apply. */
function parameterProvenance(parameter) {
  const parts = [`Compiled: ${parameter.compiled}${unitSuffix(parameter)}`];
  if (parameter.saved !== undefined) parts.push(`saved: ${parameter.saved}${unitSuffix(parameter)}`);
  if (parameter.current !== bootValue(parameter)) parts.push(savingSupported ? "previewing, not saved" : "modified");
  return parts.join(" · ");
}



/* Whether every target has the metadata a profile export needs. */
function isProfileReady() {
  const targets = [...tuningTargets.values()];
  return targets.length > 0 && targets.every(
    (target) => target.stableId && target.devicetreePath && target.parameters.length > 0 &&
      target.parameters.every((parameter) => parameter.key && parameter.devicetreeProperty),
  );
}

function renderTuning() {
  /* Rebuilding the list mid-drag would drop the dragged point. */
  if (curveEditors.dragging) {
    curveEditors.renderPending = true;
    return;
  }
  const targets = [...tuningTargets.values()];
  const modified = targets.reduce(
    (count, target) => count + target.parameters.filter((parameter) => parameter.current !== bootValue(parameter)).length,
    0,
  );
  const saved = targets.reduce(
    (count, target) => count + target.parameters.filter((parameter) => parameter.saved !== undefined).length,
    0,
  );
  const profileReady = isProfileReady();
  elements["reset-all"].disabled = tuningTargets.size === 0;
  elements["save-all"].disabled = tuningTargets.size === 0 || modified === 0;
  elements["profile-export"].disabled = !profileReady;
  elements["profile-copy"].disabled = !profileReady;
  elements["profile-import"].disabled = !profileReady;
  elements["modified-count"].textContent = savingSupported
    ? `${modified} unsaved · ${saved} saved`
    : `${modified} modified`;
  calibration?.refresh();
  if (!tuningTargets.size) {
    elements.tuning.innerHTML = '<p class="muted">This firmware has no tunable pipeline stages. Gate, acceleration, transfer, coalescer, scroll, text, axis-intent, and axis-constraint stages appear here.</p>';
    renderGames([]);
    return;
  }

  const games = applicableGames(tuningTargets.values());
  elements.tuning.innerHTML = [...tuningTargets.values()]
    .map((target) => {
      /* An acceleration curve gets the graphical editor; its other
       * parameters keep the ordinary controls. */
      const curve = findCurve(target);
      if (curve) curveEditors.settle(target.id, curve);
      const parameters = curve ? target.parameters.filter(({ id }) => !curve.ids.has(id)) : target.parameters;
      const empty = target.parameters.length === 0;
      const unsaved = target.parameters.some((parameter) => parameter.current !== bootValue(parameter));
      const hasSaved = target.parameters.some((parameter) => parameter.saved !== undefined);
      const subtitle = !savingSupported
        ? "Temporary preview values"
        : unsaved ? "Previewing unsaved changes" : hasSaved ? "Running saved values" : "Running compiled values";
      /* With saving, Discard goes back to what a reboot would run and Reset
       * to compiled also forgets what was saved. */
      const actions = savingSupported
        ? `<button data-save-target="${target.id}" ${empty || !unsaved ? "disabled" : ""}>Save</button>
        <button data-reset-target="${target.id}" ${empty || !unsaved ? "disabled" : ""}>Discard</button>
        <button data-clear-target="${target.id}" ${empty || (!hasSaved && !unsaved) ? "disabled" : ""}>Reset to compiled</button>`
        : `<button data-reset-target="${target.id}" ${empty ? "disabled" : ""}>Reset target</button>`;
      return `<article class="tuning-target">
      <div class="tuning-target-heading">
        <div><h3>${escapeHtml(target.label)}</h3><span>${target.stableId ? `<code>${escapeHtml(target.stableId)}</code> · ` : ""}${subtitle}</span></div>
        <div class="tuning-target-actions">${actions}</div>
      </div>
      ${curve ? curveEditors.render(target.id, curve) : ""}
      ${parameters.length ? `<div class="parameters">${parameters.map((parameter) => renderParameterRow(target, parameter, { games })).join("")}</div>`
        : (curve ? "" : '<p class="muted">Loading parameters…</p>')}
    </article>`;
    })
    .join("");
  renderGames(games);
}

/* One setting's row. In the settings menu it links to the games that use
 * it; on a game screen it names its stage and links back to the menu. */
function renderParameterRow(target, parameter, { games, inGame = false } = {}) {
  const changed = parameter.current !== bootValue(parameter);
  const control = parameter.type === TUNING.BOOLEAN
    ? `<input type="checkbox" data-value ${parameter.current ? "checked" : ""}>`
    : `<input type="number" data-value value="${parameter.current}" min="${parameter.minimum}" max="${parameter.maximum}" step="${parameter.step}">`;
  const help = parameter.description
    ? `<span class="parameter-help" tabindex="0" role="img" aria-label="${escapeHtml(parameter.description)}" data-tooltip="${escapeHtml(parameter.description)}">?</span>`
    : "";
  const links = inGame
    ? `<small class="game-links">${escapeHtml(target.label)} · <a href="#setting-${target.id}-${parameter.id}" data-show-setting="${target.id}-${parameter.id}">in settings</a></small>`
    : (() => {
      const used = games ? gamesForSetting(games, target.id, parameter.id) : [];
      return used.length
        ? `<small class="game-links">Played in ${used.map(({ id, title }) => `<a href="#game-${id}" data-open-game="${id}">${escapeHtml(title)}</a>`).join(", ")}</small>`
        : "";
    })();
  const anchor = inGame ? "" : ` id="setting-${target.id}-${parameter.id}"`;
  return `<div class="parameter ${changed ? "changed" : ""}"${anchor} data-target="${target.id}" data-parameter="${parameter.id}">
    <label><span>${escapeHtml(parameter.label)}${parameter.unit ? ` <small>(${escapeHtml(parameter.unit)})</small>` : ""}</span>${help}</label>
    <div class="parameter-control">${control}<button data-preview>Preview</button></div>
    <small>${parameterProvenance(parameter)}</small>
    ${links}
  </div>`;
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

function describeStageFrame(event) {
  const [, kind, x, y, cpi] = event.values;
  const motion = kind === SIGNAL_KIND.NORMALIZED_MOTION
    ? `${(x / 65536).toFixed(2)}, ${(y / 65536).toFixed(2)} mm`
    : `${x}, ${y}`;
  return `frame · ${SIGNAL_KIND_LABELS[kind] ?? `kind ${kind}`} ${motion}${cpi ? ` · ${cpi} CPI` : ""}`;
}

function describeStateEvent(event) {
  if (event.targetKind === 4 && event.values[0] === STAGE_FRAME) return describeStageFrame(event);
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
    elements.diagnostics.innerHTML = '<p class="muted">Connect current firmware to inspect stage decisions.</p>';
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
  calibration?.refresh();
}

function setStateLevel(targetId, level) {
  if (simulator) {
    stateStatus.levels.set(targetId, level);
    stateDirty = true;
    calibration?.refresh();
  } else if (transport && protocolReady) {
    tuningRequests.enqueue(MESSAGE.STATE_CONTROL_REQUEST, encodeStateControl(targetId, level));
  }
}

function addStateEvent(event) {
  calibration?.handleSample(event);
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

/* Help text, keys, and devicetree properties are fixed per firmware, so a
 * refreshed description keeps what earlier responses brought and only asks
 * for what is still missing. */
function setTuningDescription(description) {
  const target = tuningTargets.get(description.targetId);
  if (!target) return;
  const known = new Map(target.parameters.map((parameter) => [parameter.id, parameter]));
  target.parameters = description.parameters.map((parameter) => {
    const { description: help, key, devicetreeProperty } = known.get(parameter.id) ?? {};
    return { ...parameter, ...(help === undefined ? {} : { description: help }),
      ...(key === undefined ? {} : { key, devicetreeProperty }) };
  });
  renderTuning();
  stateDirty = true;
  for (const parameter of target.parameters.filter(({ description: help }) => help === undefined)) {
    queueTuningRequest(MESSAGE.TUNING_HELP_REQUEST, Uint8Array.of(target.id, parameter.id));
  }
  for (const parameter of target.parameters.filter(({ key }) => key === undefined)) {
    queueTuningRequest(MESSAGE.TUNING_PARAMETER_METADATA_REQUEST,
      Uint8Array.of(target.id, parameter.id));
  }
}

/* Send values as one batch so the keyboard never runs half of a curve. */
function previewValues(targetId, values, what = "preview") {
  const target = tuningTargets.get(targetId);
  if (!target) return;
  if (simulator) {
    for (const { parameterId, value } of values) {
      target.parameters.find(({ id }) => id === parameterId).current = value;
    }
    renderTuning();
    notice(`${target.label} simulator ${what} updated.`);
  } else if (transport) {
    tuningRequests.enqueue(MESSAGE.TUNING_SET_MANY_REQUEST, encodeTuningSetMany(targetId, values));
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

function setDevices(list) {
  devices = new Map(list.map((device) => [device.id, { ...device, description: undefined }]));
  renderDevices();
  for (const device of devices.values()) {
    if (device.settable) {
      queueTuningRequest(MESSAGE.DEVICE_DESCRIBE_REQUEST, Uint8Array.of(device.id));
    }
  }
  renderCapabilityChecks();
}

/* Device descriptions echo their stable identity instead of a numeric id,
 * and labels are the stable ids today. */
function setDeviceDescription(description) {
  const device = [...devices.values()].find((candidate) => candidate.label === description.stableId);
  if (!device) return;
  device.description = description;
  /* Describing a peripheral's device reads it live through that half, so
   * the answer is fresher than the list's reachability flag. */
  if (device.location !== DEVICE.LOCATION_LOCAL) device.reachable = !description.stale;
  renderDevices();
}

function locationLabel(location) {
  return location === DEVICE.LOCATION_LOCAL ? "this half" : `peripheral ${location}`;
}

function renderDevices() {
  const supported = protocolReady || simulator;
  if (!supported || devices.size === 0) {
    elements.devices.innerHTML = supported
      ? '<p class="muted">No pointing devices declared in the active configuration.</p>'
      : '<p class="muted">Connect current firmware to discover pointing devices.</p>';
    return;
  }

  elements.devices.innerHTML = [...devices.values()].map((device) => {
    const described = device.description;
    const remote = device.location !== DEVICE.LOCATION_LOCAL;
    const badge = `${escapeHtml(locationLabel(device.location))}${remote && !device.reachable ? " · not reachable" : ""}`;
    let body = '<p class="muted">Read-only sensor; resolution is fixed by configuration.</p>';
    if (device.settable && !described) {
      body = '<p class="muted">Loading capabilities…</p>';
    } else if (described) {
      const bootCpi = described.savedCpi ?? described.defaultCpi;
      const modified = described.currentCpi !== bootCpi;
      const provenance = [`Compiled: ${described.defaultCpi} CPI`];
      if (described.savedCpi !== undefined) provenance.push(`saved: ${described.savedCpi} CPI`);
      if (modified) provenance.push(savingSupported ? "previewing, not saved" : "modified");
      const control = described.discrete
        ? `<select data-device-value>
            ${described.values.map((value) => `<option value="${value}" ${value === described.currentCpi ? "selected" : ""}>${value} CPI</option>`).join("")}
          </select>`
        : `<input type="number" data-device-value value="${described.currentCpi}" min="${described.range.min}" max="${described.range.max}" step="${described.range.step}">`;
      body = `<div class="parameter ${modified ? "changed" : ""}">
        <label><span>Sensor resolution <small>(CPI)</small></span></label>
        <div class="parameter-control">${control}<button data-device-preview>Preview</button></div>
        <small>${provenance.join(" · ")}</small>
        ${device.error ? `<small class="device-error">${escapeHtml(device.error)}</small>` : ""}
      </div>`;
    }
    const bootCpi = described ? (described.savedCpi ?? described.defaultCpi) : undefined;
    const unsaved = Boolean(described) && described.currentCpi !== bootCpi;
    const hasSaved = described?.savedCpi !== undefined;
    const actions = savingSupported && device.settable
      ? `<button data-device-save ${unsaved ? "" : "disabled"}>Save</button>
        <button data-device-reset ${unsaved ? "" : "disabled"}>Discard</button>
        <button data-device-clear ${hasSaved || unsaved ? "" : "disabled"}>Reset to compiled</button>`
      : `<button data-device-reset ${device.settable ? "" : "disabled"}>Reset default</button>`;
    /* The whole card carries the id: the actions sit in the heading,
     * outside the resolution row. */
    return `<article class="tuning-target" data-device="${device.id}">
      <div class="tuning-target-heading">
        <div><h3>${escapeHtml(device.label)}</h3><span>${badge}</span></div>
        <div class="tuning-target-actions">${actions}</div>
      </div>
      ${body}
    </article>`;
  }).join("");
}


function renderLoop() {
  renderDiagnostics();
  requestAnimationFrame(renderLoop);
}

async function send(type, payload) {
  if (transport) await transport.write(encodeFrame(type, payload));
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
    queueTuningRequest(MESSAGE.DEVICE_LIST_REQUEST);
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
  } else if (frame.type === MESSAGE.DEVICE_LIST_RESPONSE) {
    tuningRequests.complete(MESSAGE.DEVICE_LIST_REQUEST);
    setDevices(parseDeviceList(frame.payload));
  } else if (frame.type === MESSAGE.DEVICE_DESCRIPTION_RESPONSE) {
    tuningRequests.complete(MESSAGE.DEVICE_DESCRIBE_REQUEST);
    setDeviceDescription(parseDeviceDescription(frame.payload));
  } else if (frame.type === MESSAGE.TUNING_RESULT) {
    const result = parseTuningResult(frame.payload);
    tuningRequests.complete(result.requestType);
    if ([MESSAGE.DEVICE_SAVE_REQUEST, MESSAGE.DEVICE_CLEAR_SAVED_REQUEST].includes(result.requestType)) {
      handleDeviceSavingResult(result);
      return;
    }
    if ([MESSAGE.TUNING_SAVE_REQUEST, MESSAGE.TUNING_CLEAR_SAVED_REQUEST].includes(result.requestType)) {
      handleTuningSavingResult(result);
      return;
    }
    if (result.requestType === MESSAGE.DEVICE_PREVIEW_REQUEST) {
      /* Device ids ride the result's parameter-id byte; target-id is the
       * all-targets sentinel for device results. */
      const device = devices.get(result.parameterId);
      if (result.status !== TUNING.STATUS_OK) {
        const messages = ["success", "unknown device", "unknown parameter", "not applicable to this device", "internal error", "the half that owns it didn't answer"];
        const reason = messages[result.status] ?? `status ${result.status}`;
        if (device) {
          /* The page notice sits far above this panel, so the card says it
           * too. */
          device.error = `Preview rejected: ${reason}.`;
          if (result.status === DEVICE.STATUS_UNREACHABLE) device.reachable = false;
          renderDevices();
        }
        notice(`Device preview rejected: ${reason}.`, true);
      } else if (device) {
        device.reachable = true;
        device.error = undefined;
        if (device.description) device.description.currentCpi = result.value;
        renderDevices();
        notice(`${device.label} resolution preview updated.${savingSupported ? " Save to keep it after a reboot." : " Changes remain temporary until reboot."}`);
      }
      return;
    }
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
      notice(`${target.label} preview updated.${savingSupported ? " Save to keep it after a reboot." : " Changes remain temporary until reboot."}`);
    } else if (result.requestType === MESSAGE.TUNING_SET_MANY_REQUEST && target) {
      queueTuningRequest(MESSAGE.TUNING_DESCRIBE_REQUEST, Uint8Array.of(target.id));
      notice(`${target.label} values applied atomically as a temporary preview.`);
    } else if (result.requestType === MESSAGE.TUNING_RESET_REQUEST) {
      const resetTargets = result.targetId === TUNING.ALL_TARGETS ? [...tuningTargets.values()] : [target].filter(Boolean);
      for (const item of resetTargets) queueTuningRequest(MESSAGE.TUNING_DESCRIBE_REQUEST, Uint8Array.of(item.id));
      const restored = savingSupported ? "previews discarded" : "defaults restored";
      notice(result.targetId === TUNING.ALL_TARGETS ? `All ${restored}.` : `${target?.label ?? "Target"} ${restored}.`);
    }
  }
}

const SAVING_FAILURES = ["success", "unknown target", "unknown parameter", "invalid value", "storage error", "the half that owns it didn't answer", "this build can't save values"];

function savingFailure(status) {
  return SAVING_FAILURES[status] ?? `status ${status}`;
}

function handleTuningSavingResult(result) {
  const all = result.targetId === TUNING.ALL_TARGETS;
  const affected = all ? [...tuningTargets.values()] : [tuningTargets.get(result.targetId)].filter(Boolean);
  /* Re-read either way: a batch save can fail partway. */
  for (const item of affected) queueTuningRequest(MESSAGE.TUNING_DESCRIBE_REQUEST, Uint8Array.of(item.id));
  const what = all ? "All targets" : (affected[0]?.label ?? "Target");
  const saving = result.requestType === MESSAGE.TUNING_SAVE_REQUEST;
  if (result.status === TUNING.STATUS_UNSUPPORTED) {
    setSavingSupported(false);
    renderTuning();
    renderDevices();
  }
  if (result.status !== TUNING.STATUS_OK) {
    notice(`${saving ? "Save" : "Reset"} failed: ${savingFailure(result.status)}.`, true);
  } else {
    notice(saving ? `${what} saved on the device.` : `${what} back to compiled values; nothing saved.`);
  }
}

function handleDeviceSavingResult(result) {
  const device = devices.get(result.parameterId);
  const saving = result.requestType === MESSAGE.DEVICE_SAVE_REQUEST;
  if (result.status !== TUNING.STATUS_OK) {
    const reason = savingFailure(result.status);
    if (device) {
      device.error = `${saving ? "Save" : "Reset"} failed: ${reason}.`;
      renderDevices();
    }
    notice(`Device ${saving ? "save" : "reset"} failed: ${reason}.`, true);
    return;
  }
  if (device) {
    device.error = undefined;
    queueTuningRequest(MESSAGE.DEVICE_DESCRIBE_REQUEST, Uint8Array.of(device.id));
  }
  const label = device?.label ?? "Device";
  notice(saving ? `${label} resolution saved on the device.` : `${label} back to its compiled resolution.`);
}

function receive(bytes) {
  for (const frame of decoder.push(bytes)) handleFrame(frame);
}

async function disconnect() {
  const link = transport;
  /* Detach first so late responses can't send more requests on a closing
   * link. */
  transport = undefined;
  tuningRequests.clear();
  elements.connect.disabled = true;
  /* Best effort, and bounded: a stalled link mustn't hold the disconnect. */
  try {
    if (link && protocolReady) {
      await Promise.race([
        link.write(encodeStateControl(STATE.ALL_TARGETS, STATE.OFF)),
        new Promise((resolve) => setTimeout(resolve, 500)),
      ]);
    }
  } catch {}
  await link?.close();
  elements.connect.disabled = false;
  protocolReady = false;
  setSavingSupported(false);
  calibration.cancel();
  if (heartbeat) clearInterval(heartbeat);
  heartbeat = undefined;
  elements.connect.textContent = "Connect via USB";
  elements["connect-bluetooth"].hidden = false;
  elements.simulate.disabled = false;
  tuningTargets = new Map();
  devices = new Map();
  stateEvents = [];
  stateStatus = { schemaVersion: 0, dropped: 0, queueCapacity: 0, levels: new Map(), labels: new Map() };
  stateDirty = true;
  renderTuning();
  setStatus("Disconnected");
}

const UNSUPPORTED = new Map([
  [SerialTransport, "Web Serial is unavailable. Use current desktop Chrome or Edge, or the simulator."],
  [BluetoothTransport, "Web Bluetooth is unavailable. Use current desktop Chrome or Edge; on Linux, turn on Experimental Web Platform features in chrome://flags if it stays unavailable."],
]);

async function connect(Transport) {
  if (!Transport.supported) {
    notice(UNSUPPORTED.get(Transport), true);
    return;
  }
  elements.connect.disabled = elements["connect-bluetooth"].disabled = true;
  try {
    if (simulator) startSimulator();
    decoder = new FrameDecoder();
    const link = new Transport({
      onData: (bytes) => { if (transport === link) receive(bytes); },
      onError: (error) => notice(`${link.label} read failed: ${error.message}`, true),
      onClose: () => { if (transport === link) disconnect(); },
    });
    await link.open();
    transport = link;
    protocolReady = false;
    setSavingSupported(false);
    elements.connect.textContent = "Disconnect";
    elements["connect-bluetooth"].hidden = true;
    elements.simulate.disabled = true;
    tuningTargets = new Map();
    devices = new Map();
    stateEvents = [];
    stateStatus = { schemaVersion: 0, dropped: 0, queueCapacity: 0, levels: new Map(), labels: new Map() };
    stateDirty = true;
    tuningRequests.clear();
    renderTuning();
    setStatus(`Connected over ${link.label}`, true);
    await send(MESSAGE.DESCRIBE_REQUEST);
  } catch (error) {
    notice(`Connection failed: ${error.message}`, true);
    await disconnect();
  } finally {
    elements.connect.disabled = elements["connect-bluetooth"].disabled = false;
    /* The Bluetooth button hides while connected, which would drop focus. */
    elements.connect.focus();
  }
}

function startSimulator() {
  if (transport) {
    notice("Disconnect the keyboard before starting the simulator.", true);
    return;
  }
  if (simulator) {
    clearInterval(simulator);
    simulator = undefined;
    calibration.cancel();
    elements.simulate.textContent = "Simulate";
    setStatus(transport ? "Connected" : "Disconnected", Boolean(transport));
    tuningTargets = new Map();
    devices = new Map();
    protocolReady = false;
    setSavingSupported(false);
    stateEvents = [];
    stateStatus = { schemaVersion: 0, dropped: 0, queueCapacity: 0, levels: new Map(), labels: new Map() };
    stateDirty = true;
    renderTuning();
    return;
  }
  setDescription({ version: PROTOCOL_VERSION });
  tuningTargets = new Map(buildSimulatorTuningTargets().map((target) => [target.id, target]));
  renderTuning();
  setStateStatus(buildSimulatorState());
  let sequence = 0;
  let tick = 0;
  simulator = setInterval(() => {
    const emit = (targetId, event, flags, values) => {
      const level = stateStatus.levels.get(targetId) ?? STATE.OFF;
      if (level === STATE.OFF) return;
      sequence += 1;
      addStateEvent(buildSimulatorEvent(targetId, event, flags, tick * 8, sequence, values));
    };
    if (tick % 24 === 0) {
      emit(1, STATE.EVENT_FRAME, STATE.FLAG_QUALIFIED, [STAGE_EVENT_LABELS.indexOf("qualified"), 0]);
    }
    if (tick % 120 === 60) {
      emit(1, STATE.EVENT_FRAME, STATE.FLAG_DISCARDED, [STAGE_EVENT_LABELS.indexOf("discarded"), 0]);
    }
    const intent = tick % 80 < 40 ? 2 : 3;
    if (tick % 40 === 0) {
      emit(5, STATE.EVENT_FRAME, STATE.FLAG_INTENT_CHANGED,
        [STAGE_EVENT_INTENT_CHANGED, intent]);
    }
    if (tick % 90 === 30) {
      emit(6, STATE.EVENT_FRAME, STATE.FLAG_SUPPRESSED, [STAGE_EVENT_LABELS.indexOf("suppressed"), 0]);
    }
    if (tick % 24 === 12) {
      emit(7, STATE.EVENT_FLUSH, STATE.FLAG_OUTPUT, [STAGE_EVENT_LABELS.indexOf("flushed"), 3 + (tick % 4)]);
    }
    for (const { id, label } of SIMULATED_STAGES) {
      if (stateStatus.levels.get(id) !== STATE.VERBOSE) continue;
      const values = buildSimulatorFrame(label, tick);
      if (values) emit(id, STATE.EVENT_FRAME, 0, values);
    }
    tick += 1;
  }, 8);
  elements.simulate.textContent = "Stop simulation";
  setStatus("Simulating", true);
  calibration.refresh();
}

elements.connect.addEventListener("click", () => (transport ? disconnect() : connect(SerialTransport)));
elements["connect-bluetooth"].addEventListener("click", () => connect(BluetoothTransport));
elements.simulate.addEventListener("click", startSimulator);
elements.clear.addEventListener("click", () => {
  stateEvents = [];
  stateDirty = true;
});

elements.diagnostics.addEventListener("change", (event) => {
  const select = event.target.closest("[data-state-target]");
  if (!select) return;
  setStateLevel(Number(select.dataset.stateTarget), Number(select.value));
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
  const save = event.target.closest("[data-save-target]");
  const clear = event.target.closest("[data-clear-target]");
  if (save) {
    queueTuningRequest(MESSAGE.TUNING_SAVE_REQUEST, Uint8Array.of(Number(save.dataset.saveTarget)));
    return;
  }
  if (clear) {
    const targetId = Number(clear.dataset.clearTarget);
    curveEditors.discard(targetId);
    tuningRequests.enqueue(MESSAGE.TUNING_CLEAR_SAVED_REQUEST, encodeTuningClearSaved(targetId));
    return;
  }
  const gameLink = event.target.closest("[data-open-game]");
  if (gameLink) {
    event.preventDefault();
    openGame(gameLink.dataset.openGame);
    return;
  }
  if (preview) {
    previewParameterRow(preview.closest("[data-target][data-parameter]"));
  } else if (reset) {
    resetTarget(Number(reset.dataset.resetTarget));
  }
});

function previewParameterRow(row) {
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
  } else if (transport) {
    tuningRequests.enqueue(MESSAGE.TUNING_SET_REQUEST, encodeTuningSet(targetId, parameterId, value));
  }
}

function resetTarget(targetId) {
  curveEditors.discard(targetId);
  if (simulator) {
    const target = tuningTargets.get(targetId);
    for (const parameter of target.parameters) parameter.current = parameter.compiled;
    renderTuning();
    notice(`${target.label} simulator defaults restored.`);
  } else {
    queueTuningRequest(MESSAGE.TUNING_RESET_REQUEST, Uint8Array.of(targetId));
  }
}

function resetAllTargets() {
  curveEditors.discardAll();
  if (simulator) {
    for (const target of tuningTargets.values()) {
      for (const parameter of target.parameters) parameter.current = parameter.compiled;
    }
    renderTuning();
    notice("All simulator defaults restored.");
  } else {
    queueTuningRequest(MESSAGE.TUNING_RESET_REQUEST, Uint8Array.of(TUNING.ALL_TARGETS));
  }
}

elements["reset-all"].addEventListener("click", () => resetAllTargets());
elements["save-all"].addEventListener("click", () => {
  if (transport && savingSupported) tuningRequests.enqueue(MESSAGE.TUNING_SAVE_REQUEST, encodeTuningSave());
});

elements.devices.addEventListener("click", async (event) => {
  const preview = event.target.closest("[data-device-preview]");
  const reset = event.target.closest("[data-device-reset]");
  const save = event.target.closest("[data-device-save]");
  const clear = event.target.closest("[data-device-clear]");
  const card = event.target.closest("[data-device]");
  if (!card || (!preview && !reset && !save && !clear)) return;
  const deviceId = Number(card.dataset.device);
  const device = devices.get(deviceId);
  if (!device?.description) return;
  if (save || clear) {
    if (transport) {
      tuningRequests.enqueue(save ? MESSAGE.DEVICE_SAVE_REQUEST : MESSAGE.DEVICE_CLEAR_SAVED_REQUEST,
        save ? encodeDeviceSave(deviceId) : encodeDeviceClearSaved(deviceId));
    }
    return;
  }

  /* Device previews reuse the preview message for discards too: sending
   * the value a reboot would run restores it without a dedicated message. */
  const requested = preview
    ? Number(card.querySelector("[data-device-value]")?.value ?? device.description.currentCpi)
    : (device.description.savedCpi ?? device.description.defaultCpi);
  if (!Number.isInteger(requested)) {
    notice("Enter a whole CPI value.", true);
    return;
  }
  previewDevice(deviceId, requested);
});

function previewDevice(deviceId, cpi) {
  const device = devices.get(deviceId);
  if (simulator) {
    device.description.currentCpi = cpi;
    renderDevices();
    notice(`${device.label} simulator resolution updated.`);
  } else if (transport) {
    tuningRequests.enqueue(MESSAGE.DEVICE_PREVIEW_REQUEST, encodeDevicePreview(deviceId, cpi));
  }
}

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

const playground = new PointingPlayground(document.getElementById("playground"), {
  getContext: playgroundContext,
  onExport: (result) => {
    downloadJson(
      result,
      `zmk-pointing-playground-${result.activity.id}-${new Date().toISOString().replaceAll(":", "-")}.json`,
    );
    notice(`Exported ${result.activity.label} playground results.`);
  },
});

/* Calibration sends one atomic transaction per target, like a profile. */
function applyCalibration(target, values) {
  const updates = values.map(({ key, value }) => ({
    parameterId: target.parameters.find((parameter) => parameter.key === key).id,
    value,
  }));
  if (simulator) {
    for (const update of updates) {
      target.parameters.find(({ id }) => id === update.parameterId).current = update.value;
    }
    renderTuning();
    notice(`${target.label} simulator calibration applied.`);
  } else if (transport) {
    tuningRequests.enqueue(MESSAGE.TUNING_SET_MANY_REQUEST, encodeTuningSetMany(target.id, updates));
  }
}

calibration = new CalibrationPanel(document.getElementById("calibration"), {
  getTuningTargets: () => tuningTargets,
  getStateStatus: () => stateStatus,
  getDropped: () => stateStatus.dropped,
  canControl: () => Boolean(simulator) || (Boolean(transport) && protocolReady),
  setStateLevel,
  applyValues: applyCalibration,
  onExport: (result) => {
    downloadJson(
      result,
      `zmk-pointing-calibration-${result.calibration.mode}-${new Date().toISOString().replaceAll(":", "-")}.json`,
    );
    notice(result.targets.length
      ? "Exported the calibration. Import it as a profile to apply it again."
      : "Exported the calibration evidence; it has no tunable target to import.");
  },
});

renderLoop();

/* --- App shell: views, games, first setup ----------------------------- */

const views = [...document.querySelectorAll("[data-view]")];
const viewTabs = [...document.querySelectorAll("[data-view-tab]")];

function showView(name) {
  for (const view of views) view.hidden = view.dataset.view !== name;
  for (const tab of viewTabs) {
    if (tab.dataset.viewTab === name) tab.setAttribute("aria-current", "page");
    else tab.removeAttribute("aria-current");
  }
}

for (const tab of viewTabs) tab.addEventListener("click", () => showView(tab.dataset.viewTab));

const gameScreen = document.getElementById("game-screen");
const gameList = document.getElementById("game-list");
const setupSteps = document.getElementById("setup-steps");
const setupStart = document.querySelector("[data-setup-start]");
let currentGames = [];
let openGameId;
/* The first-setup run: game ids in order and where the user is. */
let setup;

function settingRows(settings) {
  return settings.map(({ targetId, parameterId }) => {
    const target = tuningTargets.get(targetId);
    const parameter = target?.parameters.find(({ id }) => id === parameterId);
    return parameter ? renderParameterRow(target, parameter, { inGame: true }) : "";
  }).join("");
}

function renderGames(games) {
  currentGames = games;
  setupStart.disabled = games.length === 0;
  gameList.innerHTML = games.length
    ? games.map((game) => `<article class="game-card" id="game-${game.id}">
        <h3>${escapeHtml(game.title)}</h3>
        <p>${escapeHtml(game.summary)}</p>
        <p>Drives the ${escapeHtml(game.drives)} · ${game.main.length + game.related.length} settings</p>
        <button type="button" data-open-game="${game.id}">Open</button>
      </article>`).join("")
    : `<p class="muted">${tuningTargets.size
      ? "None of this firmware's stages has a game yet, or their names are still loading."
      : "Connect current firmware, or use the simulator, to see the games your keyboard can play."}</p>`;
  renderSetupSteps();
  renderGameScreen();
  renderCapabilityChecks();
}

function renderSetupSteps() {
  setupSteps.hidden = !setup;
  if (!setup) return;
  setupSteps.innerHTML = setup.ids.map((id, index) => {
    const game = currentGames.find((item) => item.id === id);
    const state = index < setup.index ? "done" : index === setup.index ? "current" : "";
    return `<li class="${state}">${escapeHtml(game?.title ?? id)}</li>`;
  }).join("");
}

function renderGameScreen() {
  const game = currentGames.find(({ id }) => id === openGameId);
  gameScreen.hidden = !game;
  if (!game) return;
  gameScreen.querySelector("[data-game-drives]").textContent = `Drives the ${game.drives}`;
  gameScreen.querySelector("[data-game-title]").textContent = game.title;
  gameScreen.querySelector("[data-game-summary]").textContent = game.summary;
  gameScreen.querySelector("[data-game-settings]").innerHTML = settingRows(game.main)
    || '<p class="muted">This game\'s main settings are still loading.</p>';
  gameScreen.querySelector("[data-game-related]").innerHTML = settingRows(game.related);
  const unsaved = [...game.main, ...game.related].some(({ targetId, parameterId }) => {
    const parameter = tuningTargets.get(targetId)?.parameters.find(({ id }) => id === parameterId);
    return parameter && parameter.current !== bootValue(parameter);
  });
  /* While a blind comparison runs, nothing on screen may show which side
   * is in effect. */
  const hiding = Boolean(blindSession);
  gameScreen.querySelector("[data-game-settings]").hidden = hiding;
  gameScreen.querySelector("[data-game-related-wrap]").hidden = hiding || game.related.length === 0;
  const blind = ["ring", "flicks"].includes(game.runner) && !hiding;
  gameScreen.querySelector("[data-game-blind]").hidden = !blind;
  gameScreen.querySelector("[data-game-blind-hint]").hidden = !blind;
  renderSweepForm(game, blind);
  const save = gameScreen.querySelector("[data-game-save]");
  save.hidden = !savingSupported || hiding;
  save.disabled = !unsaved;
  renderGameResults(game);
  const next = gameScreen.querySelector("[data-setup-next]");
  next.hidden = !setup || setup.ids[setup.index] !== game.id;
  next.textContent = setup && setup.index === setup.ids.length - 1 ? "Finish setup" : "Next game";
}

function openGame(id) {
  if (!currentGames.some((game) => game.id === id)) return;
  if (openGameId !== id) {
    stopGames();
    gameScreen.querySelector("[data-game-play-area]").hidden = true;
    gameScreen.querySelector("[data-game-comparison]").replaceChildren();
    comparePicks.clear();
  }
  openGameId = id;
  showView("games");
  renderGameScreen();
  gameScreen.scrollIntoView({ block: "start" });
}

gameList.addEventListener("click", (event) => {
  const open = event.target.closest("[data-open-game]");
  if (open) openGame(open.dataset.openGame);
});

setupStart.addEventListener("click", () => {
  setup = { ids: currentGames.map(({ id }) => id), index: 0 };
  renderSetupSteps();
  openGame(setup.ids[0]);
});

gameScreen.addEventListener("click", (event) => {
  const game = currentGames.find(({ id }) => id === openGameId);
  if (!game) return;
  const preview = event.target.closest("[data-preview]");
  const settingLink = event.target.closest("[data-show-setting]");
  if (preview) {
    previewParameterRow(preview.closest("[data-target][data-parameter]"));
  } else if (settingLink) {
    event.preventDefault();
    showView("settings");
    document.getElementById(`setting-${settingLink.dataset.showSetting}`)?.scrollIntoView({ block: "center" });
  } else if (event.target.closest("[data-game-play]")) {
    playGame(game);
  } else if (event.target.closest("[data-game-sweep]")) {
    startSweep(game);
  } else if (event.target.closest("[data-game-blind]")) {
    startBlindComparison(game);
  } else if (event.target.closest("[data-preference]")) {
    finishBlindComparison(event.target.closest("[data-preference]").dataset.preference);
  } else if (event.target.closest("[data-game-copy]")) {
    copyLatestResults(game);
  } else if (event.target.closest("[data-game-compare]")) {
    renderComparison(game);
  } else if (event.target.closest("[data-game-clear]")) {
    clearHistory(game.id);
    comparePicks.clear();
    gameScreen.querySelector("[data-game-comparison]").replaceChildren();
    renderGameResults(game);
  } else if (event.target.closest("[data-game-save]")) {
    /* Save every stage this game's settings live on. */
    const targetIds = new Set([...game.main, ...game.related].map(({ targetId }) => targetId));
    for (const targetId of targetIds) queueTuningRequest(MESSAGE.TUNING_SAVE_REQUEST, Uint8Array.of(targetId));
  } else if (event.target.closest("[data-setup-next]")) {
    setup.index += 1;
    if (setup.index >= setup.ids.length) {
      setup = undefined;
      openGameId = undefined;
      renderSetupSteps();
      renderGameScreen();
      notice(savingSupported
        ? "First setup finished. Anything you saved now survives a reboot."
        : "First setup finished. This build can't save, so copy the config from Settings to keep it.");
      return;
    }
    renderSetupSteps();
    openGame(setup.ids[setup.index]);
  } else if (event.target.closest("[data-game-close]")) {
    stopGames();
    gameScreen.querySelector("[data-game-play-area]").hidden = true;
    openGameId = undefined;
    renderGameScreen();
  }
});

/* What the connected firmware offers, as plain checks a person or an agent
 * reading the page can follow. */
function renderCapabilityChecks() {
  const list = document.getElementById("capability-checks");
  if (!protocolReady && !simulator) {
    list.innerHTML = '<li class="muted">Not connected.</li>';
    return;
  }
  const checks = [
    [true, transport ? `Protocol v${PROTOCOL_VERSION} connected.` : "Simulator running; no keyboard is connected."],
    [tuningTargets.size > 0, `${tuningTargets.size} tunable stages reported.`],
    [devices.size > 0, `${devices.size} pointing devices reported.`],
    [currentGames.length > 0, `${currentGames.length} games available.`],
    [savingSupported, savingSupported ? "Saving on the device is available." : "This build can't save on the device; previews last until reboot."],
  ];
  list.innerHTML = checks
    .map(([ok, text]) => `<li class="${ok ? "" : "missing"}">${ok ? "OK" : "Missing"}: ${escapeHtml(text)}</li>`)
    .join("");
}


/* --- Playing and recording games -------------------------------------- */

const comparePicks = new Set();
const gameProgress = gameScreen.querySelector("[data-game-progress]");
const gameSurface = gameScreen.querySelector("[data-game-surface]");
const runnerEvents = {
  onProgress: (text) => { gameProgress.textContent = text; },
  onFinish: (run) => {
    saveRun(run);
    gameProgress.textContent = Number.isFinite(run.throughput)
      ? `Done. Throughput ${run.throughput.toFixed(2)} bit/s; details below.`
      : `Done. ${run.notes?.find((note) => !note.startsWith("Played against")) ?? "Details below."}`;
    const game = currentGames.find(({ id }) => id === run.game);
    if (game) renderGameResults(game);
  },
};
const gameRunner = new TargetGameRunner(gameSurface, runnerEvents);
const compassRunner = new CompassRunner(gameSurface, runnerEvents);
const pixelRunner = new PixelRunner(gameSurface, runnerEvents);
const pursuitRunner = new PursuitRunner(gameSurface, runnerEvents);
const stillRunner = new StillRunner(gameSurface, runnerEvents);
const scrollBandRunner = new ScrollBandRunner(gameSurface, runnerEvents);
const railRunner = new RailRunner(gameSurface, runnerEvents);

function stopGames() {
  gameRunner.stop();
  compassRunner.stop();
  pixelRunner.stop();
  pursuitRunner.stop();
  stillRunner.stop();
  scrollBandRunner.stop();
  railRunner.stop();
  if (blindSession) {
    const { restore } = blindSession;
    blindSession = undefined;
    applyValues(restore);
    renderGameScreen();
  }
}

/* The values this game's settings had while it was played. */
function settingsSnapshot(game) {
  return [...game.main, ...game.related].flatMap(({ targetId, parameterId }) => {
    const target = tuningTargets.get(targetId);
    const parameter = target?.parameters.find(({ id }) => id === parameterId);
    return parameter
      ? [{ target: target.label, key: parameter.key, label: parameter.label, value: parameter.current, unit: parameter.unit }]
      : [];
  });
}

function flickSequences(size, trials, prefix = "") {
  return trials.map((trial, index) => ({
    label: `${prefix}Flick ${index + 1} of ${trials.length}`,
    targets: [trial.home, { ...trial.target, group: trial.band }],
  }));
}

/* The orientation the compass game was played with, from its settings. */
function currentOrientation(game) {
  const value = (key) => {
    for (const { targetId, parameterId } of game.main) {
      const parameter = tuningTargets.get(targetId)?.parameters.find(({ id }) => id === parameterId);
      if (parameter?.key === key) return parameter.current !== 0;
    }
    return undefined;
  };
  const orientation = { swapXY: value("swap-xy"), invertX: value("invert-x"), invertY: value("invert-y") };
  return Object.values(orientation).every((item) => item !== undefined) ? orientation : undefined;
}

function playGame(game) {
  if (!["ring", "flicks", "compass", "pixel", "pursuit", "still", "scroll-band", "rail"].includes(game.runner)) {
    playground.openActivity(game.activity);
    return;
  }
  stopGames();
  const area = gameScreen.querySelector("[data-game-play-area]");
  area.hidden = false;
  const size = { width: gameSurface.clientWidth, height: gameSurface.clientHeight };
  const centre = { x: size.width / 2, y: size.height / 2 };
  const run = {
    game: game.id,
    title: game.title,
    startedAt: new Date().toISOString(),
    settings: settingsSnapshot(game),
    notes: simulator ? ["Played against the simulator."] : [],
  };
  /* Fixed seeds: every run plays the same layout, so runs compare. */
  if (game.runner === "ring") {
    const sequences = ringLevels(size).map((level, index) => ({
      label: `Ring ${index + 1} of 3 (${level.id.toFixed(1)} bits)`,
      targets: ringSequence({ amplitude: level.amplitude, width: level.width, centre }),
    }));
    gameRunner.start(sequences, run);
  } else if (game.runner === "flicks") {
    gameRunner.start(flickSequences(size, flickTrials(size, { random: seededRandom(7) })), run, (recorded) => {
      const result = analyseFlicks(recorded);
      return { ...result, notes: [...run.notes, ...result.notes] };
    });
  } else if (game.runner === "rail") {
    railRunner.start(RAIL_COURSES, run, (runs) => {
      const result = analyseRail(runs);
      return { ...result, notes: [...run.notes, ...formatRail(result.rail), ...result.notes] };
    }, { width: RAIL_WIDTH, timeout: RAIL_TIMEOUT_MS, finished: railFinished });
  } else if (game.runner === "scroll-band") {
    scrollBandRunner.start(scrollTrials({ random: seededRandom(9) }), run, (trials) => {
      const result = analyseScrollBand(trials);
      return { ...result, notes: [...run.notes, ...formatScrollBand(result.scrollBand), ...result.notes] };
    }, { hold: BAND_HOLD_MS, timeout: SCROLL_TIMEOUT_MS });
  } else if (game.runner === "still") {
    /* Targets alternate either side of the centre, so every click is
     * followed by a real move to the next one. */
    const reach = Math.min(size.width, size.height) * 0.25;
    const targets = Array.from({ length: 12 }, (_, i) => ({
      x: centre.x + (i % 2 ? reach : -reach),
      y: centre.y + ((i % 4) < 2 ? -reach / 2 : reach / 2),
      width: 28,
    }));
    stillRunner.start({ targets, home: { ...centre, width: 28 } }, run, (clicks, rest) => {
      const result = analyseStillHands(clicks, rest);
      return { ...result, notes: [...run.notes, ...formatStillHands(result.stillHands), ...result.notes] };
    });
  } else if (game.runner === "pursuit") {
    const sweep = { centre, amplitude: Math.min(size.width, size.height) * 0.35 };
    pursuitRunner.start(PURSUIT_PHASES, run, (recorded) => {
      const result = analysePursuit(recorded.map(({ phase, samples }) => measurePursuitPhase(phase, samples, sweep)));
      return { ...result, notes: [...run.notes, ...formatPursuit(result.pursuit), ...result.notes] };
    }, { position: (phase, elapsed) => pursuitPosition(phase, elapsed, sweep) });
  } else if (game.runner === "pixel") {
    pixelRunner.start(pixelTrials(size, { random: seededRandom(5) }), run, (trials) => {
      const result = analysePixel(trials);
      return { ...result, notes: [...run.notes, ...formatPixel(result.pixel), ...result.notes] };
    }, { hold: HOLD_MS, timeout: PIXEL_TIMEOUT_MS, onTarget });
  } else {
    const random = seededRandom(3);
    const cues = [...cueDirections(8), ...cueDirections(8)]
      .map((cue) => ({ cue, order: random() }))
      .sort((a, b) => a.order - b.order)
      .map(({ cue }) => cue);
    const current = currentOrientation(game);
    compassRunner.start(
      { home: { ...centre, width: 28 }, radius: Math.min(size.width, size.height) * 0.3, cues },
      run,
      (trials) => {
        const result = analyseCompass(trials, current);
        return { ...result, notes: [...run.notes, ...result.notes] };
      },
    );
  }
  gameSurface.scrollIntoView({ block: "center" });
}

/* --- Blind comparison (#145) ----------------------------------------- */

let blindSession;

/* Sets values without the per-preview notice, for switching sides. */
function applyValues(values) {
  for (const { targetId, parameterId, value } of values) {
    const parameter = tuningTargets.get(targetId)?.parameters.find(({ id }) => id === parameterId);
    if (!parameter || parameter.current === value) continue;
    if (simulator) parameter.current = value;
    else if (transport) tuningRequests.enqueue(MESSAGE.TUNING_SET_REQUEST, encodeTuningSet(targetId, parameterId, value));
  }
  if (simulator) renderTuning();
}

/* Side A is the values in effect now; side B is whatever was typed into
 * this game's main settings without previewing. */
function blindValues(game) {
  const A = [];
  const B = [];
  for (const row of gameScreen.querySelectorAll("[data-game-settings] [data-target][data-parameter]")) {
    const targetId = Number(row.dataset.target);
    const parameterId = Number(row.dataset.parameter);
    const parameter = tuningTargets.get(targetId)?.parameters.find(({ id }) => id === parameterId);
    const input = row.querySelector("[data-value]");
    if (!parameter || !input) continue;
    const value = parameter.type === TUNING.BOOLEAN ? Number(input.checked) : Number(input.value);
    if (!Number.isInteger(value) || value < parameter.minimum || value > parameter.maximum || value === parameter.current) continue;
    A.push({ targetId, parameterId, value: parameter.current, name: parameter.key ?? parameter.label });
    B.push({ targetId, parameterId, value, name: parameter.key ?? parameter.label });
  }
  return game.main.length ? { A, B } : { A: [], B: [] };
}

/* One block's targets: the middle ring, or nine flicks in a layout that
 * changes per block so neither side gets a memorised order. */
function blindBlockSequences(game, size, index, prefix) {
  if (game.runner === "ring") {
    const level = ringLevels(size)[1];
    return [{
      label: `${prefix}Ring (${level.id.toFixed(1)} bits)`,
      targets: ringSequence({ amplitude: level.amplitude, width: level.width, centre: { x: size.width / 2, y: size.height / 2 } }),
    }];
  }
  return flickSequences(size, flickTrials(size, { perBand: 3, random: seededRandom(20 + index) }), prefix);
}

function startBlindComparison(game) {
  const values = blindValues(game);
  if (!values.B.length) {
    notice("Type a different value into at least one setting above, without previewing it, to compare against.", true);
    return;
  }
  stopGames();
  const plan = planBlocks();
  blindSession = {
    game, values, order: plan.order, planned: plan.blocks, labels: hiddenLabels(), index: 0, results: [],
    startedAt: new Date().toISOString(), settings: settingsSnapshot(game), restore: values.A, onDone: askPreference,
  };
  gameScreen.querySelector("[data-game-play-area]").hidden = false;
  gameScreen.querySelector("[data-game-comparison]").replaceChildren();
  renderGameScreen();
  runBlindBlock();
}

function runBlindBlock() {
  const session = blindSession;
  const planned = session.planned[session.index];
  applyValues(session.values[planned.condition]);
  const label = session.labels[planned.condition];
  const kind = planned.scored ? "scored" : `${planned.kind}, not scored`;
  const prefix = `Block ${session.index + 1} of ${session.planned.length} · setting ${label} (${kind}) · `;
  const size = { width: gameSurface.clientWidth, height: gameSurface.clientHeight };
  const analyse = session.game.runner === "flicks" ? analyseFlicks : undefined;
  gameRunner.start(blindBlockSequences(session.game, size, session.index, prefix), {}, analyse, (result) => {
    session.results.push({
      condition: planned.condition,
      scored: planned.scored,
      measured: result.trials.flat(),
      throughput: result.throughput,
    });
    session.index += 1;
    if (session.index < session.planned.length) {
      runBlindBlock();
      return;
    }
    session.onDone();
  });
  gameSurface.scrollIntoView({ block: "center" });
}

function askPreference() {
  gameProgress.textContent = "All blocks done. Which felt better?";
  gameScreen.querySelector("[data-game-results-wrap]").hidden = false;
  gameScreen.querySelector("[data-game-comparison]").innerHTML = `<div class="game-comparison">
    <p>Which setting felt better to use? Answer before seeing the numbers.</p>
    <div class="panel-actions">
      <button type="button" data-preference="X">X</button>
      <button type="button" data-preference="Y">Y</button>
      <button type="button" data-preference="same">No difference</button>
    </div>
  </div>`;
}

function finishBlindComparison(preference) {
  const session = blindSession;
  if (!session || session.index < session.planned.length) return;
  blindSession = undefined;
  applyValues(session.restore);
  renderGameScreen();
  const result = compareConditions(session.results);
  const describe = (values) => values.map(({ name, value }) => `${name} ${value}`).join(", ");
  const lines = [
    `Blind comparison, order ${session.order}: A (${session.labels.A}) = current values, ${describe(session.values.A)}; B (${session.labels.B}) = ${describe(session.values.B)}.`,
    describePreference(preference, session.labels, result),
    ...describeComparison(result),
    ...(session.game.runner === "flicks" ? ["A", "B"].flatMap((side) => {
      const byBand = Object.fromEntries(summarizeBy(result.measured[side], (item) => item.group));
      return flickFlags(byBand, result.measured[side][0]?.width ?? 16).map((flag) => `${side}: ${flag}`);
    }) : []),
    "One session is a small sample; repeat on another day before deciding. The current values are back in effect.",
  ];
  const run = {
    game: session.game.id,
    title: `${session.game.title} · blind comparison`,
    startedAt: session.startedAt,
    finishedAt: new Date().toISOString(),
    groups: { Setting: { A: result.A.summary, B: result.B.summary } },
    comparison: { order: session.order, labels: session.labels, preference, values: session.values },
    notes: [...(simulator ? ["Played against the simulator."] : []), ...lines],
    settings: session.settings,
  };
  saveRun(run);
  gameProgress.textContent = "Done. Results below.";
  gameScreen.querySelector("[data-game-comparison]").innerHTML = `<div class="game-comparison">
    <h3>X was ${session.labels.A === "X" ? "A, the current values" : "B, the typed values"}; Y was ${session.labels.A === "Y" ? "A, the current values" : "B, the typed values"}.</h3>
    ${lines.slice(1).map((line) => `<p>${escapeHtml(line)}</p>`).join("")}
    <pre>${escapeHtml(formatRun(run))}</pre>
  </div>`;
  renderGameResults(session.game);
}

/* --- Parameter sweep (#145) ------------------------------------------ */

function sweepParameters(game) {
  return [...game.main, ...game.related].flatMap(({ targetId, parameterId }) => {
    const target = tuningTargets.get(targetId);
    const parameter = target?.parameters.find(({ id }) => id === parameterId);
    return parameter && parameter.type !== TUNING.BOOLEAN ? [{ target, parameter }] : [];
  });
}

/* Keeps the form's choices while the screen re-renders. */
function renderSweepForm(game, shown) {
  const wrap = gameScreen.querySelector("[data-game-sweep-wrap]");
  const options = sweepParameters(game);
  wrap.hidden = !shown || options.length === 0;
  if (wrap.hidden) return;
  const select = wrap.querySelector("[data-sweep-setting]");
  const html = options.map(({ target, parameter }) => `<option value="${target.id}:${parameter.id}">${escapeHtml(`${parameter.label ?? parameter.key} (${target.label})`)}</option>`).join("");
  const key = `${game.id}|${options.map(({ target, parameter }) => `${target.id}:${parameter.id}`).join(",")}`;
  if (select.dataset.options !== key) {
    const previous = select.dataset.game === game.id ? select.value : undefined;
    select.innerHTML = html;
    select.dataset.game = game.id;
    select.dataset.options = key;
    if (previous && options.some(({ target, parameter }) => `${target.id}:${parameter.id}` === previous)) select.value = previous;
    else fillSweepRange(game);
  }
}

/* A starting range around the current value: half to double it, inside
 * the setting's limits. */
function fillSweepRange(game) {
  const wrap = gameScreen.querySelector("[data-game-sweep-wrap]");
  const chosen = selectedSweepParameter(game);
  if (!chosen) return;
  const { parameter } = chosen;
  const current = parameter.current;
  wrap.querySelector("[data-sweep-from]").value = Math.max(parameter.minimum, Math.round(current / 2));
  wrap.querySelector("[data-sweep-to]").value = Math.min(parameter.maximum, Math.max(current * 2, current + 2));
}

function selectedSweepParameter(game) {
  const [targetId, parameterId] = gameScreen.querySelector("[data-sweep-setting]").value.split(":").map(Number);
  return sweepParameters(game).find(({ target, parameter }) => target.id === targetId && parameter.id === parameterId);
}

gameScreen.querySelector("[data-sweep-setting]").addEventListener("change", () => {
  const game = currentGames.find(({ id }) => id === openGameId);
  if (game) fillSweepRange(game);
});

function startSweep(game) {
  const chosen = selectedSweepParameter(game);
  const wrap = gameScreen.querySelector("[data-game-sweep-wrap]");
  if (!chosen) return;
  const { target, parameter } = chosen;
  const values = sweepValues(
    Number(wrap.querySelector("[data-sweep-from]").value),
    Number(wrap.querySelector("[data-sweep-to]").value),
    Number(wrap.querySelector("[data-sweep-count]").value),
    parameter,
  );
  if (values.length < 3 || values.some((value) => !Number.isFinite(value))) {
    notice("Pick a range wide enough for at least 3 different whole values.", true);
    return;
  }
  stopGames();
  const plan = planSweep(values.length);
  const setting = (value) => [{ targetId: target.id, parameterId: parameter.id, value, name: parameter.key ?? parameter.label }];
  blindSession = {
    game,
    sweep: { values, name: parameter.key ?? parameter.label },
    values: Object.fromEntries(values.map((value, key) => [key, setting(value)])),
    order: plan.order.map((key) => plan.labels[key]).join(""),
    planned: plan.blocks,
    labels: plan.labels,
    index: 0,
    results: [],
    startedAt: new Date().toISOString(),
    settings: settingsSnapshot(game),
    restore: setting(parameter.current),
    onDone: finishSweep,
  };
  gameScreen.querySelector("[data-game-play-area]").hidden = false;
  gameScreen.querySelector("[data-game-comparison]").replaceChildren();
  renderGameScreen();
  runBlindBlock();
}

function finishSweep() {
  const session = blindSession;
  blindSession = undefined;
  applyValues(session.restore);
  renderGameScreen();
  const rows = summarizeSweep(session.results, session.sweep.values);
  const lines = describeSweep(session.sweep.name, rows);
  const run = {
    game: session.game.id,
    title: `${session.game.title} · sweep of ${session.sweep.name}`,
    startedAt: session.startedAt,
    finishedAt: new Date().toISOString(),
    sweep: { name: session.sweep.name, order: session.order, labels: session.labels, rows },
    notes: [...(simulator ? ["Played against the simulator."] : []), ...lines,
      `The value in effect before the sweep, ${session.restore[0].value}, is back in effect.`],
    settings: session.settings,
  };
  saveRun(run);
  gameProgress.textContent = "Sweep done. Results below.";
  gameScreen.querySelector("[data-game-results-wrap]").hidden = false;
  const charts = SWEEP_MEASURES.map((measure) => {
    const chart = sweepChart(rows, measure);
    return chart ? `<figure><figcaption>${escapeHtml(measure.label)}${measure.unit ? ` (${escapeHtml(measure.unit)})` : ""}</figcaption>${chart}</figure>` : "";
  }).join("");
  gameScreen.querySelector("[data-game-comparison]").innerHTML = `<div class="game-comparison">
    <h3>Sweep of ${escapeHtml(session.sweep.name)}</h3>
    <div class="sweep-charts">${charts}</div>
    <pre>${escapeHtml(formatRun(run))}</pre>
  </div>`;
  renderGameResults(session.game);
}

function renderGameResults(game) {
  const wrap = gameScreen.querySelector("[data-game-results-wrap]");
  const runs = loadHistory(game.id);
  wrap.hidden = runs.length === 0;
  const list = gameScreen.querySelector("[data-game-history]");
  list.innerHTML = runs.map((run, index) => ({ run, index })).reverse().map(({ run, index }) => `<li>
      <label><input type="checkbox" data-compare-pick="${index}" ${comparePicks.has(index) ? "checked" : ""}>
        ${escapeHtml(new Date(run.finishedAt).toLocaleString())}${Number.isFinite(run.throughput) ? ` · ${run.throughput.toFixed(2)} bit/s` : ""}</label>
      <details><summary>Details</summary><pre>${escapeHtml(formatRun(run))}</pre></details>
    </li>`).join("");
  gameScreen.querySelector("[data-game-compare]").disabled = comparePicks.size !== 2;
}

gameScreen.addEventListener("change", (event) => {
  const pick = event.target.closest("[data-compare-pick]");
  if (!pick) return;
  const index = Number(pick.dataset.comparePick);
  if (pick.checked) comparePicks.add(index);
  else comparePicks.delete(index);
  gameScreen.querySelector("[data-game-compare]").disabled = comparePicks.size !== 2;
});

async function copyLatestResults(game) {
  const latest = loadHistory(game.id).at(-1);
  if (!latest) return;
  try {
    await navigator.clipboard.writeText(formatRun(latest));
    notice("Copied the latest results.");
  } catch {
    notice("The browser blocked copying; open Details and copy the text instead.", true);
  }
}

/* Two runs side by side with the settings that differed. A fair verdict
 * needs the blind comparison mode (#145); this shows what was measured. */
function renderComparison(game) {
  const runs = loadHistory(game.id);
  const [a, b] = [...comparePicks].sort((x, y) => x - y).map((index) => runs[index]);
  if (!a || !b) return;
  const rows = [
    ["Throughput (bit/s)", (run) => run.throughput, 2],
    ["Hit rate (%)", (run) => run.summary?.hitRate * 100, 0],
    ["Median time (ms)", (run) => run.summary?.movementTime, 0],
    ["Median endpoint (px, + past)", (run) => run.summary?.signedError, 1],
    ["Re-entries per target", (run) => run.summary?.reEntries, 2],
    ["Median overshoot (%)", (run) => run.summary?.overshoot * 100, 1],
    ["Re-rolls per target", (run) => run.summary?.rerolls, 2],
  ];
  const cell = (value, digits) => (Number.isFinite(value) ? value.toFixed(digits) : "n/a");
  const settingKey = (setting) => `${setting.target} / ${setting.key ?? setting.label}`;
  const bValues = new Map(b.settings.map((setting) => [settingKey(setting), setting.value]));
  const changed = a.settings.filter((setting) => bValues.get(settingKey(setting)) !== setting.value);
  gameScreen.querySelector("[data-game-comparison]").innerHTML = `<div class="game-comparison">
    <table>
      <thead><tr><th>Measure</th><th>A · ${escapeHtml(new Date(a.finishedAt).toLocaleString())}</th><th>B · ${escapeHtml(new Date(b.finishedAt).toLocaleString())}</th></tr></thead>
      <tbody>${rows.map(([label, value, digits]) => `<tr><td>${label}</td><td>${cell(value(a), digits)}</td><td>${cell(value(b), digits)}</td></tr>`).join("")}
      ${changed.map((setting) => `<tr><td>${escapeHtml(settingKey(setting))}</td><td>${setting.value}</td><td>${bValues.get(settingKey(setting)) ?? "n/a"}</td></tr>`).join("")}</tbody>
    </table>
    <p class="muted">${changed.length ? "" : "No setting differed between these runs. "}One run each is a rough guide: practice and warm-up move these numbers too.</p>
  </div>`;
}

renderTuning();
