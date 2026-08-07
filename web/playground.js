import {
  ACTIVITIES,
  createRun,
  exportRun,
  finishRun,
  recordArrow,
  recordClick,
  recordDrag,
  recordPointer,
  recordSelection,
  recordWheel,
} from "./playground-model.js";

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
  constructor(root, { getContext, onExport }) {
    this.root = root;
    this.getContext = getContext;
    this.onExport = onExport;
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

  stop() {
    clearInterval(this.timer);
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
    this.resetButton.disabled = false;
    this.finishButton.disabled = false;
    this.exportButton.disabled = false;
    this.status.textContent = "Running";
    this.timer = setInterval(() => this.renderMetrics(), 100);
    this.renderMetrics();
  }

  finish(completed = true) {
    if (!this.run || !this.running) return;
    finishRun(this.run, completed);
    this.running = false;
    clearInterval(this.timer);
    document.body.classList.remove("playground-running");
    this.startButton.disabled = false;
    this.activity.disabled = false;
    this.finishButton.disabled = true;
    this.status.textContent = completed ? "Completed" : "Stopped";
    this.renderMetrics();
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

    switch (this.activity.value) {
    case "scroll-vertical": this.renderScroll("vertical"); break;
    case "scroll-horizontal": this.renderScroll("horizontal"); break;
    case "scroll-two-axis": this.renderScroll("both"); break;
    case "text-selection": this.renderText(); break;
    case "cursor-targets": this.renderTargets(false); break;
    case "cursor-precision": this.renderTargets(true); break;
    case "cursor-path": this.renderPath(); break;
    case "click-drag": this.renderDrag(); break;
    }
  }

  renderMetrics() {
    if (!this.run) return;
    const metrics = this.run.metrics;
    const elapsed = this.run.durationMs ?? Date.now() - this.run.startedMs;
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

  renderTargets(precision) {
    this.instructions.textContent = precision
      ? "Click each small target in order. Misses and pointer path length are recorded."
      : "Click each target in order. Misses, elapsed time, and pointer path length are recorded.";
    const area = element("div", `playground-pointer-area ${precision ? "precision" : ""}`);
    area.tabIndex = 0;
    area.setAttribute("aria-label", precision ? "Precision cursor targets" : "Cursor targets");
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
