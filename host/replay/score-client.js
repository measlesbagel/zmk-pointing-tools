/* SPDX-License-Identifier: MIT */

/*
 * Page-side API for scoring traces in the browser (docs/trace-replay.md,
 * "Replay in the browser"):
 *
 *   const scorer = createScoreWorker();
 *   const set = await scorer.scoreTraces(traces, profile);
 *
 * scoreTraces resolves to the shape bench.js reports for one fixture set:
 * { fixtures, scores, fixtureResults }. profile is a tuning profile whose
 * values replace the traces' recorded settings, or null for the recorded
 * settings. Requests run one at a time in a worker, in order.
 */

export function createScoreWorker(url = new URL("./score-worker.js", import.meta.url)) {
  const worker = new Worker(url, { type: "module" });
  const pending = new Map();
  let nextId = 0;
  const rejectAll = (error) => {
    for (const { reject } of pending.values()) reject(error);
    pending.clear();
  };
  worker.addEventListener("message", ({ data }) => {
    const request = pending.get(data.id);
    if (!request) return;
    pending.delete(data.id);
    if ("error" in data) request.reject(new Error(data.error));
    else request.resolve(data.result);
  });
  /* The worker failed to load or threw outside a request. */
  worker.addEventListener("error", (event) => {
    rejectAll(new Error(`score worker failed: ${event.message ?? "load error"}`));
  });
  return {
    scoreTraces(traces, profile = null) {
      return new Promise((resolve, reject) => {
        const id = nextId++;
        pending.set(id, { resolve, reject });
        worker.postMessage({ id, traces, profile });
      });
    },
    close() {
      worker.terminate();
      rejectAll(new Error("score worker closed"));
    },
  };
}
