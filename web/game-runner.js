/* Plays a target game in a surface element: shows one target at a time,
 * records the pointer path between selections, and hands back measured
 * trials. Scoring lives in measure.js. */

import {
  DIRECTION_LABELS,
  directionBin,
  measureTrial,
  sequenceThroughput,
  summarize,
  summarizeBy,
  throughput,
} from "./measure.js";
import { FLICK_BANDS, flickFlags } from "./target-games.js";

export class TargetGameRunner {
  constructor(surface, { onProgress, onFinish }) {
    this.surface = surface;
    this.onProgress = onProgress;
    this.onFinish = onFinish;
  }

  /* sequences: [{ targets: [{x, y, width, group?}], label }], in surface
   * coordinates; the first target of each sequence starts it. `analyse`
   * turns the recorded trials, one list per sequence, into result fields;
   * the default scores a throughput ring. `onFinish` replaces the
   * constructor's handler for this run only. */
  start(sequences, run, analyse = analyseRing, onFinish = this.onFinish) {
    this.stop();
    this.sequences = sequences;
    this.run = run;
    this.analyse = analyse;
    this.finished = onFinish;
    this.sequenceIndex = 0;
    this.targetIndex = 0;
    this.recorded = sequences.map(() => []);
    this.samples = [];
    this.trialStart = undefined;
    this.listeners = new AbortController();
    const { signal } = this.listeners;
    this.surface.classList.add("running");
    this.surface.addEventListener("pointermove", (event) => this.move(event), { signal });
    this.surface.addEventListener("pointerdown", (event) => this.select(event), { signal });
    this.render();
  }

  stop() {
    this.listeners?.abort();
    this.listeners = undefined;
    this.surface.classList.remove("running");
    this.surface.replaceChildren();
  }

  point(event) {
    const rect = this.surface.getBoundingClientRect();
    /* Targets are positioned inside the border, so measure from there. */
    return { x: event.clientX - rect.left - this.surface.clientLeft, y: event.clientY - rect.top - this.surface.clientTop };
  }

  move(event) {
    if (this.trialStart === undefined) return;
    /* Coalesced events keep every sensor report the browser merged into
     * one frame, which the path measures need. */
    const events = event.getCoalescedEvents?.() ?? [];
    for (const item of events.length ? events : [event]) {
      this.samples.push({ t: item.timeStamp, ...this.point(item) });
    }
  }

  select(event) {
    if (event.button !== 0) return;
    event.preventDefault();
    const sequence = this.sequences[this.sequenceIndex];
    const target = sequence.targets[this.targetIndex];
    const endpoint = this.point(event);
    if (this.targetIndex > 0) {
      const previous = sequence.targets[this.targetIndex - 1];
      this.recorded[this.sequenceIndex].push({
        start: { x: previous.x, y: previous.y },
        target: { x: target.x, y: target.y },
        width: target.width,
        ...(target.group ? { group: target.group } : {}),
        startedAt: this.trialStart,
        endedAt: event.timeStamp,
        endpoint,
        samples: [...this.samples, { t: event.timeStamp, ...endpoint }],
      });
    }
    this.samples = [{ t: event.timeStamp, ...endpoint }];
    this.trialStart = event.timeStamp;
    this.targetIndex += 1;
    if (this.targetIndex >= sequence.targets.length) {
      this.sequenceIndex += 1;
      this.targetIndex = 0;
      this.trialStart = undefined;
      if (this.sequenceIndex >= this.sequences.length) {
        this.finish();
        return;
      }
    }
    this.render();
  }

  render() {
    const sequence = this.sequences[this.sequenceIndex];
    const target = sequence.targets[this.targetIndex];
    const node = document.createElement("div");
    node.className = `game-target${this.targetIndex === 0 ? " start" : ""}`;
    node.style.left = `${target.x - target.width / 2}px`;
    node.style.top = `${target.y - target.width / 2}px`;
    node.style.width = node.style.height = `${target.width}px`;
    this.surface.replaceChildren(node);
    const total = sequence.targets.length - 1;
    this.onProgress?.(this.targetIndex === 0
      ? `${sequence.label}: click the highlighted target to begin.`
      : `${sequence.label}: target ${this.targetIndex} of ${total}.`);
  }

