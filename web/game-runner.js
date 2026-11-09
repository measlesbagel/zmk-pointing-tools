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

/* Still hands: click each target as it appears, then rest with hands off
 * the ball. Records the pointer while each button is down, after it comes
 * up, and through the rest. */
export class StillRunner {
  constructor(surface, { onProgress, onFinish }) {
    this.surface = surface;
    this.onProgress = onProgress;
    this.onFinish = onFinish;
  }

  /* targets: [{x, y, width}] to click in order; rest in ms. */
  start({ targets, home, rest = 8000 }, run, analyse) {
    this.stop();
    Object.assign(this, { targets, home, restMs: rest, run, analyse });
    this.index = 0;
    this.clicks = [];
    this.current = undefined;
    this.restSamples = undefined;
    this.listeners = new AbortController();
    const { signal } = this.listeners;
    this.surface.classList.add("running");
    this.surface.addEventListener("pointermove", (event) => this.move(event), { signal });
    this.surface.addEventListener("pointerdown", (event) => this.down(event), { signal });
    this.surface.addEventListener("pointerup", (event) => this.up(event), { signal });
    this.render();
  }

  stop() {
    clearTimeout(this.restTimer);
    clearInterval(this.countdown);
    this.listeners?.abort();
    this.listeners = undefined;
    this.surface.classList.remove("running");
    this.surface.replaceChildren();
  }

  point(event) {
    const rect = this.surface.getBoundingClientRect();
    return { t: event.timeStamp, x: event.clientX - rect.left - this.surface.clientLeft, y: event.clientY - rect.top - this.surface.clientTop };
  }

  move(event) {
    const events = event.getCoalescedEvents?.() ?? [];
    for (const item of events.length ? events : [event]) {
      const point = this.point(item);
      if (this.restSamples) this.restSamples.push(point);
      else if (this.current?.up) this.current.after.push(point);
      else if (this.current) this.current.held.push(point);
    }
  }

  down(event) {
    if (event.button !== 0) return;
    event.preventDefault();
    const point = this.point(event);
    if (this.index < this.targets.length) {
      const target = this.targets[this.index];
      if (Math.hypot(point.x - target.x, point.y - target.y) > target.width / 2) return;
      /* The previous click's after-samples end at this press. */
      if (this.current) this.clicks.push(this.current);
      this.current = { down: point, held: [], after: [] };
      return;
    }
    if (this.restSamples || Math.hypot(point.x - this.home.x, point.y - this.home.y) > this.home.width / 2) return;
    if (this.current) this.clicks.push(this.current);
    this.current = undefined;
    this.beginRest(point);
  }

  up(event) {
    if (event.button !== 0 || !this.current || this.current.up) return;
    this.current.up = this.point(event);
    this.index += 1;
    this.render();
  }

  beginRest(point) {
    this.restSamples = [point];
    const ends = point.t + this.restMs;
    this.surface.replaceChildren();
    const tick = () => this.onProgress?.(`Hands off the ball and the keys: ${Math.max(0, Math.ceil((ends - performance.now()) / 1000))} s.`);
    tick();
    this.countdown = setInterval(tick, 250);
    this.restTimer = setTimeout(() => {
      clearInterval(this.countdown);
      const result = { ...this.run, finishedAt: new Date().toISOString(), ...this.analyse(this.clicks, this.restSamples) };
      this.stop();
      this.onFinish?.(result);
    }, this.restMs);
  }

  render() {
    const dot = ({ x, y, width }, className) => {
      const node = document.createElement("div");
      node.className = className;
      node.style.left = `${x - width / 2}px`;
      node.style.top = `${y - width / 2}px`;
      node.style.width = node.style.height = `${width}px`;
      return node;
    };
    if (this.index < this.targets.length) {
      this.surface.replaceChildren(dot(this.targets[this.index], "game-target"));
      this.onProgress?.(`Click ${this.index + 1} of ${this.targets.length}: click each target as soon as it appears, with your usual button.`);
      return;
    }
    this.surface.replaceChildren(dot(this.home, "game-target start"));
    this.onProgress?.("Last part: click the centre, then take your hands off the ball and keys for a few seconds.");
  }
}

