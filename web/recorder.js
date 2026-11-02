/* SPDX-License-Identifier: MIT */

/* Record and score (#124): records real motion from one pipeline through
 * firmware input capture (docs/protocol.md, "Input capture"), keeps the
 * recordings in this browser, and scores the current values against
 * candidate settings on them with the browser replay module (replay/,
 * docs/trace-replay.md). The replay module loads only when something is
 * scored.
 *
 * Capture requests don't use the tuning request queue: firmware built
 * without capture ignores them, and the queue ends the session when a
 * request goes unanswered. They run one at a time here, each with its own
 * timeout, and a status query nobody answers marks capture unsupported
 * until Record is pressed or an agent asks for a recording, which ask
 * again. */

import { CAPTURE, MESSAGE, captureDecoder, captureSettingsHash, encodeFrame, parseCaptureBatch,
  parseCaptureDescription, parseCaptureStatus } from "./protocol.js";
import { ACTIVITIES, GUIDED_TESTS } from "./playground-model.js";
import { FREE_TRUTH, SCORE_CAVEAT, TRUTH_SOURCES, addRecords, compareScoreSets, createRecordingStats, describeTruth,
  deviceGuide, exportRecording, importRecording, metricTruth, newRecordingId, pipelineMode, resyncs } from "./recording-model.js";

const STATUS_TIMEOUT_MS = 2_500;
/* Stopping sends everything buffered first; a large buffer on a slow link
 * takes seconds. */
const STOP_TIMEOUT_MS = 15_000;
const POLL_MS = 1_000;
const DB_NAME = "zmk-pointing-tools";
const DB_STORE = "recordings";
const RECORDING_LIMIT = 30;
/* Browser pointer samples closer together than this are thinned. */
const POINTER_INTERVAL_MS = 2;
const REPLAY_MISSING = "Scoring needs the browser replay module (replay/ next to the tuner). The hosted tuner has it; for a local copy, run host/wasm/build.sh web/replay in the devenv shell.";

function escapeHtml(value) {
  return String(value).replaceAll("&", "&amp;").replaceAll("<", "&lt;").replaceAll(">", "&gt;")
    .replaceAll('"', "&quot;");
}

const seconds = (ms) => `${(ms / 1000).toFixed(1)} s`;

/* IndexedDB, best effort: without it recordings last until the page
 * closes. */
function database() {
  return new Promise((resolve, reject) => {
    const request = indexedDB.open(DB_NAME, 1);
    request.onupgradeneeded = () => request.result.createObjectStore(DB_STORE, { keyPath: "id" });
    request.onsuccess = () => resolve(request.result);
    request.onerror = () => reject(request.error);
  });
}

async function store(mode, action) {
  const db = await database();
  try {
    return await new Promise((resolve, reject) => {
      const transaction = db.transaction(DB_STORE, mode);
      const request = action(transaction.objectStore(DB_STORE));
      transaction.oncomplete = () => resolve(request.result);
      transaction.onerror = () => reject(transaction.error);
    });
  } finally {
    db.close();
  }
}

function concat(chunks) {
  const out = new Uint8Array(chunks.reduce((total, chunk) => total + chunk.length, 0));
  let offset = 0;
  for (const chunk of chunks) {
    out.set(chunk, offset);
    offset += chunk.length;
  }
  return out;
}

export class Recorder {
  #chain = Promise.resolve();
  #waiting;
  #replay;
  #scorer;
  #checking;
  #modes = new Map();
  #roundTrip;