  finish() {
    const result = {
      ...this.run,
      finishedAt: new Date().toISOString(),
      ...this.analyse(this.recorded, this.sequences),
    };
    this.stop();
    this.finished?.(result);
  }
}

/* Ring scoring: throughput overall and per level, and the path measures
 * split by direction. */
export function analyseRing(recorded, sequences) {
  const measured = recorded.flat().map(measureTrial);
  const labels = DIRECTION_LABELS[4];
  const byDirection = summarizeBy(measured, (item) => labels[directionBin(item.direction)]);
  const bySequence = new Map(sequences.map((sequence, i) => [
    sequence.label,
    { ...summarize(recorded[i].map(measureTrial)), throughput: sequenceThroughput(recorded[i]) },
  ]));
  return {
    throughput: throughput(recorded),
    summary: summarize(measured),
    groups: {
      Direction: Object.fromEntries(byDirection),
      Level: Object.fromEntries(bySequence),
    },
    trials: recorded.map((trials) => trials.map(measureTrial)),
  };
}

/* Distance flicks scoring: the path measures per distance band, with a
 * flag for any band that did clearly worse. */
export function analyseFlicks(recorded) {
  const measured = recorded.flat().map((trial) => ({ ...measureTrial(trial), group: trial.group }));
  const byBand = summarizeBy(measured, (item) => item.group);
  const ordered = Object.fromEntries(FLICK_BANDS.filter(({ band }) => byBand.has(band)).map(({ band }) => [band, byBand.get(band)]));
  return {
    summary: summarize(measured),
    groups: { Band: ordered },
    notes: flickFlags(ordered, measured[0]?.width ?? 16),
    trials: recorded.map((trials) => trials.map((trial) => ({ ...measureTrial(trial), group: trial.group }))),
  };
}

/* Compass start: click home, a cue target appears in some direction, and
 * the trial ends as soon as the cursor has left home by `leave` pixels.
 * Only the start of each movement matters, so a wrong orientation never
 * leaves anyone chasing a target. */
export class CompassRunner {
  constructor(surface, { onProgress, onFinish }) {
    this.surface = surface;
    this.onProgress = onProgress;
    this.onFinish = onFinish;
  }

  /* cues: [{x, y}] unit screen vectors, in play order. */
  start({ home, radius, cues, leave = 40 }, run, analyse) {
    this.stop();
    Object.assign(this, { home, radius, cues, leave, run, analyse });
    this.index = 0;
    this.trials = [];
    this.samples = undefined;
    this.listeners = new AbortController();
    const { signal } = this.listeners;
    this.surface.classList.add("running");
    this.surface.addEventListener("pointermove", (event) => this.move(event), { signal });
    this.surface.addEventListener("pointerdown", (event) => this.select(event), { signal });
    this.render();
  }

  stop() {
    this.listeners?.abort();
    this.listeners = undefined;
    this.surface.classList.remove("running");
    this.surface.replaceChildren();
  }

  point(event) {
    const rect = this.surface.getBoundingClientRect();
    /* Targets are positioned inside the border, so measure from there. */
    return { x: event.clientX - rect.left - this.surface.clientLeft, y: event.clientY - rect.top - this.surface.clientTop };
  }

  select(event) {
    if (event.button !== 0 || this.samples) return;
    event.preventDefault();
    const point = this.point(event);
    if (Math.hypot(point.x - this.home.x, point.y - this.home.y) > this.home.width / 2) return;
    this.origin = point;
    this.samples = [{ t: event.timeStamp, ...point }];
    this.render();
  }

  move(event) {
    if (!this.samples) return;
    const events = event.getCoalescedEvents?.() ?? [];
    for (const item of events.length ? events : [event]) this.samples.push({ t: item.timeStamp, ...this.point(item) });
    const last = this.samples.at(-1);
    if (Math.hypot(last.x - this.origin.x, last.y - this.origin.y) < this.leave) return;
    this.trials.push({ cue: this.cues[this.index], start: this.origin, samples: this.samples });
    this.samples = undefined;
    this.index += 1;
    if (this.index >= this.cues.length) {
      const result = { ...this.run, finishedAt: new Date().toISOString(), ...this.analyse(this.trials) };
      this.stop();
      this.onFinish?.(result);
      return;
    }
    this.render();
  }

