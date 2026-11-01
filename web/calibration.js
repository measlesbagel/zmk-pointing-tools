/* SPDX-License-Identifier: MIT */

import { STATE } from "./protocol.js";
import {
  MODES,
  accelerationConfig,
  accelerationTargets,
  analyzeCalibration,
  calibrationTargets,
  countSequenceGaps,
  createCalibrationExport,
  matchAccelerationTarget,
  parseStageFrame,
  segmentGestures,
} from "./calibration-model.js";

const MAX_FRAMES = 50_000;

function escapeHtml(value) {
  return String(value)
    .replaceAll("&", "&amp;")
    .replaceAll("<", "&lt;")
    .replaceAll(">", "&gt;")
    .replaceAll('"', "&quot;");
}

function option(value, label, selected) {
  return `<option value="${escapeHtml(value)}" ${selected ? "selected" : ""}>${escapeHtml(label)}</option>`;
}

function format(value, digits = 1) {
  return Number.isFinite(value) ? value.toFixed(digits) : "–";
}

/*
 * Guided calibration: record every-frame samples from a normalize stage
 * while the user repeats one gesture, then recommend the value that makes
 * that gesture produce the requested output. Apply previews it in RAM;
 * export writes an importable profile with the evidence.
 */
export class CalibrationPanel {
  constructor(root, host) {
    this.host = host;
    this.measure = root.querySelector("[data-calibration-measure]");
    this.target = root.querySelector("[data-calibration-target]");
    this.acceleration = root.querySelector("[data-calibration-acceleration]");
    this.accelerationField = root.querySelector("[data-calibration-acceleration-field]");
    this.output = root.querySelector("[data-calibration-output]");
    this.unit = root.querySelector("[data-calibration-unit]");
    this.status = root.querySelector("[data-calibration-status]");
    this.instructions = root.querySelector("[data-calibration-instructions]");
    this.result = root.querySelector("[data-calibration-result]");
    this.startButton = root.querySelector("[data-calibration-start]");
    this.stopButton = root.querySelector("[data-calibration-stop]");
    this.applyButton = root.querySelector("[data-calibration-apply]");
    this.exportButton = root.querySelector("[data-calibration-export]");
    this.frames = [];
    this.sequences = [];
    this.mode = "cursor";

    this.startButton.addEventListener("click", () => this.start());
    this.stopButton.addEventListener("click", () => this.stop());
    this.applyButton.addEventListener("click", () => this.apply());
    this.exportButton.addEventListener("click", () => this.export());
    this.target.addEventListener("change", () => {
      const mode = this.target.value.split(":")[0];
      if (mode !== this.mode) {
        this.mode = mode;
        this.output.value = MODES[mode].defaultOutput;
        this.acceleration.dataset.chosen = "";
      }
      this.refresh();
    });
    this.acceleration.addEventListener("change", () => {
      this.acceleration.dataset.chosen = "true";
      this.analyze();
    });
    this.output.addEventListener("input", () => this.analyze());
    this.output.value = MODES[this.mode].defaultOutput;
    this.refresh();
  }

  selectedTarget() {
    const [, id] = this.target.value.split(":");
    return id ? this.host.getTuningTargets().get(Number(id)) : undefined;
  }

  selectedAcceleration() {
    if (this.mode !== "cursor" || this.acceleration.value === "") return undefined;
    return this.host.getTuningTargets().get(Number(this.acceleration.value));
  }