  /* host: send(type, pipelineId) sends a capture request, profile() reads
   * the current values as a tuning profile, ready() says whether that can
   * happen yet, transport() names the link, notice(text, error), and the
   * hooks onStart(pipeline) and onEnd(pipeline, outcome) for agent runs,
   * candidates() for snapshots to score, and onChange(). */
  constructor(root, host) {
    this.root = root;
    this.host = host;
    this.recordings = [];
    this.importedProfiles = [];
    this.status = null;
    this.supported = null;
    this.active = null;
    this.result = null;
    this.selectPipeline = root.querySelector("[data-recording-pipeline]");
    this.freePanel = root.querySelector("[data-recording-free]");
    this.label = root.querySelector("[data-recording-label]");
    this.startButton = root.querySelector("[data-recording-start]");
    this.stopButton = root.querySelector("[data-recording-stop]");
    this.statusText = root.querySelector("[data-recording-status]");
    this.live = root.querySelector("[data-recording-live]");
    this.announcer = root.querySelector("[data-recording-announce]");
    this.list = root.querySelector("[data-recording-list]");
    this.candidateList = root.querySelector("[data-score-candidates]");
    this.scoreButton = root.querySelector("[data-score]");
    this.results = root.querySelector("[data-score-results]");
    this.recordingFile = root.querySelector("[data-recording-file]");
    this.profileFile = root.querySelector("[data-score-profile-file]");
    this.startButton.addEventListener("click", () => this.#startFromPanel());
    this.stopButton.addEventListener("click", () => this.stop("stopped by you"));
    root.querySelector("[data-recording-import]").addEventListener("click", () => this.recordingFile.click());
    root.querySelector("[data-score-profile-import]").addEventListener("click", () => this.profileFile.click());
    this.recordingFile.addEventListener("change", () => this.#importFile(this.recordingFile, (data) => this.add(importRecording(data))));
    this.profileFile.addEventListener("change", () => this.#importFile(this.profileFile, (data, name) => {
      if (data?.schema !== "zmk-pointing-tools/tuning-profile" || !Array.isArray(data.targets)) {
        throw new Error("expected a tuning profile export");
      }
      this.importedProfiles.push({ name: name.replace(/\.json$/, ""), profile: data });
      this.render();
    }));
    this.list.addEventListener("click", (event) => {
      const id = event.target.closest("[data-recording-id]")?.dataset.recordingId;
      if (!id) return;
      const recording = this.recordings.find((candidate) => candidate.id === id);
      if (event.target.closest("[data-recording-delete]") && recording
        && confirm(`Delete the recording "${recording.label}"? It can't be recovered unless you exported it.`)) {
        this.remove(id);
      }
      if (event.target.closest("[data-recording-export]")) this.#download(id);
    });
    this.scoreButton.addEventListener("click", () => this.#scoreFromPanel());
    this.load();
    this.render();
  }

  async load() {
    try {
      const saved = await store("readonly", (objects) => objects.getAll());
      this.recordings = saved.sort((a, b) => String(a.createdAt ?? "").localeCompare(String(b.createdAt ?? "")));
    } catch {
      this.persistent = false;
    }
    this.render();
  }

  async #importFile(input, apply) {
    const [file] = input.files;
    input.value = "";
    if (!file) return;
    try {
      apply(JSON.parse(await file.text()), file.name);
    } catch (error) {
      this.host.notice(`Import failed: ${error.message}`, true);
    }
  }

