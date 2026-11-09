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

export class TargetGameRunner {
  constructor(surface, { onProgress, onFinish }) {
    this.surface = surface;
    this.onProgress = onProgress;
    this.onFinish = onFinish;
  }

  /* sequences: [{ targets: [{x, y, width}], label }], in surface
   * coordinates; the first target of each sequence starts it. */
  start(sequences, run) {
    this.stop();
    this.sequences = sequences;
    this.run = run;
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
    const measured = this.recorded.flat().map(measureTrial);
    const labels = DIRECTION_LABELS[4];
    const byDirection = summarizeBy(measured, (item) => labels[directionBin(item.direction)]);
    const bySequence = new Map(this.sequences.map((sequence, i) => [
      sequence.label,
      { ...summarize(this.recorded[i].map(measureTrial)), throughput: sequenceThroughput(this.recorded[i]) },
    ]));
    const result = {
      ...this.run,
      finishedAt: new Date().toISOString(),
      throughput: throughput(this.recorded),
      summary: summarize(measured),
      groups: {
        Direction: Object.fromEntries(byDirection),
        Level: Object.fromEntries(bySequence),
      },
      trials: this.recorded.map((trials) => trials.map(measureTrial)),
    };
    this.stop();
    this.onFinish?.(result);
  }
}
