import {
  ACTIVITIES,
  PRECISION_LAYOUT,
  PRECISION_START,
  createRun,
  exportRun,
  finishPrecisionTrial,
  finishRun,
  observePrecisionPointer,
  precisionTrials,
  recordArrow,
  recordClick,
  recordDrag,
  recordPointer,
  recordSelection,
  recordWheel,
  startPrecisionTrial,
  summarizePrecision,
} from "./playground-model.js";

const PRECISION_DWELL_MS = 500;
const PRECISION_HISTORY_KEY = "zpt-precision-runs";
const PRECISION_HISTORY_LIMIT = 20;

/* Saved precision summaries live in this browser only; storage can be
 * missing or full, and the test still runs without it. */
export function loadPrecisionHistory() {
  try {
    const saved = JSON.parse(localStorage.getItem(PRECISION_HISTORY_KEY) ?? "[]");
    return Array.isArray(saved) ? saved : [];
  } catch {
    return [];
  }
}

function savePrecisionHistory(history) {
  try {
    localStorage.setItem(PRECISION_HISTORY_KEY, JSON.stringify(history.slice(-PRECISION_HISTORY_LIMIT)));
  } catch {
    /* Comparison is lost on reload, nothing else. */
  }
}

const TARGET_POSITIONS = [
  [12, 18], [82, 78], [22, 70], [74, 20], [48, 48], [88, 42],
];

function element(tag, className, text) {
  const node = document.createElement(tag);
  if (className) node.className = className;
  if (text !== undefined) node.textContent = text;
  return node;
}

export class PointingPlayground {
  constructor(root, { getContext, onExport, onRunStart = () => {}, onRunEnd = () => {} }) {
    this.root = root;
    this.getContext = getContext;
    this.onExport = onExport;
    this.onRunStart = onRunStart;
    this.onRunEnd = onRunEnd;
    this.dialog = root.querySelector("dialog");
    this.activity = root.querySelector("[data-playground-activity]");
    this.surface = root.querySelector("[data-playground-surface]");
    this.instructions = root.querySelector("[data-playground-instructions]");
    this.status = root.querySelector("[data-playground-status]");
    this.metrics = root.querySelector("[data-playground-metrics]");
    this.startButton = root.querySelector("[data-playground-start]");
    this.resetButton = root.querySelector("[data-playground-reset]");
    this.finishButton = root.querySelector("[data-playground-finish]");
    this.exportButton = root.querySelector("[data-playground-export]");
    this.precisionFields = root.querySelectorAll("[data-playground-precision]");
    this.label = root.querySelector("[data-playground-label]");
    this.acquire = root.querySelector("[data-playground-acquire]");
    this.results = root.querySelector("[data-playground-results]");
    this.acquire.addEventListener("change", () => {
      this.run = undefined;
      this.renderActivity();
    });
    this.results.addEventListener("click", (event) => {
      if (!event.target.matches("[data-precision-clear]")) return;
      savePrecisionHistory([]);
      this.renderPrecisionResults();
    });

    for (const category of [...new Set(ACTIVITIES.map(({ category }) => category))]) {
      const group = document.createElement("optgroup");
      group.label = category;
      for (const activity of ACTIVITIES.filter((item) => item.category === category)) {
        const option = document.createElement("option");
        option.value = activity.id;
        option.textContent = activity.label;
        group.append(option);
      }
      this.activity.append(group);
    }

    root.querySelector("[data-playground-open]").addEventListener("click", () => {
      this.dialog.showModal();
      this.renderActivity();
    });
    root.querySelector("[data-playground-close]").addEventListener("click", () => this.dialog.close());
    this.dialog.addEventListener("close", () => {
      if (this.running) this.finish(false);
      this.stop();
    });
    this.activity.addEventListener("change", () => {
      this.run = undefined;
      this.renderActivity();
    });
    this.startButton.addEventListener("click", () => this.start());
    this.resetButton.addEventListener("click", () => this.start());
    this.finishButton.addEventListener("click", () => this.finish(true));
    this.exportButton.addEventListener("click", () => this.export());
    this.renderActivity();
  }

  /* Opens the playground on the activity an agent asked for, with its
   * label, for the user to start. */
  prepare({ activity, label, acquire }) {
    if (this.running) this.finish(false);
    this.activity.value = activity;
    if (activity === "cursor-precision") {
      this.label.value = label;
      if (acquire) this.acquire.value = acquire;
    }
    this.run = undefined;
    if (!this.dialog.open) this.dialog.showModal();
    this.renderActivity();
    this.startButton.focus();
  }