  #download(id) {
    const recording = this.find(id);
    const link = document.createElement("a");
    link.href = URL.createObjectURL(new Blob([JSON.stringify(exportRecording(recording))], { type: "application/json" }));
    link.download = `zmk-pointing-recording-${recording.label.replace(/[^\w-]+/g, "-")}-${recording.createdAt.replaceAll(":", "-")}.json`;
    link.click();
    URL.revokeObjectURL(link.href);
  }

  find(id) {
    const recording = this.recordings.find((candidate) => candidate.id === id);
    if (!recording) throw new Error(`No recording "${id}". list_recordings has their ids.`);
    return recording;
  }

  async add(recording) {
    this.recordings.push(recording);
    while (this.recordings.length > RECORDING_LIMIT) {
      const [oldest] = this.recordings;
      this.host.notice(`The tuner keeps ${RECORDING_LIMIT} recordings, so it deleted the oldest, "${oldest.label}". Export recordings to keep more.`, true);
      await this.remove(oldest.id);
    }
    try {
      await store("readwrite", (objects) => objects.put(recording));
    } catch {
      this.host.notice("This browser didn't keep the recording; it lasts until the page closes. Export it to keep it.", true);
    }
    this.render();
  }

  async remove(id) {
    this.recordings = this.recordings.filter((recording) => recording.id !== id);
    try {
      await store("readwrite", (objects) => objects.delete(id));
    } catch {}
    this.render();
  }

  /* --- Capture requests --- */

  #request(type, pipelineId, timeoutMs) {
    const run = () => new Promise((resolve, reject) => {
      /* Each settle clears this request's waiter only, never a later one. */
      const settle = (finish, value) => {
        clearTimeout(timer);
        if (this.#waiting === waiter) this.#waiting = undefined;
        finish(value);
      };
      const timer = setTimeout(() => settle(reject, new Error("the keyboard didn't answer the capture request")), timeoutMs);
      const sentAt = performance.now();
      const waiter = { type, reject: (error) => settle(reject, error), resolve: (frame) => {
        this.#roundTrip = { sentAt, receivedAt: performance.now() };
        settle(resolve, frame);
      } };
      this.#waiting = waiter;
      try {
        Promise.resolve(this.host.send(type, pipelineId)).catch(waiter.reject);
      } catch (error) {
        waiter.reject(error);
      }
    });
    const result = this.#chain.then(run, run);
    this.#chain = result.catch(() => {});
    return result;
  }

  async #queryStatus(pipelineId, timeoutMs = STATUS_TIMEOUT_MS) {
    const frame = await this.#request(MESSAGE.CAPTURE_CONTROL_REQUEST, pipelineId, timeoutMs);
    this.status = parseCaptureStatus(frame.payload);
    this.#syncClock(this.status.uptimeMs, this.#roundTrip);
    return this.status;
  }

  /* Lines this page's clock up with the device's while recording: the
   * status with the shortest round trip places its uptime halfway between
   * the request and the answer. */
  #syncClock(uptimeMs, { sentAt, receivedAt }) {
    if (!this.active) return;
    const roundTripMs = receivedAt - sentAt;
    if (this.active.clock && this.active.clock.roundTripMs <= roundTripMs) return;
    this.active.clock = { roundTripMs, offsetMs: uptimeMs - (sentAt + receivedAt) / 2 };
  }

  async #describe(pipelineId) {
    const frame = await this.#request(MESSAGE.CAPTURE_DESCRIBE_REQUEST, pipelineId, STATUS_TIMEOUT_MS);
    return { frame, description: parseCaptureDescription(frame.payload) };
  }

  /* The pipeline a guided test records: a replayable one whose stages make
   * it a pipeline of that mode, preferring the one picked in the panel, then
   * one the router runs now. Pipelines are user configuration, so they are
   * told apart by their stages, never by name. */
  async #pipelineFor(mode) {
    const matching = [];
    for (const pipeline of this.pipelines().filter(({ replayable }) => replayable)) {
      if (!this.#modes.has(pipeline.id)) {
        try {
          this.#modes.set(pipeline.id, pipelineMode((await this.#describe(pipeline.id)).description));
        } catch {
          continue;
        }
      }
      if (this.#modes.get(pipeline.id) === mode) matching.push(pipeline);
    }
    const picked = Number(this.selectPipeline.value);
    return matching.find(({ id }) => id === picked) ?? matching.find(({ active }) => active) ?? matching[0];
  }

  /* After the keyboard or simulator connects. */
  async connected() {
    this.supported = null;
    this.status = null;
    this.#modes.clear();
    this.render();
    try {
      await this.#queryStatus(undefined);
      this.supported = true;
    } catch (error) {
      this.supported = false;
      this.unsupportedReason = error.message;
    }
    this.render();
  }

  /* Asks again whether the keyboard records, after a status query went
   * unanswered: a single miss, such as on a busy Bluetooth link, mustn't
   * hide recording until the next connection. */
  async checkSupport() {
    if (this.supported === false && this.host.connected() && !this.#checking) {
      this.#checking = this.connected().finally(() => { this.#checking = undefined; });
    }
    await this.#checking;
    return this.supported;
  }

  disconnected() {
    this.#waiting?.reject(new Error("the connection ended"));
    this.supported = null;
    this.status = null;
    this.#modes.clear();
    if (this.active) this.#finish("the connection ended");
    this.render();
  }

  /* Every capture frame the tuner receives. */
  handleFrame(frame) {
    if (frame.type === MESSAGE.CAPTURE_BATCH) {
      if (!this.active) return;
      this.active.chunks.push(encodeFrame(frame.type, frame.payload));
      try {
        addRecords(this.active.stats, parseCaptureBatch(frame.payload, this.active.decoder));
      } catch (error) {
        this.active.warnings.push(error.message);
      }
      this.renderLive();
      return;
    }
    if (frame.type === MESSAGE.CAPTURE_STATUS_RESPONSE || frame.type === MESSAGE.CAPTURE_DESCRIPTION_RESPONSE) {
      if (this.active && frame.type === MESSAGE.CAPTURE_STATUS_RESPONSE) {
        this.active.chunks.push(encodeFrame(frame.type, frame.payload));
      }
      const expected = this.#waiting?.type === MESSAGE.CAPTURE_CONTROL_REQUEST
        ? MESSAGE.CAPTURE_STATUS_RESPONSE : MESSAGE.CAPTURE_DESCRIPTION_RESPONSE;
      if (this.#waiting && frame.type === expected) this.#waiting.resolve(frame);
    }
  }

  /* A tuning result answering a capture describe request: the pipeline is
   * unknown. */
  handleRejection() {
    if (this.#waiting?.type === MESSAGE.CAPTURE_DESCRIBE_REQUEST) {
      this.#waiting.reject(new Error("the keyboard doesn't know that pipeline"));
    }
  }

  /* Sets the panel up for an agent's accepted recording request, for the
   * user to start. */
  async prepare({ activity, label }) {
    await this.checkSupport();
    const pipeline = this.pipelines().find((candidate) => candidate.label === activity);
    if (pipeline) this.selectPipeline.value = String(pipeline.id);
    this.label.value = label;
    this.freePanel.open = true;
    this.root.scrollIntoView({ block: "start" });
    if (!this.startButton.disabled) this.startButton.focus({ preventScroll: true });
    else {
      this.root.tabIndex = -1;
      this.root.focus({ preventScroll: true });
    }
  }

  /* Screen readers hear only starts and stops, not every batch. */
  announce(text) {
    this.announcer.textContent = text;
  }

  get recording() {
    return Boolean(this.active);
  }

  pipelines() {
    return this.status?.pipelines ?? [];
  }

  /* Starts recording a pipeline: reads the profile right before, checks
   * that its settings hash matches what the keyboard runs, and records
   * every profile the pipeline runs while recording. */
  async #startFromPanel() {
    try {
      if (this.supported === false && !(await this.checkSupport())) {
        throw new Error("the keyboard still doesn't answer capture requests");
      }
      await this.start({ pipelineId: Number(this.selectPipeline.value), label: this.label.value });
    } catch (error) {
      this.host.notice(`Could not record: ${error.message}`, true);
    }
  }

  async start(options) {
    if (this.active) throw new Error(`"${this.active.label}" is already recording`);
    if (this.starting) throw new Error("a recording is already starting");
    this.starting = true;
    this.render();
    try {
      return await this.#start(options);
    } finally {
      this.starting = false;
      this.render();
    }
  }

  async #start({ pipelineId, label, guided = null }) {
    if (!this.supported) throw new Error("this keyboard can't record motion");
    const pipeline = this.pipelines().find(({ id }) => id === pipelineId);
    if (!pipeline) throw new Error(`no pipeline ${pipelineId}`);
    if (!this.host.ready()) throw new Error("wait until the tuner has read every tuning value");
    const name = String(label ?? "").replace(/[“”"]/g, "").trim().slice(0, 40) || `${pipeline.label} ${new Date().toLocaleTimeString()}`;
    const { frame: describeFrame, description } = await this.#describe(pipelineId);
    const profile = this.host.profile();
    const stageIds = description.stages.map(({ stableId }) => stableId);
    const warnings = [];
    if (captureSettingsHash(profile, stageIds) !== description.settingsHash) {
      warnings.push("The tuner's values didn't match what the keyboard runs when recording started; traces with unknown settings will be skipped.");
    }
    this.active = {
      id: newRecordingId(),
      label: name, pipeline: description.stableId, pipelineId, description, stageIds,
      createdAt: new Date().toISOString(), startedMs: Date.now(), transport: this.host.transport(),
      chunks: [encodeFrame(describeFrame.type, describeFrame.payload)], decoder: captureDecoder(),
      stats: createRecordingStats(), profiles: [profile], warnings,
      startDropped: null, guided, clock: null,
    };
    try {
      const status = await this.#queryStatus(pipelineId);
      if (status.recording !== pipelineId) throw new Error("the keyboard didn't start recording");
      this.active.startDropped = status.dropped;
    } catch (error) {
      this.active = null;
      this.render();
      throw error;
    }
    this.poll = setInterval(() => this.#poll(), POLL_MS);
    this.host.onStart?.(description.stableId, { guided: Boolean(guided) });
    this.host.notice(`Recording ${description.stableId}. Recordings stay in this browser.`);
    this.announce(`Recording “${name}” started.`);
    this.render();
    return this.active;
  }

  async #poll() {
    if (!this.active || this.stopping) return;
    try {
      await this.#queryStatus(undefined);
      if (this.status.recording !== this.active?.pipelineId && this.active && !this.stopping) {
        /* The keyboard stopped by itself, such as after a missed heartbeat. */
        this.#finish("the keyboard stopped recording");
      }
    } catch {}
    this.renderLive();
  }

  /* The tuner's values changed: keep a profile for each settings hash the
   * pipeline may run, so every stretch replays with its settings. */
  tuningChanged() {
    if (!this.active || !this.host.ready()) return;
    const profile = this.host.profile();
    const hash = captureSettingsHash(profile, this.active.stageIds);
    if (!this.active.profiles.some((known) => captureSettingsHash(known, this.active.stageIds) === hash)) {
      this.active.profiles.push(profile);
    }
  }

  async stop(reason = "stopped") {
    if (!this.active || this.stopping) return null;
    this.stopping = true;
    this.render();
    /* A status answering an earlier query can arrive first and still say
     * recording, so ask until one says stopped. */
    const deadline = Date.now() + STOP_TIMEOUT_MS;
    try {
      do {
        await this.#queryStatus(CAPTURE.NONE, Math.max(deadline - Date.now(), 1));
      } while (this.status.recording !== null && this.active && Date.now() < deadline);
      if (this.status.recording !== null) throw new Error("the keyboard still reported recording");
    } catch (error) {
      this.active?.warnings.push(`The stop wasn't confirmed (${error.message}); records still buffered are missing.`);
    }
    return this.#finish(reason);
  }

  #finish(reason) {
    clearInterval(this.poll);
    const active = this.active;
    this.active = null;
    this.stopping = false;
    if (!active) return null;
    const { chunks, decoder, description, stageIds, startedMs, guided, clock, ...kept } = active;
    const recording = {
      ...kept,
      guided: guided && clock ? deviceGuide(guided, clock) : null,
      endReason: reason,
      durationMs: Date.now() - startedMs,
      stream: concat(chunks),
      firmwareDropped: this.status && active.startDropped !== null ? Math.max(0, this.status.dropped - active.startDropped) : null,
    };
    if (guided && !clock) recording.warnings.push("The keyboard's clock couldn't be read, so the guided test's labels were dropped.");
    this.add(recording);
    this.host.onEnd?.(recording.pipeline, { completed: reason !== "the connection ended", result: this.describe(recording) },
      { guided: Boolean(guided) });
    this.host.notice(`Saved recording "${recording.label}": ${recording.stats.frames} frames, ${recording.stats.dropped} records dropped, ${resyncs(recording.stats)} resyncs.`);
    this.announce(`Recording “${recording.label}” stopped: ${reason}.`);
    this.render();
    return recording;
  }

  /* --- Guided tests ---
   * A playground test records the pipeline that matches it while it runs,
   * with a label per trial or stretch in this page's clock and the
   * browser's pointer path, both moved to the device's clock when it
   * stops. */

  /* Starts recording for a guided test, or says why it can't. */
  async guidedStart(test, { label, acceleration = "unknown" } = {}) {
    const { mode } = GUIDED_TESTS[test] ?? {};
    this.guide = null;
    if (!mode) return { recording: false, reason: "this activity isn't a guided test" };
    /* Reset restarts the test: the run so far is kept as its own recording. */
    if (this.active?.guided) await this.stop("the guided test restarted");
    if (this.active || this.starting) return { recording: false, reason: `"${this.active?.label ?? "a recording"}" is already recording` };
    if (!(await this.checkSupport())) return { recording: false, reason: "this keyboard can't record motion" };
    const guide = { test, acceleration, labels: [], pointer: mode === "cursor" ? [] : null };
    this.guide = guide;
    this.guideStarting = (async () => {
      const pipeline = await this.#pipelineFor(mode);
      if (!pipeline) throw new Error(`the keyboard has no replayable ${mode} pipeline`);
      await this.start({ pipelineId: pipeline.id, label, guided: guide });
      return pipeline.label;
    })();
    try {
      return { recording: true, pipeline: await this.guideStarting };
    } catch (error) {
      if (this.guide === guide) this.guide = null;
      return { recording: false, reason: error.message };
    }
  }

  /* A trial or stretch of the guided test, with startPerf and endPerf in
   * performance.now() time. */
  guidedLabel(label) {
    this.guide?.labels.push(label);
  }

  guidedPointer(timeMs, x, y) {
    const samples = this.guide?.pointer;
    if (!samples || (samples.length && timeMs - samples.at(-1)[0] < POINTER_INTERVAL_MS)) return;
    samples.push([timeMs, x, y]);
  }

  /* Stops the guided test's recording and returns it, or null when it
   * didn't record or was stopped some other way. */
  async guidedFinish() {
    const guide = this.guide;
    this.guide = null;
    if (!guide) return null;
    await this.guideStarting?.catch(() => {});
    if (this.active?.guided !== guide) return null;
    return this.stop("the guided test ended");
  }

  /* What the agent and the run result see of a recording. */
  describe(recording) {
    return {
      recordingId: recording.id, label: recording.label, pipeline: recording.pipeline,
      truth: recording.guided ? "labels" : FREE_TRUTH,
      ...(recording.guided ? { guidedTest: recording.guided.test, labels: recording.guided.labels.length,
        pointerAcceleration: recording.guided.pointer?.acceleration ?? null } : {}),
      createdAt: recording.createdAt, durationMs: recording.durationMs, transport: recording.transport,
      frames: recording.stats.frames, keypresses: recording.stats.keypresses,
      droppedRecords: recording.stats.dropped, resyncs: resyncs(recording.stats),
      settingsChanges: recording.stats.settingsChanges, profiles: recording.profiles.length,
      endReason: recording.endReason, warnings: recording.warnings,
    };
  }

  /* --- Scoring --- */

  async #loadReplay() {
    this.#replay ??= Promise.all([
      import("./replay/capture-convert.js"),
      import("./replay/score-client.js"),
      import("./replay/quality.js"),
    ]).then(([convert, client, quality]) => ({ ...convert, ...client, METRICS: quality.METRICS }))
      .catch(() => {
        this.#replay = undefined;
        throw new Error(REPLAY_MISSING);
      });
    return this.#replay;
  }

  /* Converts recordings to traces. A guided recording's truth comes only
   * from its test's labels; a free recording's is derived from the motion
   * by the converter, which tags what it derived. */
  async traces(ids) {
    const replay = await this.#loadReplay();
    const warnings = [];
    const traces = [];
    for (const id of ids) {
      const recording = this.find(id);
      const truth = recording.guided
        ? { labels: recording.guided.labels, pointer: recording.guided.pointer ?? undefined, truth: "labels-only" }
        : { truth: "derive" };
      const { fixtures, warnings: found } = replay.captureToFixtures(replay.decodeCaptureStream(recording.stream),
        recording.profiles, { id: recording.id, description: `Tuner recording "${recording.label}"`,
          provenance: `zmk-pointing-tools tuner recording, ${recording.createdAt}`, ...truth });
      warnings.push(...found.map((warning) => `${recording.label}: ${warning}`));
      traces.push(...fixtures);
    }
    return { replay, traces, warnings };
  }

  /* Scores the current values and each candidate profile on recordings.
   * Candidates are { name, profile }. Free recordings, whose truth is a
   * heuristic, score only with free. */
  async score(chosen, candidates, { free = false } = {}) {
    const ids = [...new Set(chosen)];
    if (!ids.length) throw new Error("choose at least one recording");
    const unguided = ids.map((id) => this.find(id)).filter((recording) => !recording.guided);
    if (unguided.length && !free) {
      throw new Error(`${unguided.map(({ label }) => `"${label}"`).join(", ")} ${unguided.length === 1 ? "is a free recording" : "are free recordings"}: no targets, so truth would be a heuristic. Record a guided test, or score free recordings explicitly.`);
    }
    const { replay, traces, warnings } = await this.traces(ids);
    if (!traces.length) throw new Error(`no replayable traces in these recordings${warnings.length ? `: ${warnings.join("; ")}` : ""}`);
    this.#scorer ??= replay.createScoreWorker();
    const sets = [{ name: "current values", profile: this.host.profile() }, ...candidates];
    const scored = [];
    try {
      for (const { name, profile } of sets) scored.push({ name, ...(await this.#scorer.scoreTraces(traces, profile)) });
    } catch (error) {
      this.#scorer = undefined;
      throw error;
    }
    const rows = compareScoreSets(scored, replay.METRICS, metricTruth(traces, scored));
    /* Nothing scored isn't "no difference": say so instead of comparing. */
    if (!rows.some(({ values }) => values[0])) {
      throw new Error(`these recordings carry no ground truth yet, so no metric could be scored (${traces.length} traces). Nothing was compared.`);
    }
    /* The screen scale is fitted per recording from the recorded settings,
     * so every set and every trace of a recording carries the same one. */
    const recordingOf = new Map(traces.map((trace) => [trace.id, trace.metadata.capture?.recording]));
    const screen = [...new Map((scored[0].fixtureResults ?? []).filter((result) => result.screen)
      .map(({ id, screen: fit }) => [recordingOf.get(id), fit])).entries()]
      .map(([recording, fit]) => ({ recording: this.recordings.find(({ id }) => id === recording)?.label ?? recording, ...fit }));
    return {
      recordings: ids.map((id) => this.find(id).label),
      screen,
      traces: traces.length,
      strokes: traces.reduce((total, trace) => total + (trace.truth?.strokes.length ?? 0), 0),
      columns: sets.map(({ name }) => name),
      rows,
      warnings,
      caveat: SCORE_CAVEAT,
    };
  }

  #candidates() {
    return [...this.host.candidates(), ...this.importedProfiles.map(({ name, profile }) => ({ name: `profile ${name}`, profile }))];
  }

  async #scoreFromPanel() {
    const ids = [...this.list.querySelectorAll("[data-recording-pick]:checked")].map((box) => box.closest("[data-recording-id]").dataset.recordingId);
    const chosen = new Set([...this.candidateList.querySelectorAll("[data-candidate]:checked")].map((box) => box.dataset.candidate));
    const candidates = this.#candidates().filter(({ name }) => chosen.has(name));
    this.scoring = true;
    this.scoreButton.disabled = true;
    this.results.innerHTML = '<p class="muted">Scoring…</p>';
    try {
      /* Ticking a free recording asks for it explicitly. */
      this.result = await this.score(ids, candidates, { free: true });
      this.renderResult();
    } catch (error) {
      this.results.innerHTML = `<p class="device-error">${escapeHtml(error.message)}</p>`;
    } finally {
      this.scoring = false;
      this.renderCandidates();
    }
  }

  /* --- Rendering --- */

  render() {
    const pipelines = this.pipelines();
    const chosen = this.selectPipeline.value;
    this.selectPipeline.innerHTML = pipelines.length
      ? pipelines.map(({ id, label, replayable }) => `<option value="${id}" ${String(id) === chosen ? "selected" : ""}>${escapeHtml(label)}${replayable ? "" : " (doesn't replay)"}</option>`).join("")
      : '<option value="">No pipelines</option>';
    const recording = Boolean(this.active);
    this.selectPipeline.disabled = recording || this.starting || !pipelines.length;
    this.label.disabled = recording || this.starting;
    /* Unsupported but connected: Record asks the keyboard again. */
    this.startButton.disabled = recording || this.starting
      || !(this.supported === false ? this.host.connected() : this.supported && pipelines.length);
    this.stopButton.disabled = !recording || this.stopping;
    this.statusText.textContent = this.stopping ? "Stopping…"
      : recording ? "Recording" : this.starting ? "Starting…"
        : this.supported === null ? (this.host.connected() ? "Checking…" : "Not connected")
          : this.supported ? "Ready" : "Not supported";
    this.statusText.classList.toggle("live", recording);
    this.renderLive();
    this.renderList();
    this.renderCandidates();
  }

  renderLive() {
    const active = this.active;
    if (!active) {
      this.live.hidden = this.supported !== false;
      this.live.textContent = this.supported === false
        ? `This keyboard didn't answer capture requests, so it can't record motion (${this.unsupportedReason ?? "no answer"}). Firmware needs protocol v8 with CONFIG_ZMK_POINTING_TOOLS_INPUT_CAPTURE.`
        : "";
      return;
    }
    this.live.hidden = false;
    const used = this.status?.bufferUsed ?? 0;
    const size = this.status?.bufferSize || 1;
    const percent = Math.round((100 * used) / size);
    this.live.innerHTML = `<span>Buffer</span><meter min="0" max="100" value="${percent}" aria-label="Capture buffer fill">${percent}%</meter><span>${percent}%</span>
      <span>${seconds(Date.now() - active.startedMs)}</span>
      <span>${active.stats.frames.toLocaleString()} frames</span>
      <span class="${active.stats.dropped ? "warning" : ""}">${active.stats.dropped.toLocaleString()} dropped</span>
      <span>${resyncs(active.stats)} resyncs</span>`;
  }

  renderList() {
    if (!this.recordings.length) {
      this.list.innerHTML = '<p class="muted">No recordings yet.</p>';
      return;
    }
    /* New recordings start ticked; ones shown before keep their box. */
    const picked = new Set([...this.list.querySelectorAll("[data-recording-pick]:checked")].map((box) => box.closest("[data-recording-id]").dataset.recordingId));
    const shown = new Set([...this.list.querySelectorAll("[data-recording-id]")].map((row) => row.dataset.recordingId));
    /* Free recordings start unticked: their truth is a heuristic. */
    const tick = (recording) => picked.has(recording.id) || (!shown.has(recording.id) && Boolean(recording.guided));
    const truth = (recording) => (recording.guided
      ? `Guided: ${escapeHtml(ACTIVITIES.find(({ id }) => id === recording.guided.test)?.label ?? recording.guided.test)} <small>(${recording.guided.labels.length} labels)</small>`
      : `<span class="warning">Free: ${FREE_TRUTH}</span>`);
    this.list.innerHTML = `<table><thead><tr><th>Score</th><th>Recording</th><th>Truth</th><th>Pipeline</th><th>Recorded</th><th>Length</th><th>Frames</th><th>Dropped</th><th>Resyncs</th><th></th></tr></thead><tbody>${
      this.recordings.slice().reverse().map((recording) => `<tr data-recording-id="${escapeHtml(recording.id)}">
        <td><input type="checkbox" data-recording-pick ${tick(recording) ? "checked" : ""} aria-label="Score with ${escapeHtml(recording.label)}"></td>
        <td>${escapeHtml(recording.label)}</td><td>${truth(recording)}</td><td><code>${escapeHtml(recording.pipeline)}</code></td>
        <td>${escapeHtml(new Date(recording.createdAt).toLocaleString())}</td><td>${escapeHtml(seconds(Number(recording.durationMs)))}</td>
        <td>${escapeHtml(Number(recording.stats.frames).toLocaleString())}</td><td>${escapeHtml(Number(recording.stats.dropped).toLocaleString())}</td><td>${escapeHtml(resyncs(recording.stats))}</td>
        <td><button type="button" data-recording-export aria-label="Export ${escapeHtml(recording.label)}">Export</button>
          <button type="button" data-recording-delete aria-label="Delete ${escapeHtml(recording.label)}">Delete</button></td>
      </tr>`).join("")}</tbody></table>`;
  }

  renderCandidates() {
    const candidates = this.#candidates();
    const checked = new Set([...this.candidateList.querySelectorAll("[data-candidate]:checked")].map((box) => box.dataset.candidate));
    this.scoreButton.disabled = !this.recordings.length || this.scoring;
    const names = candidates.map(({ name }) => name).join("\n");
    if (names === this.candidateList.dataset.names) return;
    /* New candidates start ticked; ones shown before keep their box. */
    const shown = new Set((this.candidateList.dataset.names ?? "").split("\n"));
    this.candidateList.dataset.names = names;
    this.candidateList.innerHTML = `<label><input type="checkbox" checked disabled> Current values</label>${
      candidates.map(({ name }) => `<label><input type="checkbox" data-candidate="${escapeHtml(name)}" ${checked.has(name) || !shown.has(name) ? "checked" : ""}> ${escapeHtml(name)}</label>`).join("")
    }${candidates.length ? "" : '<span class="muted">Save snapshots in Agent access or import a profile to compare against.</span>'}`;
  }

  renderResult() {
    const { columns, rows: all, warnings, traces, strokes, caveat, screen } = this.result;
    /* Metrics nothing scored don't apply to these recordings. */
    const rows = all.filter(({ values }) => values.some(Boolean));
    const unscored = all.filter(({ values }) => !values.some(Boolean)).map(({ metric }) => metric);
    const notes = rows.flatMap((row) => row.note.map((note, index) => note && `${row.metric} for ${columns[index]}: ${note === "missing" ? "not scored" : note}`)).filter(Boolean);
    const format = (entry, unit) => (entry ? `${entry.value}${unit === "%" ? " %" : unit ? ` ${unit}` : ""} <small>(${entry.strokes})</small>` : "–");
    const change = (row, index) => {
      if (index === 0 || (row.change[index] === null && !row.note[index])) return "";
      const better = row.better[index];
      const text = row.change[index] === null ? "missing" : `${row.change[index] > 0 ? "+" : ""}${row.change[index]}`;
      return ` <small class="${better === true ? "better" : better === false ? "worse" : ""}">${escapeHtml(text)}${row.note[index] ? "*" : ""}</small>`;
    };
    this.results.innerHTML = `
      <p class="muted">${traces} traces, ${strokes} strokes. Values are means over strokes (stroke count in brackets); changes are from the current values, with better in green and worse in amber.</p>
      <table class="score-table"><thead><tr><th>Metric</th>${columns.map((name) => `<th>${escapeHtml(name)}</th>`).join("")}<th>Truth strokes by source</th></tr></thead><tbody>${
        rows.map((row) => `<tr><th scope="row">${escapeHtml(row.metric)} <small>${!row.ranked ? "not ranked" : row.higherIsBetter ? "higher is better" : "lower is better"}</small></th>${
          row.values.map((entry, index) => `<td>${format(entry, row.unit)}${change(row, index)}</td>`).join("")
        }<td class="muted">${escapeHtml(describeTruth(row.truth))}</td></tr>`).join("")
      }</tbody></table>
      ${unscored.length ? `<p class="muted">Not scored on these recordings: ${escapeHtml(unscored.join(", "))}.</p>` : ""}
      ${notes.length ? `<p class="muted">* Worse whatever the value: ${escapeHtml(notes.join("; "))}. A metric scored over different strokes can't be compared, and settings mustn't win by making strokes unscorable.</p>` : ""}
      ${screen.map((fit) => `<p class="${fit.usable ? "muted" : "warning"}">Screen scale for “${escapeHtml(fit.recording)}”: ${fit.pxPerCount ?? "–"} px per count (r² ${fit.r2 ?? "–"}, clocks ${fit.offsetMs ?? "–"} ms apart, pointer acceleration ${escapeHtml(fit.acceleration)}). ${fit.usable ? "Pixel metrics use it." : `Pixel metrics aren't scored: ${escapeHtml(fit.reason)}.`}</p>`).join("")}
      <p class="muted">Replay judges what doesn't depend on your reactions: each trial's first submovement, homing steps and wobble, and how the gain changes as you slow down. Total time, corrections, misses and throughput depend on you correcting what you see, so only live runs judge them.</p>
      <p class="muted">Truth sources: ${Object.entries(TRUTH_SOURCES).map(([source, text]) => `${source === "typing" ? "typing heuristic" : source}, ${escapeHtml(text)}`).join("; ")}.</p>
      <p class="temporary-warning">${escapeHtml(caveat)}</p>
      ${warnings.length ? `<details><summary>${warnings.length} conversion warnings</summary><ul>${warnings.map((warning) => `<li>${escapeHtml(warning)}</li>`).join("")}</ul></details>` : ""}`;
  }
}