  dot(x, y, width, className) {
    const node = document.createElement("div");
    node.className = className;
    node.style.left = `${x - width / 2}px`;
    node.style.top = `${y - width / 2}px`;
    node.style.width = node.style.height = `${width}px`;
    return node;
  }

  render() {
    const { home } = this;
    if (!this.samples) {
      this.surface.replaceChildren(this.dot(home.x, home.y, home.width, "game-target start"));
      this.onProgress?.(`Cue ${this.index + 1} of ${this.cues.length}: click the centre, then roll toward the target that appears.`);
      return;
    }
    const cue = this.cues[this.index];
    this.surface.replaceChildren(
      this.dot(home.x, home.y, home.width, "game-target start"),
      this.dot(home.x + cue.x * this.radius, home.y + cue.y * this.radius, 28, "game-target"),
    );
    this.onProgress?.(`Cue ${this.index + 1} of ${this.cues.length}: roll toward the target.`);
  }
}

/* Pixel hunt: click home, then hold the pointer on a tiny target for
 * `hold` ms. A faint ring marks where the target is; only its pixels
 * count. A trial that isn't held within `timeout` ms is scored as not
 * reached. */
export class PixelRunner {
  constructor(surface, { onProgress, onFinish }) {
    this.surface = surface;
    this.onProgress = onProgress;
    this.onFinish = onFinish;
  }

  /* trials: [{ width, home, target: { left, top, width } }] from pixel.js. */
  start(trials, run, analyse, { hold = 300, timeout = 10000, onTarget }) {
    this.stop();
    Object.assign(this, { plan: trials, run, analyse, hold, timeout, onTarget });
    this.index = 0;
    this.recorded = [];
    this.trial = undefined;
    this.listeners = new AbortController();
    const { signal } = this.listeners;
    this.surface.classList.add("running");
    this.surface.addEventListener("pointermove", (event) => this.move(event), { signal });
    this.surface.addEventListener("pointerdown", (event) => this.select(event), { signal });
    this.render();
  }

  stop() {
    clearTimeout(this.holdTimer);
    clearTimeout(this.timeoutTimer);
    this.listeners?.abort();
    this.listeners = undefined;
    this.surface.classList.remove("running");
    this.surface.replaceChildren();
  }

  /* The surface can sit at a fractional page position. Measuring from the
   * nearest whole pixel, and nudging drawn targets by the same amount,
   * keeps a 1 px target on exactly the screen pixel the pointer reports. */
  origin() {
    const rect = this.surface.getBoundingClientRect();
    const left = rect.left + this.surface.clientLeft;
    const top = rect.top + this.surface.clientTop;
    return { x: Math.round(left), y: Math.round(top), dx: Math.round(left) - left, dy: Math.round(top) - top };
  }

  point(event) {
    const origin = this.origin();
    return { x: event.clientX - origin.x, y: event.clientY - origin.y };
  }

  select(event) {
    if (event.button !== 0 || this.trial) return;
    event.preventDefault();
    const point = this.point(event);
    const { home } = this.plan[this.index];
    if (Math.hypot(point.x - home.x, point.y - home.y) > home.width / 2) return;
    this.trial = { ...this.plan[this.index], startedAt: event.timeStamp, samples: [{ t: event.timeStamp, ...point }] };
    this.timeoutTimer = setTimeout(() => this.end(undefined), this.timeout);
    this.render();
  }

  move(event) {
    if (!this.trial) return;
    const events = event.getCoalescedEvents?.() ?? [];
    for (const item of events.length ? events : [event]) this.trial.samples.push({ t: item.timeStamp, ...this.point(item) });
    const last = this.trial.samples.at(-1);
    if (this.onTarget(this.trial.target, last)) {
      if (this.holdStart === undefined) {
        this.holdStart = last.t;
        this.holdTimer = setTimeout(() => this.end(this.holdStart), this.hold);
      }
    } else if (this.holdStart !== undefined) {
      clearTimeout(this.holdTimer);
      this.holdStart = undefined;
    }
  }

  end(heldAt) {
    clearTimeout(this.holdTimer);
    clearTimeout(this.timeoutTimer);
    this.holdStart = undefined;
    this.recorded.push({ ...this.trial, heldAt });
    this.trial = undefined;
    this.index += 1;
    if (this.index >= this.plan.length) {
      const result = { ...this.run, finishedAt: new Date().toISOString(), ...this.analyse(this.recorded) };
      this.stop();
      this.onFinish?.(result);
      return;
    }
    this.render();
  }