/* Scroll to the band: the wheel moves a ruled page under a fixed band in
 * the middle of the surface. A trial starts at its first wheel event and
 * ends once the target line has stayed in the band for `hold` ms. */
export class ScrollBandRunner {
  constructor(surface, { onProgress, onFinish }) {
    this.surface = surface;
    this.onProgress = onProgress;
    this.onFinish = onFinish;
  }

  start(trials, run, analyse, { hold = 400, timeout = 15000 } = {}) {
    this.stop();
    Object.assign(this, { plan: trials, run, analyse, hold, timeout });
    this.index = 0;
    this.page = 0;
    this.recorded = [];
    this.listeners = new AbortController();
    const { signal } = this.listeners;
    this.surface.classList.add("running", "scrolling");
    this.surface.addEventListener("wheel", (event) => this.wheel(event), { signal, passive: false });
    this.ruler = document.createElement("div");
    this.ruler.className = "scroll-page";
    this.bandNode = document.createElement("div");
    this.bandNode.className = "scroll-band";
    this.targetNode = document.createElement("div");
    this.targetNode.className = "scroll-target";
    this.hint = document.createElement("div");
    this.hint.className = "scroll-hint";
    this.surface.replaceChildren(this.ruler, this.bandNode, this.targetNode, this.hint);
    this.begin();
  }

  stop() {
    clearTimeout(this.holdTimer);
    clearTimeout(this.timeoutTimer);
    this.listeners?.abort();
    this.listeners = undefined;
    this.surface.classList.remove("running", "scrolling");
    this.surface.replaceChildren();
  }

  begin() {
    const trial = this.plan[this.index];
    this.trial = { ...trial, origin: this.page, events: [], shownAt: performance.now() };
    this.bandNode.style.height = `${trial.band}px`;
    this.timeoutTimer = setTimeout(() => this.end(undefined), this.timeout);
    this.onProgress?.(`Scroll ${this.index + 1} of ${this.plan.length} (${trial.known ? "target shown" : "target hidden until it comes into view"}): bring the line into the band and hold it there.`);
    this.render();
  }

  wheel(event) {
    event.preventDefault();
    if (!this.trial) return;
    const scale = event.deltaMode === 1 ? 40 : event.deltaMode === 2 ? this.surface.clientHeight : 1;
    const delta = event.deltaY * scale;
    this.page += delta;
    const trial = this.trial;
    const offset = this.page - trial.origin;
    trial.events.push({ t: event.timeStamp, delta, offset });
    const position = offset * Math.sign(trial.distance);
    const inside = Math.abs(position - Math.abs(trial.distance)) <= trial.band / 2;
    if (inside && this.holdStart === undefined) {
      this.holdStart = event.timeStamp;
      this.holdTimer = setTimeout(() => this.end(this.holdStart), this.hold);
    } else if (!inside && this.holdStart !== undefined) {
      clearTimeout(this.holdTimer);
      this.holdStart = undefined;
    }
    this.render();
  }

  end(heldAt) {
    clearTimeout(this.holdTimer);
    clearTimeout(this.timeoutTimer);
    this.holdStart = undefined;
    const { known, distance, band, events, shownAt } = this.trial;
    this.recorded.push({ known, distance, band, events, startedAt: events[0]?.t ?? shownAt, heldAt });
    this.trial = undefined;
    this.index += 1;
    if (this.index >= this.plan.length) {
      const result = { ...this.run, finishedAt: new Date().toISOString(), ...this.analyse(this.recorded) };
      this.stop();
      this.onFinish?.(result);
      return;
    }
    this.begin();
  }

  render() {
    const height = this.surface.clientHeight;
    const centre = height / 2;
    this.ruler.style.backgroundPositionY = `${-this.page}px`;
    this.bandNode.style.top = `${centre - this.trial.band / 2}px`;
    /* Where the target line is on screen: it sits `distance` below where
     * the page was when the trial began. */
    const y = centre + this.trial.distance - (this.page - this.trial.origin);
    const visible = y >= 0 && y <= height;
    this.targetNode.hidden = !visible;
    this.targetNode.style.top = `${y}px`;
    const remaining = Math.round(y - centre);
    this.hint.hidden = visible || !this.trial.known;
    this.hint.textContent = remaining > 0 ? `↓ ${remaining} px` : `↑ ${-remaining} px`;
    this.hint.style.top = remaining > 0 ? "" : "8px";
    this.hint.style.bottom = remaining > 0 ? "8px" : "";
  }
}

