/* SPDX-License-Identifier: MIT */

import {
  CURVE_POINTS,
  SHAPES,
  curveBatch,
  curvePoints,
  gainFunction,
  samePoints,
  sampleShape,
  sensitivityFunction,
  shapeDefaults,
  validatePoints,
} from "./acceleration-curve.js";

/* Graphical editor for acceleration curve targets. Edits stay in a draft
 * until Preview sends the whole curve as one atomic batch, so the keyboard
 * never sees half a curve. */

/* The chart is drawn at its on-screen width, so text keeps its size on a
 * phone instead of shrinking with a scaled-down drawing. */
const MAX_WIDTH = 960;
const MIN_WIDTH = 280;
const MARGIN = { left: 48, right: 14, top: 24, bottom: 38 };
const LOG_MIN = 2;
const LOG_TICKS = [2, 5, 10, 20, 50, 100, 200, 500, 1000, 2000, 5000, 10000];
const SAMPLES = 240;
/* Rough trackball speed bands, to read the chart by feel. */
const BANDS = [
  { from: 0, to: 15, label: "aim" },
  { from: 15, to: 80, label: "steer" },
  { from: 80, to: 200, label: "travel" },
  { from: 200, to: Infinity, label: "flick" },
];

function makeScale(min, max, r0, r1, log) {
  if (log) {
    const a = Math.log(min);
    const b = Math.log(max);
    const f = (v) => r0 + ((Math.log(Math.max(v, min)) - a) / (b - a)) * (r1 - r0);
    f.inv = (p) => Math.exp(a + ((p - r0) / (r1 - r0)) * (b - a));
    return f;
  }
  const f = (v) => r0 + ((v - min) / (max - min)) * (r1 - r0);
  f.inv = (p) => min + ((p - r0) / (r1 - r0)) * (max - min);
  return f;
}

function niceStep(span, target) {
  const raw = span / target;
  const power = 10 ** Math.floor(Math.log10(raw));
  const n = raw / power;
  return (n < 1.5 ? 1 : n < 3 ? 2 : n < 7 ? 5 : 10) * power;
}

function linearTicks(min, max, target) {
  const step = niceStep(max - min, target);
  const ticks = [];
  for (let v = Math.ceil(min / step) * step; v <= max + 1e-9; v += step) ticks.push(Math.round(v * 1e6) / 1e6);
  return ticks;
}

const niceCeil = (value, step) => Math.ceil(value / step) * step;
const clamp = (value, low, high) => Math.min(Math.max(value, low), high);

function path(points) {
  return points.map(([x, y], i) => `${i ? "L" : "M"}${x.toFixed(1)} ${y.toFixed(1)}`).join("");
}

export class CurveEditors {
  /* getCurve(targetId) returns the target's live curve parameters (see
   * findCurve); onPreview(targetId, values) sends a batch; onRender()
   * redraws the whole tuning list. */
  constructor({ escapeHtml, getCurve, onPreview, onRender }) {
    this.escapeHtml = escapeHtml;
    this.getCurve = getCurve;
    this.onPreview = onPreview;
    this.onRender = onRender;
    this.editors = new Map();
    this.geometry = new Map();
    this.drag = null;
    this.renderPending = false;
    this.root = null;
    this.size = { width: MAX_WIDTH, height: 320 };
  }

  /* Size the chart to the space the tuning list leaves it: the list's
   * width less the target's and the editor's padding and borders. */
  measure() {
    const available = (this.root?.clientWidth ?? MAX_WIDTH) - 56;
    const width = Math.round(clamp(available, MIN_WIDTH, MAX_WIDTH));
    this.size = { width, height: width < 480 ? 250 : 320 };
  }

  get dragging() {
    return this.drag !== null;
  }

  editor(targetId) {
    let editor = this.editors.get(targetId);
    if (!editor) {
      editor = { draft: null, shape: "custom", shapeValues: {}, view: "sensitivity", log: false };
      this.editors.set(targetId, editor);
    }
    return editor;
  }

