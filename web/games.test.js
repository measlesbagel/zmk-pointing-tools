import test from "node:test";
import assert from "node:assert/strict";

import { ACTIVITIES } from "./playground-model.js";
import { GAMES, applicableGames, classifyStage, gamesForSetting } from "./games.js";

function target(id, label, keys) {
  return { id, label, parameters: keys.map((key, parameterId) => ({ id: parameterId, key })) };
}

const cursorTransfer = target(0, "trackball-cursor", ["scale-multiplier", "scale-divisor"]);
const acceleration = target(1, "trackball-accel", [
  "curve-speed-1", "curve-speed-2", "curve-sensitivity-1", "curve-sensitivity-2", "speed-rise", "speed-fall",
]);
const scroll = target(2, "scroll-ball", ["steps-per-meter", "report-interval"]);

test("classifies stages by a key only they have", () => {
  assert.equal(classifyStage(cursorTransfer), "cursor-transfer");
  assert.equal(classifyStage(acceleration), "cursor-acceleration");
  assert.equal(classifyStage(target(3, "x", ["swap-xy", "invert-x", "invert-y"])), "orientation");
  /* Keys arrive with metadata; without them nothing is guessed. */
  assert.equal(classifyStage({ id: 4, parameters: [{ id: 0 }] }), undefined);
});

test("offers only the games the keyboard has stages for, in setup order", () => {
  const games = applicableGames([scroll, acceleration, cursorTransfer]);
  assert.deepEqual(games.map(({ id }) => id), ["cursor-speed", "distance-flicks", "cursor-precision", "scroll-speed"]);

  const speed = games.find(({ id }) => id === "cursor-speed");
  assert.deepEqual(speed.main, [{ targetId: 0, parameterId: 0 }, { targetId: 0, parameterId: 1 }]);
  /* Curve families resolve to every point. */
  assert.deepEqual(speed.related.map(({ parameterId }) => parameterId), [0, 1, 2, 3]);
  assert.deepEqual(speed.targets, [{ id: 0, label: "trackball-cursor" }]);
});

test("links a setting back to every game that shows it", () => {
  const games = applicableGames([acceleration, cursorTransfer]);
  assert.deepEqual(gamesForSetting(games, 1, 2).map(({ id }) => id), ["cursor-speed", "distance-flicks", "cursor-precision"]);
  assert.deepEqual(gamesForSetting(games, 1, 4).map(({ id }) => id), ["distance-flicks", "cursor-precision"]);
  assert.deepEqual(gamesForSetting(games, 9, 0), []);
});

test("every game has a runner or an existing playground activity", () => {
  const activities = new Set(ACTIVITIES.map(({ id }) => id));
  for (const game of GAMES) assert.ok(activities.has(game.activity), game.id);
});