  stop() {
    clearInterval(this.timer);
    clearTimeout(this.dwellTimer);
    this.listeners?.abort();
    document.body.classList.remove("playground-running");
  }

  start() {
    this.stop();
    const context = this.getContext();
    this.run = createRun(this.activity.value, context);
    this.running = true;
    document.body.classList.add("playground-running");
    this.renderActivity();
    this.startButton.disabled = true;
    this.activity.disabled = true;
    this.acquire.disabled = true;
    this.resetButton.disabled = false;
    this.finishButton.disabled = false;
    this.exportButton.disabled = false;
    this.status.textContent = "Running";
    this.timer = setInterval(() => this.renderMetrics(), 100);
    this.renderMetrics();
    this.onRunStart(this.activity.value);
  }

  finish(completed = true) {
    if (!this.run || !this.running) return;
    finishRun(this.run, completed);
    this.running = false;
    clearInterval(this.timer);
    document.body.classList.remove("playground-running");
    this.startButton.disabled = false;
    this.activity.disabled = false;
    this.acquire.disabled = false;
    clearTimeout(this.dwellTimer);
    this.finishButton.disabled = true;
    this.status.textContent = completed ? "Completed" : "Stopped";
    this.renderMetrics();
    const { activity, durationMs, metrics, precision } = this.run;
    this.onRunEnd(activity.id, {
      completed,
      result: {
        durationMs,
        metrics: { ...metrics },
        ...(precision ? { precision: { label: precision.label, acquire: precision.acquire, trials: precision.trials.length, summary: precision.summary ?? null } } : {}),
      },
    });
  }

  complete() { this.finish(true); }

  export() {
    if (!this.run) return;
    if (this.running) this.finish(false);
    this.onExport(exportRun(this.run, this.getContext()));
  }

  listen(target, type, callback, options = {}) {
    target.addEventListener(type, callback, { ...options, signal: this.listeners.signal });
  }

  renderActivity() {
    this.listeners?.abort();
    this.listeners = new AbortController();
    this.surface.replaceChildren();
    this.surface.className = "playground-surface";
    this.metrics.textContent = "Start the activity to collect measurements.";
    this.status.textContent = this.run && this.running ? "Running" : "Ready";
    this.startButton.disabled = Boolean(this.running);
    this.resetButton.disabled = !this.run;
    this.finishButton.disabled = !this.running;
    this.exportButton.disabled = !this.run;
    const precision = this.activity.value === "cursor-precision";
    for (const field of this.precisionFields) field.hidden = !precision;
    this.results.hidden = !precision;

    switch (this.activity.value) {
    case "scroll-vertical": this.renderScroll("vertical"); break;
    case "scroll-horizontal": this.renderScroll("horizontal"); break;
    case "scroll-two-axis": this.renderScroll("both"); break;
    case "text-selection": this.renderText(); break;
    case "cursor-targets": this.renderTargets(); break;
    case "cursor-precision": this.renderPrecision(); break;
    case "cursor-path": this.renderPath(); break;
    case "click-drag": this.renderDrag(); break;
    }
  }

  renderMetrics() {
    if (!this.run) return;
    const metrics = this.run.metrics;
    const elapsed = this.run.durationMs ?? Date.now() - this.run.startedMs;
    if (this.run.precision) {
      const { trials, total, current } = this.run.precision;
      const done = trials.length;
      this.metrics.textContent = [
        `${(elapsed / 1000).toFixed(1)} s`,
        done >= total ? `${total} targets done` : current ? `Target ${done + 1} of ${total} · ${current} px` : "Acquire START to begin",
        `${trials.reduce((sum, { corrections }) => sum + corrections, 0)} corrections`,
        `${metrics.errors} misses`,
      ].join(" · ");
      return;
    }
    this.metrics.textContent = [
      `${(elapsed / 1000).toFixed(1)} s`,
      `${metrics.wheelEvents} wheel frames`,
      `H/V ${metrics.wheelX.toFixed(0)}/${metrics.wheelY.toFixed(0)}`,
      `${metrics.arrowKeys} arrows`,
      `${metrics.targets} targets`,
      `${metrics.errors} errors`,
      `${metrics.pointerDistance.toFixed(0)} px path`,
    ].join(" · ");
  }