  points(targetId, curve) {
    return this.editor(targetId).draft ?? curvePoints(curve);
  }

  /* Firmware reported new values: a draft that now matches them is done. */
  settle(targetId, curve) {
    const editor = this.editors.get(targetId);
    if (editor?.draft && samePoints(editor.draft, curvePoints(curve))) editor.draft = null;
  }

  discard(targetId) {
    const editor = this.editors.get(targetId);
    if (editor) {
      editor.draft = null;
      editor.shape = "custom";
    }
  }

  discardAll() {
    for (const targetId of this.editors.keys()) this.discard(targetId);
  }

  status(targetId, curve) {
    const points = this.points(targetId, curve);
    const invalid = validatePoints(points, curve);
    if (invalid) return { text: invalid, warning: true, canPreview: false };
    const edited = !samePoints(points, curvePoints(curve));
    if (edited) return { text: "Edited here, not on the keyboard yet.", warning: true, canPreview: true };
    const compiled = samePoints(points, curvePoints(curve, "compiled"));
    return {
      text: compiled ? "Matches the keyboard and its compiled curve." : "Matches the keyboard (modified from compiled).",
      warning: false,
      canPreview: false,
    };
  }

  render(targetId, curve) {
    const esc = this.escapeHtml;
    const editor = this.editor(targetId);
    const points = this.points(targetId, curve);
    const status = this.status(targetId, curve);
    const shape = SHAPES.find(({ id }) => id === editor.shape);
    const shapeControls = shape
      ? `<div class="curve-shape">
          <p class="muted">${esc(shape.summary)} Sampled into the eight points at ${shape.speeds ? "speeds that follow its options" : "fixed speeds"}.</p>
          <div class="curve-options">${shape.options.map((option) => {
            const value = editor.shapeValues[option.id] ?? option.value;
            return `<label title="${esc(option.help)}"><span>${esc(option.label)} <output data-curve-option-value="${option.id}">${value}${option.unit ? ` ${esc(option.unit)}` : ""}</output></span>
              <input type="range" data-curve-option="${option.id}" min="${option.min}" max="${option.max}" step="${option.step}" value="${value}"></label>`;
          }).join("")}</div>
        </div>`
      : "";
    const pointInputs = points.map((point, i) => {
      const speed = curve.speeds[i];
      const sensitivity = curve.sensitivities[i];
      return `<div class="curve-point">
        <span>Point ${i + 1}</span>
        <label>mm/s <input type="number" data-curve-speed="${i}" value="${point.speed}" min="${speed.minimum}" max="${speed.maximum}" step="1"></label>
        <label>% <input type="number" data-curve-percent="${i}" value="${point.percent}" min="${sensitivity.minimum}" max="${sensitivity.maximum}" step="1"></label>
      </div>`;
    }).join("");
    if (!this.drag) this.measure();
    const view = (id, label) =>
      `<button type="button" data-curve-view="${id}" aria-pressed="${editor.view === id}">${label}</button>`;

    return `<div class="curve-editor" data-curve-target="${targetId}">
      <div class="curve-toolbar">
        <label>Shape
          <select data-curve-shape>
            <option value="custom" ${shape ? "" : "selected"}>Custom points</option>
            ${SHAPES.map(({ id, label }) => `<option value="${id}" ${editor.shape === id ? "selected" : ""}>${esc(label)}</option>`).join("")}
          </select>
        </label>
        <div class="curve-views" role="group" aria-label="Chart">${view("sensitivity", "Sensitivity")}${view("gain", "Gain")}</div>
        <label class="curve-check"><input type="checkbox" data-curve-log ${editor.log ? "checked" : ""}> Log speed axis</label>
      </div>
      ${shapeControls}
      <svg class="curve-chart" data-curve-chart viewBox="0 0 ${this.size.width} ${this.size.height}" role="img" aria-label="Acceleration curve">${this.chart(targetId, curve)}</svg>
      <div class="curve-legend">
        <span><i class="swatch draft"></i>Editing</span>
        <span><i class="swatch current"></i>On keyboard</span>
        <span><i class="swatch compiled"></i>Compiled</span>
      </div>
      <p class="curve-readout muted" data-curve-readout>${editor.view === "sensitivity"
        ? "Sensitivity multiplies motion at each ball speed, on top of the base gain. Drag a point, or focus it and use the arrow keys (Shift for steps of 10)."
        : "Gain is how fast cursor speed grows with ball speed; a jump in it feels like a bump mid-motion. Switch to Sensitivity to drag points."}</p>
      <div class="curve-points">${pointInputs}</div>
      <div class="curve-actions">
        <span class="curve-status ${status.warning ? "warning" : ""}" data-curve-status>${esc(status.text)}</span>
        <button type="button" data-curve-compiled>Load compiled</button>
        <button type="button" data-curve-discard ${status.canPreview || editor.draft ? "" : "disabled"}>Discard edits</button>
        <button type="button" class="primary" data-curve-preview ${status.canPreview ? "" : "disabled"}>Preview curve</button>
      </div>
    </div>`;
  }