  /* Rebuild the selectors from what the firmware advertises, keeping the
   * current choices where they still exist. */
  refresh() {
    const { levels, labels } = this.host.getStateStatus();
    const stateTargets = [...levels.keys()];
    const measured = this.measure.value === "" ? undefined : Number(this.measure.value);
    const measureId = stateTargets.includes(measured)
      ? measured
      : stateTargets.find((id) => (labels.get(id) ?? "").includes("normalize")) ?? stateTargets[0];
    this.measure.innerHTML = stateTargets.length
      ? stateTargets.map((id) => option(id, labels.get(id) || `Stage ${id}`, id === measureId)).join("")
      : option("", "No stage targets", true);
    this.measure.disabled = this.recording || stateTargets.length === 0;

    const tuning = this.host.getTuningTargets();
    const choices = calibrationTargets(tuning).map(({ mode, target }) => ({
      value: `${mode}:${target.id}`,
      label: `${MODES[mode].label} · ${target.label}`,
    }));
    /* Without a tunable target the panel still recommends a value. */
    for (const mode of Object.values(MODES)) {
      if (!choices.some(({ value }) => value.startsWith(`${mode.id}:`))) {
        choices.push({ value: `${mode.id}:`, label: `${mode.label} · recommendation only` });
      }
    }
    const chosen = choices.some(({ value }) => value === this.target.value) ? this.target.value : choices[0].value;
    this.target.innerHTML = choices.map(({ value, label }) => option(value, label, value === chosen)).join("");
    if (chosen.split(":")[0] !== this.mode) {
      this.mode = chosen.split(":")[0];
      this.output.value = MODES[this.mode].defaultOutput;
    }

    const candidates = accelerationTargets(tuning);
    const keep = this.acceleration.dataset.chosen === "true" &&
      (this.acceleration.value === "" || candidates.some(({ id }) => String(id) === this.acceleration.value));
    const accelerationId = keep
      ? this.acceleration.value
      : String(matchAccelerationTarget(this.selectedTarget(), candidates)?.id ?? "");
    this.acceleration.innerHTML = [
      option("", "None", accelerationId === ""),
      ...candidates.map((target) => option(target.id, target.label, String(target.id) === accelerationId)),
    ].join("");
    this.accelerationField.hidden = this.mode !== "cursor";

    this.unit.textContent = MODES[this.mode].unit;
    this.instructions.textContent = MODES[this.mode].instructions;
    this.startButton.disabled = this.recording || stateTargets.length === 0 || !this.host.canControl();
    this.stopButton.disabled = !this.recording;
    this.target.disabled = this.recording;
    /* Current values feed the acceleration model and the "now" column. */
    this.analyze();
  }

  start() {
    if (this.recording || this.measure.value === "") return;
    this.measureId = Number(this.measure.value);
    this.measureLabel = this.host.getStateStatus().labels.get(this.measureId) || `Stage ${this.measureId}`;
    this.previousLevel = this.host.getStateStatus().levels.get(this.measureId) ?? STATE.OFF;
    this.startDropped = this.host.getDropped();
    this.frames = [];
    this.sequences = [];
    this.analysis = undefined;
    this.recording = true;
    this.host.setStateLevel(this.measureId, STATE.VERBOSE);
    this.status.textContent = "Recording";
    this.result.innerHTML = '<p class="muted">Recording… make the same gesture 5 to 10 times.</p>';
    this.timer = setInterval(() => this.renderProgress(), 250);
    this.refresh();
    this.host.onRunStart?.();
  }

  /* What an agent's calibration run reports. */
  runResult() {
    const target = this.selectedTarget();
    const analysis = this.analysis && (({ gestures, ...rest }) => ({
      ...rest, gestureDistancesMm: gestures.map((gesture) => (this.mode === "cursor" ? gesture.acceleratedMm : gesture.axisMm)),
    }))(this.analysis);
    return {
      mode: this.mode,
      measuredStage: this.measureLabel,
      target: target ? target.stableId ?? target.label : null,
      frames: this.frames.length,
      analysis: analysis ?? null,
    };
  }

  stop() {
    if (!this.recording) return;
    this.recording = false;
    clearInterval(this.timer);
    this.host.setStateLevel(this.measureId, this.previousLevel);
    this.status.textContent = "Stopped";
    this.refresh();
    this.host.onRunEnd?.({ completed: true, result: this.runResult() });
  }

  /* Disconnects and simulator restarts end a recording without a host. */
  cancel() {
    if (!this.recording) return;
    this.recording = false;
    clearInterval(this.timer);
    this.status.textContent = "Cancelled";
    this.refresh();
    this.host.onRunEnd?.({ completed: false, result: this.runResult() });
  }

  handleSample(sample) {
    if (!this.recording) return;
    this.sequences.push({ sequence: sample.sequence });
    if (sample.targetId !== this.measureId || this.frames.length >= MAX_FRAMES) return;
    const frame = parseStageFrame(sample);
    if (frame) this.frames.push(frame);
  }

  renderProgress() {
    const gestures = segmentGestures(this.frames).length;
    this.result.innerHTML = `<p class="muted">Recording… ${this.frames.length.toLocaleString()} frames, ${gestures} gestures so far.</p>`;
  }

  telemetryDropped() {
    return Math.max(this.host.getDropped() - this.startDropped, countSequenceGaps(this.sequences));
  }