  render() {
    const { home, target, width } = this.plan[this.index];
    const { dx, dy } = this.origin();
    const node = (className, left, top, size) => {
      const element = document.createElement("div");
      element.className = className;
      element.style.left = `${left + dx}px`;
      element.style.top = `${top + dy}px`;
      element.style.width = element.style.height = `${size}px`;
      return element;
    };
    const progress = `Target ${this.index + 1} of ${this.plan.length} (${width} px)`;
    if (!this.trial) {
      this.surface.replaceChildren(node("game-target start", home.x - home.width / 2, home.y - home.width / 2, home.width));
      this.onProgress?.(`${progress}: click the centre to show the next target.`);
      return;
    }
    const cx = target.left + target.width / 2;
    const cy = target.top + target.width / 2;
    this.surface.replaceChildren(
      node("pixel-halo", cx - 14, cy - 14, 28),
      node("pixel-target", target.left, target.top, target.width),
    );
    this.onProgress?.(`${progress}: rest the pointer on the bright pixel in the ring until it counts.`);
  }
}

/* Pursuit: click the target to start, then keep the pointer on it while
 * it sweeps through each phase. Records the pointer and target once per
 * animation frame. */
export class PursuitRunner {
  constructor(surface, { onProgress, onFinish }) {
    this.surface = surface;
    this.onProgress = onProgress;
    this.onFinish = onFinish;
  }

  /* phases from pursuit.js; position(phase, elapsed) gives the target. */
  start(phases, run, analyse, { position }) {
    this.stop();
    this.phaseStart = undefined;
    Object.assign(this, { phases, run, analyse, position });
    this.index = 0;
    this.recorded = [];
    this.pointer = undefined;
    this.listeners = new AbortController();
    const { signal } = this.listeners;
    this.surface.classList.add("running");
    this.surface.addEventListener("pointermove", (event) => { this.pointer = this.point(event); }, { signal });
    this.surface.addEventListener("pointerdown", (event) => this.select(event), { signal });
    this.dot = document.createElement("div");
    this.dot.className = "game-target start";
    this.surface.replaceChildren(this.dot);
    this.place(this.position(phases[0], 0), phases[0].radius);
    this.onProgress?.("Click the target to start, then keep the pointer on it.");
  }

  stop() {
    cancelAnimationFrame(this.frame);
    this.listeners?.abort();
    this.listeners = undefined;
    this.surface.classList.remove("running");
    this.surface.replaceChildren();
  }

  point(event) {
    const rect = this.surface.getBoundingClientRect();
    return { x: event.clientX - rect.left - this.surface.clientLeft, y: event.clientY - rect.top - this.surface.clientTop };
  }

  place({ x, y }, radius) {
    this.dot.style.left = `${x - radius}px`;
    this.dot.style.top = `${y - radius}px`;
    this.dot.style.width = this.dot.style.height = `${radius * 2}px`;
  }

  select(event) {
    if (event.button !== 0 || this.phaseStart !== undefined) return;
    event.preventDefault();
    this.pointer = this.point(event);
    this.dot.className = "game-target";
    this.beginPhase();
  }

  beginPhase() {
    const phase = this.phases[this.index];
    this.samples = [];
    this.phaseStart = performance.now();
    this.onProgress?.(`Phase ${this.index + 1} of ${this.phases.length}: ${phase.variant}, ${phase.axis === "x" ? "side to side" : "up and down"}. Keep the pointer on the target.`);
    this.frame = requestAnimationFrame((now) => this.tick(now));
  }

  tick(now) {
    const phase = this.phases[this.index];
    const elapsed = now - this.phaseStart;
    const target = this.position(phase, elapsed);
    this.place(target, phase.radius);
    if (this.pointer) this.samples.push({ t: elapsed, p: { ...this.pointer }, target });
    if (elapsed < phase.duration) {
      this.frame = requestAnimationFrame((next) => this.tick(next));
      return;
    }
    this.recorded.push({ phase, samples: this.samples });
    this.index += 1;
    if (this.index < this.phases.length) {
      this.beginPhase();
      return;
    }
    const result = { ...this.run, finishedAt: new Date().toISOString(), ...this.analyse(this.recorded) };
    this.stop();
    this.onFinish?.(result);
  }
}