  chart(targetId, curve) {
    const editor = this.editor(targetId);
    const points = this.points(targetId, curve);
    const valid = validatePoints(points, curve) === null;
    const current = curvePoints(curve);
    const compiled = curvePoints(curve, "compiled");
    const gain = editor.view === "gain";
    const value = (pointSet) => (gain ? gainFunction(pointSet) : sensitivityFunction(pointSet));

    const { width, height } = this.size;
    const plot = { left: MARGIN.left, right: width - MARGIN.right, top: MARGIN.top, bottom: height - MARGIN.bottom };
    const frozen = this.drag?.targetId === targetId ? this.drag.geometry : null;
    /* A half-typed point may not be a number yet; draw around it. */
    const lastSpeed = Math.max(...[points, current, compiled].map((set) => set[CURVE_POINTS - 1].speed)
      .filter(Number.isFinite));
    const xMax = frozen?.xMax ?? niceCeil(Math.max(500, lastSpeed * 1.25), 100);
    const xMin = editor.log ? LOG_MIN : 0;
    const x = frozen?.x ?? makeScale(xMin, xMax, plot.left, plot.right, editor.log);

    const series = [
      { name: "compiled", points: compiled, show: !samePoints(compiled, current) },
      { name: "current", points: current, show: !samePoints(current, points) || !valid },
      { name: "draft", points, show: valid },
    ].filter(({ show }) => show).map((entry) => {
      const fn = value(entry.points);
      const samples = [];
      for (let i = 0; i <= SAMPLES; i += 1) {
        const px = plot.left + ((plot.right - plot.left) * i) / SAMPLES;
        const speed = x.inv(px);
        samples.push([px, speed, fn(speed)]);
      }
      return { ...entry, samples };
    });

    const highest = Math.max(
      ...series.flatMap(({ samples }) => samples.map((sample) => sample[2])),
      ...points.map(({ percent }) => (gain || !Number.isFinite(percent) ? 0 : percent)),
    );
    const yMax = frozen?.yMax ?? niceCeil(Math.max(150, highest * 1.1), 50);
    const y = frozen?.y ?? makeScale(0, yMax, plot.bottom, plot.top, false);
    this.geometry.set(targetId, { x, y, xMax, yMax, plot, log: editor.log });

    const xTicks = editor.log
      ? LOG_TICKS.filter((v) => v >= xMin && v <= xMax && (width >= 480 || ![5, 20, 200, 2000].includes(v)))
      : linearTicks(0, xMax, width >= 480 ? 6 : 4);
    const yTicks = linearTicks(0, yMax, height >= 320 ? 5 : 4);
    const grid = [
      ...yTicks.map((v) => `<line class="grid" x1="${plot.left}" x2="${plot.right}" y1="${y(v).toFixed(1)}" y2="${y(v).toFixed(1)}"/>
        <text class="tick" x="${plot.left - 6}" y="${(y(v) + 4).toFixed(1)}" text-anchor="end">${v}</text>`),
      ...xTicks.map((v) => `<line class="grid" x1="${x(v).toFixed(1)}" x2="${x(v).toFixed(1)}" y1="${plot.top}" y2="${plot.bottom}"/>
        <text class="tick" x="${x(v).toFixed(1)}" y="${plot.bottom + 16}" text-anchor="middle">${v}</text>`),
    ].join("");
    const bands = BANDS.map((band) => {
      const from = x(Math.max(band.from, xMin));
      const to = x(Math.min(band.to, xMax));
      if (to - from < 30) return "";
      return `${band.from > xMin ? `<line class="band" x1="${from.toFixed(1)}" x2="${from.toFixed(1)}" y1="${plot.top - 14}" y2="${plot.bottom}"/>` : ""}
        <text class="band-label" x="${((from + to) / 2).toFixed(1)}" y="${plot.top - 8}" text-anchor="middle">${band.label}</text>`;
    }).join("");
    const reference = `<line class="reference" x1="${plot.left}" x2="${plot.right}" y1="${y(100).toFixed(1)}" y2="${y(100).toFixed(1)}"/>`;
    const lines = series.map(({ name, samples }) =>
      `<path class="curve ${name}" d="${path(samples.map(([px, , v]) => [px, y(clamp(v, 0, yMax))]))}"/>`).join("");
    const draftValue = value(valid ? points : current);
    const markers = points.map((point, i) => {
      if (!Number.isFinite(point.speed) || !Number.isFinite(point.percent)) return "";
      const cx = x(clamp(point.speed, xMin, xMax));
      const cy = y(clamp(gain ? draftValue(point.speed) : point.percent, 0, yMax));
      if (gain) return `<circle class="marker" cx="${cx.toFixed(1)}" cy="${cy.toFixed(1)}" r="3.5"/>`;
      const active = this.drag?.targetId === targetId && this.drag.index === i;
      return `<g class="point ${active ? "active" : ""}" data-curve-point="${i}" tabindex="0" role="slider"
          aria-label="Point ${i + 1}" aria-valuetext="${point.speed} mm/s, ${point.percent} %">
        <circle class="hit" cx="${cx.toFixed(1)}" cy="${cy.toFixed(1)}" r="14"/>
        <circle cx="${cx.toFixed(1)}" cy="${cy.toFixed(1)}" r="6.5"/>
        <text x="${cx.toFixed(1)}" y="${(cy - 11).toFixed(1)}" text-anchor="middle">${i + 1}</text>
      </g>`;
    }).join("");
    const axes = `<text class="axis" x="${(plot.left + plot.right) / 2}" y="${height - 4}" text-anchor="middle">Ball speed (mm/s)</text>
      <text class="axis" transform="translate(13 ${(plot.top + plot.bottom) / 2}) rotate(-90)" text-anchor="middle">${gain ? "Gain" : "Sensitivity"} (%)</text>`;
    return `${grid}${bands}${reference}${lines}${markers}${axes}`;
  }