  analyze() {
    if (this.recording || this.frames.length === 0) {
      this.updateActions();
      return;
    }
    try {
      this.analysis = analyzeCalibration(this.frames, {
        mode: this.mode,
        desiredOutput: Number(this.output.value),
        target: this.selectedTarget(),
        acceleration: accelerationConfig(this.selectedAcceleration()),
        telemetryDropped: this.telemetryDropped(),
      });
    } catch (error) {
      this.analysis = undefined;
      this.result.innerHTML = `<p class="muted">${escapeHtml(error.message)}</p>`;
      this.updateActions();
      return;
    }
    this.renderAnalysis();
    this.updateActions();
  }

  updateActions() {
    const ready = Boolean(this.analysis?.recommendation.parameters.length) && !this.recording;
    this.applyButton.disabled = !ready || !this.selectedTarget() || !this.host.canControl();
    this.exportButton.disabled = !this.analysis || this.recording;
  }

  describeRecommendation() {
    const { recommendation } = this.analysis;
    if (!recommendation.parameters.length) return "No recommendation yet.";
    const target = this.selectedTarget();
    const parts = recommendation.parameters.map(({ key, value }) => {
      const parameter = target?.parameters.find((candidate) => candidate.key === key);
      const unit = parameter?.unit ? ` ${parameter.unit}` : "";
      const current = parameter ? ` (now ${parameter.current}${unit})` : "";
      return `<code>${escapeHtml(key)}</code> = ${value}${escapeHtml(unit)}${escapeHtml(current)}`;
    });
    const gain = recommendation.gain === undefined ? "" : ` · gain ${format(recommendation.gain, 3)}×`;
    return `${parts.join(", ")}${gain}`;
  }

  renderAnalysis() {
    const { summary, confidence, gestures } = this.analysis;
    const distance = summary.distanceMm;
    const lost = summary.missingFrames + summary.transportGapFrames;
    const metrics = [
      ["Gestures", `${summary.gestureCount}${summary.ignoredGestures ? ` (+${summary.ignoredGestures} ignored)` : ""}`],
      [this.mode !== "cursor" ? `Distance (${summary.axis} axis)`
        : this.selectedAcceleration() ? "Distance (with acceleration)" : "Distance",
        `${format(distance.median)} mm median · ±${format(distance.sd)} · CV ${format(distance.cv * 100, 0)} %`],
      ["Duration", `${format(summary.durationMs, 0)} ms`],
      ["Frames per gesture", format(summary.framesPerGesture, 0)],
      ["Arrival interval", `${format(summary.intervalMs)} ms`],
      ["Sensor span", `${format(summary.spanMs)} ms`],
      ["Resolution", `${summary.cpi} CPI · source ${summary.sourceId ?? "–"}`],
      ["Coalesced frames", summary.coalescedFrames],
      ["Lost frames", lost],
      ["Clipped / reversed", `${summary.clippedFrames} / ${summary.reversedFrames}`],
      ["Telemetry dropped", summary.telemetryDropped],
    ];
    this.result.innerHTML = `
      <p class="calibration-recommendation"><span class="calibration-confidence ${confidence.level}">${escapeHtml(confidence.level)}</span>${this.describeRecommendation()}</p>
      <div class="metrics">${metrics.map(([label, value]) => `<span>${escapeHtml(label)}</span><strong>${escapeHtml(value)}</strong>`).join("")}</div>
      <p class="muted calibration-gestures">Gesture distances: ${gestures.map((gesture) => format(this.mode === "cursor" ? gesture.acceleratedMm : gesture.axisMm)).join(", ") || "none"} mm</p>
      ${confidence.limitations.length ? `<ul class="calibration-limitations">${confidence.limitations.map((line) => `<li>${escapeHtml(line)}</li>`).join("")}</ul>` : ""}`;
  }

  apply() {
    const target = this.selectedTarget();
    if (!target || !this.analysis) return;
    this.host.applyValues(target, this.analysis.recommendation.parameters);
  }

  export() {
    if (!this.analysis) return;
    const acceleration = accelerationConfig(this.selectedAcceleration());
    this.host.onExport(createCalibrationExport(this.analysis, {
      target: this.selectedTarget(),
      measureLabel: this.measureLabel,
      acceleration: this.mode === "cursor" ? acceleration : undefined,
      frames: this.frames,
    }));
  }
}