  renderScroll(axis) {
    this.instructions.textContent = axis === "vertical"
      ? "Scroll from the start marker to the bottom marker. Horizontal wheel input is measured but ignored."
      : axis === "horizontal"
        ? "Scroll from the left marker to the right marker. Vertical wheel input is measured but ignored."
        : "Scroll diagonally from the top-left marker to the bottom-right marker.";
    const viewport = element("div", `playground-scroll ${axis}`);
    viewport.tabIndex = 0;
    viewport.setAttribute("aria-label", `${axis} scroll test surface`);
    const content = element("div", "playground-scroll-content");
    content.innerHTML = '<strong class="playground-start-marker">START</strong><strong class="playground-end-marker">FINISH</strong>';
    viewport.append(content);
    this.surface.append(viewport);
    viewport.scrollTo(0, 0);

    this.listen(viewport, "wheel", (event) => {
      if (!this.running) return;
      event.preventDefault();
      recordWheel(this.run, event.deltaX, event.deltaY);
      viewport.scrollBy({
        left: axis === "vertical" ? 0 : event.deltaX,
        top: axis === "horizontal" ? 0 : event.deltaY,
      });
      this.renderMetrics();
    }, { passive: false });
    this.listen(viewport, "scroll", () => {
      if (!this.running) return;
      const horizontalDone = axis === "vertical" ||
        viewport.scrollLeft >= viewport.scrollWidth - viewport.clientWidth - 4;
      const verticalDone = axis === "horizontal" ||
        viewport.scrollTop >= viewport.scrollHeight - viewport.clientHeight - 4;
      if (horizontalDone && verticalDone) this.complete();
    });
    queueMicrotask(() => { if (this.dialog.open) viewport.focus(); });
  }

  renderText() {
    this.instructions.textContent = "Move from START to TARGET with arrow navigation, then select exactly the word TARGET while holding Shift.";
    const text = [
      "START — move through this stable text-navigation exercise.",
      "The second line makes vertical movement observable and repeatable.",
      "Finish by selecting the unique word TARGET and nothing else.",
    ].join("\n");
    const targetStart = text.indexOf("TARGET");
    const textarea = element("textarea", "playground-text");
    textarea.value = text;
    textarea.spellcheck = false;
    textarea.setAttribute("aria-label", "Text navigation test document");
    const position = element("div", "playground-text-position", "Caret 0 · no selection");
    position.setAttribute("aria-live", "polite");
    this.surface.append(textarea, position);
    const updatePosition = () => {
      const selected = textarea.selectionEnd - textarea.selectionStart;
      position.textContent = selected
        ? `Selection ${textarea.selectionStart}–${textarea.selectionEnd} (${selected} characters)`
        : `Caret ${textarea.selectionStart} · no selection`;
    };
    this.listen(textarea, "beforeinput", (event) => event.preventDefault());
    this.listen(textarea, "keydown", (event) => {
      if (this.running && event.key.startsWith("Arrow")) {
        recordArrow(this.run);
        this.renderMetrics();
      }
    });
    this.listen(textarea, "select", () => {
      updatePosition();
      if (!this.running) return;
      recordSelection(this.run);
      if (textarea.selectionStart === targetStart && textarea.selectionEnd === targetStart + 6) {
        this.complete();
      }
      this.renderMetrics();
    });
    this.listen(textarea, "keyup", updatePosition);
    this.listen(textarea, "click", updatePosition);
    queueMicrotask(() => {
      if (!this.dialog.open) return;
      textarea.focus();
      textarea.setSelectionRange(0, 0);
      updatePosition();
    });
  }

  renderTargets() {
    this.instructions.textContent = "Click each target in order. Misses, elapsed time, and pointer path length are recorded.";
    const area = element("div", "playground-pointer-area");
    area.tabIndex = 0;
    area.setAttribute("aria-label", "Cursor targets");
    this.surface.append(area);
    let targetIndex = 0;
    let previous;
    const placeTarget = () => {
      area.querySelector("button")?.remove();
      if (targetIndex >= TARGET_POSITIONS.length) {
        this.complete();
        return;
      }
      const target = element("button", "playground-target", String(targetIndex + 1));
      target.type = "button";
      target.style.left = `${TARGET_POSITIONS[targetIndex][0]}%`;
      target.style.top = `${TARGET_POSITIONS[targetIndex][1]}%`;
      target.setAttribute("aria-label", `Target ${targetIndex + 1} of ${TARGET_POSITIONS.length}`);
      this.listen(target, "click", (event) => {
        event.stopPropagation();
        if (!this.running) return;
        recordClick(this.run, true);
        targetIndex += 1;
        placeTarget();
        this.renderMetrics();
      });
      area.append(target);
    };
    placeTarget();
    this.listen(area, "click", () => {
      if (this.running) recordClick(this.run, false);
      this.renderMetrics();
    });
    this.listen(area, "pointermove", (event) => {
      if (!this.running) return;
      previous = recordPointer(this.run, previous, { x: event.clientX, y: event.clientY });
    });
  }