  /* Redraw one editor's chart and status in place; inputs too unless the
   * user is typing in them. */
  refresh(root, targetId, updateInputs) {
    const curve = this.getCurve(targetId);
    const container = root.querySelector(`[data-curve-target="${targetId}"]`);
    if (!curve || !container) return;
    const points = this.points(targetId, curve);
    container.querySelector("[data-curve-chart]").innerHTML = this.chart(targetId, curve);
    const status = this.status(targetId, curve);
    const statusElement = container.querySelector("[data-curve-status]");
    statusElement.textContent = status.text;
    statusElement.classList.toggle("warning", status.warning);
    container.querySelector("[data-curve-preview]").disabled = !status.canPreview;
    container.querySelector("[data-curve-discard]").disabled = !(status.canPreview || this.editor(targetId).draft);
    container.querySelector("[data-curve-shape]").value = this.editor(targetId).shape;
    if (updateInputs) {
      points.forEach((point, i) => {
        container.querySelector(`[data-curve-speed="${i}"]`).value = point.speed;
        container.querySelector(`[data-curve-percent="${i}"]`).value = point.percent;
      });
    }
  }

  editDraft(targetId, curve) {
    const editor = this.editor(targetId);
    editor.draft = (editor.draft ?? curvePoints(curve)).map((point) => ({ ...point }));
    return editor;
  }