/* Rail and corner: the wheel moves a page with a corridor drawn on it
 * under a fixed dot. A course starts at its first wheel event and ends
 * when the dot reaches the far end. */
export class RailRunner {
  constructor(surface, { onProgress, onFinish }) {
    this.surface = surface;
    this.onProgress = onProgress;
    this.onFinish = onFinish;
  }

  /* courses from rail.js; finished(course, point) says when one is done. */
  start(courses, run, analyse, { width, timeout = 20000, finished }) {
    this.stop();
    Object.assign(this, { courses, run, analyse, width, timeout, finished });
    this.index = 0;
    this.recorded = [];
    this.listeners = new AbortController();
    const { signal } = this.listeners;
    this.surface.classList.add("running", "scrolling");
    this.surface.addEventListener("wheel", (event) => this.wheel(event), { signal, passive: false });
    this.begin();
  }

  stop() {
    clearTimeout(this.timeoutTimer);
    this.listeners?.abort();
    this.listeners = undefined;
    this.surface.classList.remove("running", "scrolling");
    this.surface.replaceChildren();
  }

  begin() {
    const course = this.courses[this.index];
    this.position = { x: 0, y: 0 };
    this.events = [];
    this.page = document.createElement("div");
    this.page.className = "rail-page";
    let at = { x: 0, y: 0 };
    const half = this.width / 2;
    for (const leg of course.legs) {
      const end = { x: at.x + leg.dx * leg.length, y: at.y + leg.dy * leg.length };
      const segment = document.createElement("div");
      segment.className = "rail-segment";
      const left = Math.min(at.x, end.x) - half;
      const top = Math.min(at.y, end.y) - half;
      segment.style.left = `${left}px`;
      segment.style.top = `${top}px`;
      segment.style.width = `${Math.abs(end.x - at.x) + this.width}px`;
      segment.style.height = `${Math.abs(end.y - at.y) + this.width}px`;
      this.page.append(segment);
      at = end;
    }
    const goal = document.createElement("div");
    goal.className = "rail-goal";
    goal.style.left = `${at.x - half}px`;
    goal.style.top = `${at.y - half}px`;
    goal.style.width = goal.style.height = `${this.width}px`;
    this.page.append(goal);
    const dot = document.createElement("div");
    dot.className = "rail-dot";
    this.surface.replaceChildren(this.page, dot);
    this.timeoutTimer = setTimeout(() => this.end(undefined), this.timeout);
    this.onProgress?.(`Rail ${this.index + 1} of ${this.courses.length} (${course.name}): scroll the dot along the track to the end, staying inside it.`);
    this.render();
  }

  wheel(event) {
    event.preventDefault();
    const scale = event.deltaMode === 1 ? 40 : event.deltaMode === 2 ? this.surface.clientHeight : 1;
    const dx = event.deltaX * scale;
    const dy = event.deltaY * scale;
    this.position = { x: this.position.x + dx, y: this.position.y + dy };
    this.events.push({ t: event.timeStamp, dx, dy, ...this.position });
    this.render();
    if (this.finished(this.courses[this.index], this.position)) this.end(event.timeStamp);
  }

  end(endedAt) {
    clearTimeout(this.timeoutTimer);
    this.recorded.push({ course: this.courses[this.index], events: this.events, endedAt });
    this.index += 1;
    if (this.index >= this.courses.length) {
      const result = { ...this.run, finishedAt: new Date().toISOString(), ...this.analyse(this.recorded) };
      this.stop();
      this.onFinish?.(result);
      return;
    }
    this.begin();
  }

  render() {
    const cx = this.surface.clientWidth / 2;
    const cy = this.surface.clientHeight / 2;
    this.page.style.transform = `translate(${cx - this.position.x}px, ${cy - this.position.y}px)`;
  }
}
