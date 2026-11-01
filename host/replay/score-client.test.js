import test from "node:test";
import assert from "node:assert/strict";

import { createScoreWorker } from "./score-client.js";

/* A stand-in for the browser Worker: postMessage clones its message as a
 * real worker does, and the test answers requests or fails the worker. */
class FakeWorker {
  static last = null;

  constructor() {
    this.listeners = { message: [], error: [] };
    this.requests = [];
    this.terminated = false;
    FakeWorker.last = this;
  }

  addEventListener(type, listener) {
    this.listeners[type].push(listener);
  }

  postMessage(message) {
    this.requests.push(structuredClone(message));
  }

  terminate() {
    this.terminated = true;
  }

  reply(data) {
    for (const listener of this.listeners.message) listener({ data });
  }

  fail(message) {
    for (const listener of this.listeners.error) listener({ message });
  }
}

function scorer() {
  globalThis.Worker = FakeWorker;
  return { scorer: createScoreWorker("score-worker.js"), worker: FakeWorker.last };
}

test("requests resolve and reject with the worker's answer for their id", async () => {
  const { scorer: client, worker } = scorer();
  const first = client.scoreTraces([{ id: "a" }]);
  const second = client.scoreTraces([{ id: "b" }], { targets: [] });
  assert.deepEqual(worker.requests.map(({ id, profile }) => [id, profile]),
    [[0, null], [1, { targets: [] }]]);
  worker.reply({ id: 1, error: "trace b: events are required" });
  worker.reply({ id: 0, result: { fixtures: 1 } });
  assert.deepEqual(await first, { fixtures: 1 });
  await assert.rejects(second, /trace b: events are required/);
});

test("close rejects pending and later requests instead of leaving them unsettled", async () => {
  const { scorer: client, worker } = scorer();
  const pending = client.scoreTraces([{ id: "a" }]);
  client.close();
  assert.equal(worker.terminated, true);
  await assert.rejects(pending, /score worker closed/);
  await assert.rejects(client.scoreTraces([{ id: "b" }]), /score worker closed/);
  assert.equal(worker.requests.length, 1);
});

test("a worker load error rejects pending and every later request", async () => {
  const { scorer: client, worker } = scorer();
  const pending = client.scoreTraces([{ id: "a" }]);
  worker.fail("");
  await assert.rejects(pending, /score worker failed: load error/);
  await assert.rejects(client.scoreTraces([{ id: "b" }]), /score worker failed: load error/);
  await assert.rejects(client.scoreTraces([{ id: "c" }]), /score worker failed: load error/);
});

test("a message that can't be cloned rejects without blocking later requests", async () => {
  const { scorer: client, worker } = scorer();
  await assert.rejects(client.scoreTraces([{ id: "a", label: () => {} }]),
    { name: "DataCloneError" });
  const next = client.scoreTraces([{ id: "b" }]);
  worker.reply({ id: worker.requests[0].id, result: { fixtures: 1 } });
  assert.deepEqual(await next, { fixtures: 1 });
  client.close();
});
