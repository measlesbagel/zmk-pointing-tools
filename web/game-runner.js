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
   * the default scores a throughput ring. */
  start(sequences, run, analyse = analyseRing) {
    this.stop();
    this.sequences = sequences;
    this.run = run;
    this.analyse = analyse;
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
    return { x: event.clientX - rect.left, y: event.clientY - rect.top };
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
    this.onFinish?.(result);
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
    trials: recorded.map((trials) => trials.map(measureTrial)),
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
    return { x: event.clientX - rect.left, y: event.clientY - rect.top };
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