  /* Shrinking targets acquired by holding still or clicking. Each trial is
   * timed from the moment the previous target was acquired, so every run
   * makes the same moves. */
  renderPrecision() {
    const hover = this.acquire.value === "hover";
    this.instructions.textContent = `${hover ? "Hold the pointer still inside" : "Click inside"} START, then each target as it appears; they shrink from 32 to 4 px. ` +
      "Label each run (say, Sniper held, then a curve) to compare them below. Set the computer's own pointer acceleration to flat first, or it adds its own curve.";
    this.renderPrecisionResults();
    this.surface.classList.add("precision");
    const area = element("div", "playground-pointer-area precision");
    area.tabIndex = 0;
    area.setAttribute("aria-label", "Small-target precision test");
    const box = element("div", "playground-precision-box");
    box.style.width = `${PRECISION_LAYOUT.width}px`;
    box.style.height = `${PRECISION_LAYOUT.height}px`;
    const marker = element("div", "playground-precision-target");
    box.append(marker);
    area.append(box);
    this.surface.append(area);

    const trials = precisionTrials();
    const results = [];
    const start = { ...PRECISION_START, index: -1, distance: 0 };
    let index = -1;
    let state = startPrecisionTrial(start, start, performance.now());
    let previous;
    const target = () => (index < 0 ? start : trials[index]);
    const show = () => {
      const { x, y, size } = target();
      marker.style.left = `${x}px`;
      marker.style.top = `${y}px`;
      marker.style.width = `${size}px`;
      marker.style.height = `${size}px`;
      marker.textContent = index < 0 ? "START" : "";
      marker.classList.remove("inside");
    };
    show();
    if (this.run && this.running) {
      this.run.precision = {
        label: this.label.value.trim() || `Run ${loadPrecisionHistory().length + 1}`,
        acquire: this.acquire.value,
        dwellMs: hover ? PRECISION_DWELL_MS : 0,
        total: trials.length,
        current: undefined,
        trials: results,
      };
    }

    const acquired = (now) => {
      clearTimeout(this.dwellTimer);
      if (index >= 0) {
        results.push(finishPrecisionTrial(state, now, hover));
        recordClick(this.run, true);
      }
      index += 1;
      if (index >= trials.length) {
        marker.remove();
        this.finishPrecision(results);
        return;
      }
      this.run.precision.current = trials[index].size;
      state = startPrecisionTrial(trials[index], index === 0 ? start : trials[index - 1], now);
      show();
      this.renderMetrics();
    };
    const point = (event) => {
      const bounds = box.getBoundingClientRect();
      return { x: event.clientX - bounds.left, y: event.clientY - bounds.top };
    };

    this.listen(area, "pointermove", (event) => {
      if (!this.running || index >= trials.length) return;
      for (const sample of event.getCoalescedEvents?.() ?? [event]) {
        const position = point(sample);
        previous = recordPointer(this.run, previous, position);
        const wasInside = state.inside;
        const inside = observePrecisionPointer(state, position, sample.timeStamp);
        marker.classList.toggle("inside", inside);
        if (!hover || inside === wasInside) continue;
        clearTimeout(this.dwellTimer);
        if (inside) {
          const entered = state.enteredMs;
          this.dwellTimer = setTimeout(() => acquired(entered + PRECISION_DWELL_MS), PRECISION_DWELL_MS);
        }
      }
    });
    this.listen(area, "click", (event) => {
      if (!this.running || hover || index >= trials.length) return;
      const position = point(event);
      observePrecisionPointer(state, position, event.timeStamp);
      if (state.inside) {
        acquired(event.timeStamp);
      } else {
        if (index >= 0) state.misses += 1;
        recordClick(this.run, false);
        this.renderMetrics();
      }
    });
    queueMicrotask(() => { if (this.dialog.open) area.focus(); });
  }

  finishPrecision(results) {
    const { label, acquire } = this.run.precision;
    const summary = summarizePrecision(results);
    this.run.precision.summary = summary;
    this.run.precision.current = undefined;
    const history = loadPrecisionHistory();
    history.push({ label, acquire, at: new Date().toISOString(), summary });
    savePrecisionHistory(history);
    this.complete();
    this.renderPrecisionResults(history);
  }