  /* Move point index within its neighbours and the parameter ranges. */
  movePoint(targetId, curve, index, speed, percent) {
    const editor = this.editDraft(targetId, curve);
    editor.shape = "custom";
    const points = editor.draft;
    const low = index === 0 ? curve.speeds[0].minimum : points[index - 1].speed + 1;
    const high = index === CURVE_POINTS - 1 ? curve.speeds[index].maximum : points[index + 1].speed - 1;
    points[index] = {
      speed: clamp(Math.round(speed), low, Math.max(low, high)),
      percent: clamp(Math.round(percent), curve.sensitivities[index].minimum, curve.sensitivities[index].maximum),
    };
    return points[index];
  }

  readout(root, targetId, index) {
    const curve = this.getCurve(targetId);
    const element = root.querySelector(`[data-curve-target="${targetId}"] [data-curve-readout]`);
    if (!curve || !element) return;
    const point = this.points(targetId, curve)[index];
    element.textContent = `Point ${index + 1}: ${point.speed} mm/s at ${point.percent} %`;
  }

  targetOf(element) {
    const container = element.closest("[data-curve-target]");
    return container ? Number(container.dataset.curveTarget) : undefined;
  }

  bind(root) {
    this.root = root;
    let resized;
    window.addEventListener("resize", () => {
      clearTimeout(resized);
      resized = setTimeout(() => {
        if (this.editors.size && !this.drag) this.onRender();
      }, 150);
    });
    root.addEventListener("pointerdown", (event) => {
      const handle = event.target.closest("[data-curve-point]");
      if (!handle) return;
      const targetId = this.targetOf(handle);
      const svg = handle.closest("[data-curve-chart]");
      this.drag = {
        targetId,
        index: Number(handle.dataset.curvePoint),
        pointerId: event.pointerId,
        svg,
        geometry: this.geometry.get(targetId),
      };
      svg.setPointerCapture(event.pointerId);
      event.preventDefault();
      this.readout(root, targetId, this.drag.index);
    });
    root.addEventListener("pointermove", (event) => {
      const drag = this.drag;
      if (!drag || event.pointerId !== drag.pointerId) return;
      const curve = this.getCurve(drag.targetId);
      if (!curve) return;
      const rect = drag.svg.getBoundingClientRect();
      const px = clamp((event.clientX - rect.left) * (this.size.width / rect.width), drag.geometry.plot.left, drag.geometry.plot.right);
      const py = clamp((event.clientY - rect.top) * (this.size.height / rect.height), drag.geometry.plot.top, drag.geometry.plot.bottom);
      let speed = drag.geometry.x.inv(px);
      /* A log axis can't show 0, so the first point snaps back to its
       * minimum at the left edge. */
      if (drag.geometry.log && drag.index === 0 && speed < LOG_MIN * 1.25) speed = curve.speeds[0].minimum;
      this.movePoint(drag.targetId, curve, drag.index, speed, drag.geometry.y.inv(py));
      this.refresh(root, drag.targetId, true);
      this.readout(root, drag.targetId, drag.index);
    });
    const endDrag = (event) => {
      if (!this.drag || event.pointerId !== this.drag.pointerId) return;
      const { targetId } = this.drag;
      this.drag = null;
      if (this.renderPending) {
        this.renderPending = false;
        this.onRender();
      } else {
        this.refresh(root, targetId, true);
      }
    };
    root.addEventListener("pointerup", endDrag);
    root.addEventListener("pointercancel", endDrag);

    root.addEventListener("keydown", (event) => {
      const handle = event.target.closest("[data-curve-point]");
      const steps = { ArrowUp: [0, 1], ArrowDown: [0, -1], ArrowRight: [1, 0], ArrowLeft: [-1, 0] };
      if (!handle || !steps[event.key]) return;
      event.preventDefault();
      const targetId = this.targetOf(handle);
      const curve = this.getCurve(targetId);
      if (!curve) return;
      const index = Number(handle.dataset.curvePoint);
      const size = event.shiftKey ? 10 : 1;
      const point = this.points(targetId, curve)[index];
      const [dx, dy] = steps[event.key];
      this.movePoint(targetId, curve, index, point.speed + dx * size, point.percent + dy * size);
      this.refresh(root, targetId, true);
      this.readout(root, targetId, index);
      root.querySelector(`[data-curve-target="${targetId}"] [data-curve-point="${index}"]`)?.focus();
    });

    root.addEventListener("input", (event) => {
      const input = event.target;
      const targetId = this.targetOf(input);
      if (targetId === undefined) return;
      const curve = this.getCurve(targetId);
      if (!curve) return;
      if (input.matches("[data-curve-speed], [data-curve-percent]")) {
        const editor = this.editDraft(targetId, curve);
        editor.shape = "custom";
        const index = Number(input.dataset.curveSpeed ?? input.dataset.curvePercent);
        const field = input.matches("[data-curve-speed]") ? "speed" : "percent";
        editor.draft[index] = { ...editor.draft[index], [field]: input.value === "" ? NaN : Number(input.value) };
        this.refresh(root, targetId, false);
      } else if (input.matches("[data-curve-option]")) {
        const editor = this.editor(targetId);
        const shape = SHAPES.find(({ id }) => id === editor.shape);
        if (!shape) return;
        editor.shapeValues = { ...shapeDefaults(shape), ...editor.shapeValues, [input.dataset.curveOption]: Number(input.value) };
        editor.draft = sampleShape(shape, editor.shapeValues, curve.sensitivities[0].maximum);
        const option = shape.options.find(({ id }) => id === input.dataset.curveOption);
        const output = input.closest("label").querySelector("output");
        output.textContent = `${input.value}${option.unit ? ` ${option.unit}` : ""}`;
        this.refresh(root, targetId, true);
      }
    });

    root.addEventListener("change", (event) => {
      const element = event.target;
      const targetId = this.targetOf(element);
      if (targetId === undefined) return;
      const curve = this.getCurve(targetId);
      if (!curve) return;
      const editor = this.editor(targetId);
      if (element.matches("[data-curve-shape]")) {
        const shape = SHAPES.find(({ id }) => id === element.value);
        editor.shape = shape ? shape.id : "custom";
        if (shape) {
          editor.shapeValues = shapeDefaults(shape);
          editor.draft = sampleShape(shape, editor.shapeValues, curve.sensitivities[0].maximum);
        }
        this.onRender();
      } else if (element.matches("[data-curve-log]")) {
        editor.log = element.checked;
        this.onRender();
      }
    });

    root.addEventListener("click", (event) => {
      const button = event.target.closest("[data-curve-view], [data-curve-preview], [data-curve-discard], [data-curve-compiled]");
      if (!button) return;
      const targetId = this.targetOf(button);
      const curve = this.getCurve(targetId);
      if (!curve) return;
      const editor = this.editor(targetId);
      if (button.matches("[data-curve-view]")) {
        editor.view = button.dataset.curveView;
      } else if (button.matches("[data-curve-discard]")) {
        this.discard(targetId);
      } else if (button.matches("[data-curve-compiled]")) {
        editor.draft = curvePoints(curve, "compiled");
        editor.shape = "custom";
      } else if (button.matches("[data-curve-preview]")) {
        const points = this.points(targetId, curve);
        if (validatePoints(points, curve) !== null) return;
        this.onPreview(targetId, curveBatch(curve, points));
        return;
      }
      this.onRender();
    });
  }
}
