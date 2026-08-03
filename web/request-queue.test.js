import test from "node:test";
import assert from "node:assert/strict";

import { ResponseRequestQueue } from "./request-queue.js";

test("sends one request at a time and waits for its matching response", () => {
  const sent = [];
  const queue = new ResponseRequestQueue((frame) => sent.push(frame));

  queue.enqueue(0x83, "targets");
  queue.enqueue(0x84, "description");
  queue.enqueue(0x86, "help");
  assert.deepEqual(sent, ["targets"]);
  assert.equal(queue.pending, 3);

  assert.equal(queue.complete(0x84), false);
  assert.deepEqual(sent, ["targets"]);
  assert.equal(queue.complete(0x83), true);
  assert.deepEqual(sent, ["targets", "description"]);
  assert.equal(queue.complete(0x84), true);
  assert.deepEqual(sent, ["targets", "description", "help"]);
  assert.equal(queue.complete(0x86), true);
  assert.equal(queue.pending, 0);
});

test("clear removes queued and in-flight session requests", () => {
  const sent = [];
  const queue = new ResponseRequestQueue((frame) => sent.push(frame));
  queue.enqueue(1, "old active");
  queue.enqueue(2, "old queued");
  queue.clear();
  assert.equal(queue.pending, 0);
  assert.equal(queue.complete(1), false);

  queue.enqueue(3, "new session");
  assert.deepEqual(sent, ["old active", "new session"]);
});

test("send failures report the error and advance the queue", async () => {
  const sent = [];
  const errors = [];
  const queue = new ResponseRequestQueue((frame) => {
    if (frame === "bad") throw new Error("write failed");
    sent.push(frame);
  }, (error) => errors.push(error.message));

  queue.enqueue(1, "bad");
  queue.enqueue(2, "good");
  await Promise.resolve();
  assert.deepEqual(errors, ["write failed"]);
  assert.deepEqual(sent, ["good"]);
  assert.equal(queue.pending, 1);
});