  /* Saved runs side by side: per target size, the median time to land,
   * how often the pointer left the target and came back, and the median
   * overshoot past its far edge. */
  renderPrecisionResults(history = loadPrecisionHistory()) {
    if (!history.length) {
      this.results.innerHTML = '<p class="muted">Finished runs appear here for comparison.</p>';
      return;
    }
    const sizes = [...new Set(history.flatMap(({ summary }) => summary.sizes.map(({ size }) => size)))]
      .sort((a, b) => b - a);
    const table = element("table");
    const head = table.createTHead().insertRow();
    for (const heading of ["Run", "Acquire", "Total", "Throughput", ...sizes.map((size) => `${size} px`)]) {
      head.append(element("th", undefined, heading));
    }
    const body = table.createTBody();
    for (const { label, acquire, summary } of history.slice().reverse()) {
      const row = body.insertRow();
      row.insertCell().textContent = label;
      row.insertCell().textContent = acquire === "click" ? "Click" : "Hold";
      row.insertCell().textContent = `${(summary.totalTimeMs / 1000).toFixed(1)} s`;
      row.insertCell().textContent = `${summary.throughput.toFixed(2)} bit/s`;
      for (const size of sizes) {
        const block = summary.sizes.find((item) => item.size === size);
        row.insertCell().textContent = block
          ? [
            `${(block.medianTimeMs / 1000).toFixed(2)} s`,
            `${block.corrections} fix`,
            `${block.medianOvershootPx} px over`,
            ...(block.misses ? [`${block.misses} miss`] : []),
          ].join(" · ")
          : "–";
      }
    }
    const heading = element("div", "playground-results-heading");
    heading.append(
      element("span", undefined, "Saved runs, newest first. Times are medians per size; fix counts times the pointer left the target and came back."),
    );
    const clear = element("button", undefined, "Clear runs");
    clear.type = "button";
    clear.dataset.precisionClear = "";
    heading.append(clear);
    this.results.replaceChildren(heading, table);
  }

  renderPath() {
    this.instructions.textContent = "Follow the diagonal line, then trace the circle. Use Finish when the pass is complete.";
    const canvas = element("canvas", "playground-path");
    canvas.width = 900;
    canvas.height = 420;
    canvas.tabIndex = 0;
    canvas.setAttribute("aria-label", "Diagonal and circular cursor path exercise");
    this.surface.append(canvas);
    const context = canvas.getContext("2d");
    context.strokeStyle = "#39434f";
    context.lineWidth = 10;
    context.beginPath();
    context.moveTo(50, 370);
    context.lineTo(400, 50);
    context.stroke();
    context.beginPath();
    context.arc(675, 210, 135, 0, Math.PI * 2);
    context.stroke();
    let previous;
    this.listen(canvas, "pointermove", (event) => {
      if (!this.running) return;
      const bounds = canvas.getBoundingClientRect();
      const current = {
        x: (event.clientX - bounds.left) * canvas.width / bounds.width,
        y: (event.clientY - bounds.top) * canvas.height / bounds.height,
      };
      if (previous) {
        context.strokeStyle = "#78d6b0";
        context.lineWidth = 2;
        context.beginPath();
        context.moveTo(previous.x, previous.y);
        context.lineTo(current.x, current.y);
        context.stroke();
      }
      previous = recordPointer(this.run, previous, current);
    });
  }

  renderDrag() {
    this.instructions.textContent = "Press and hold the token, drag it into the destination, then release.";
    const area = element("div", "playground-drag-area");
    const token = element("button", "playground-drag-token", "DRAG");
    token.type = "button";
    const destination = element("div", "playground-drop-zone", "DROP HERE");
    area.append(token, destination);
    this.surface.append(area);
    let dragging = false;
    let previous;
    this.listen(token, "pointerdown", (event) => {
      if (!this.running) return;
      dragging = true;
      token.setPointerCapture(event.pointerId);
      previous = { x: event.clientX, y: event.clientY };
    });
    this.listen(token, "pointermove", (event) => {
      if (!dragging || !this.running) return;
      const areaBounds = area.getBoundingClientRect();
      const current = { x: event.clientX, y: event.clientY };
      previous = recordPointer(this.run, previous, current);
      token.style.left = `${event.clientX - areaBounds.left}px`;
      token.style.top = `${event.clientY - areaBounds.top}px`;
    });
    this.listen(token, "pointerup", () => {
      if (!dragging || !this.running) return;
      dragging = false;
      const tokenBounds = token.getBoundingClientRect();
      const destinationBounds = destination.getBoundingClientRect();
      const correct = tokenBounds.left >= destinationBounds.left &&
        tokenBounds.right <= destinationBounds.right &&
        tokenBounds.top >= destinationBounds.top &&
        tokenBounds.bottom <= destinationBounds.bottom;
      recordDrag(this.run, correct);
      if (correct) this.complete();
      this.renderMetrics();
    });
  }
}
