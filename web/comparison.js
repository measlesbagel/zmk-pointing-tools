/* Blind comparison of two settings for one game. Pure, so the protocol and
 * the statistics run under node:test.
 *
 * The protocol adapts standard pointing-study practice to one person:
 * an unscored warm-up, the two settings in ABBA or BAAB order, an unscored
 * adaptation block after every switch, labels hidden as X and Y, and a
 * preference asked before the reveal. The result is a median difference
 * with a bootstrap interval, never a best run and never a verdict. */

import { bootstrapDifference, seededRandom, summarize } from "./measure.js";

/* The blocks of one session in play order. The warm-up is on the first
 * setting, so it also serves as that setting's adaptation. */
export function planBlocks(random = Math.random) {
  const order = random() < 0.5 ? ["A", "B", "B", "A"] : ["B", "A", "A", "B"];
  const blocks = [{ condition: order[0], scored: false, kind: "warm-up" }];
  let previous = order[0];
  for (const condition of order) {
    if (condition !== previous) blocks.push({ condition, scored: false, kind: "adaptation" });
    blocks.push({ condition, scored: true, kind: "scored" });
    previous = condition;
  }
  return { order: order.join(""), blocks };
}

/* Which hidden name each setting plays under. */
export function hiddenLabels(random = Math.random) {
  return random() < 0.5 ? { A: "X", B: "Y" } : { A: "Y", B: "X" };
}

function mean(values) {
  const finite = values.filter(Number.isFinite);
  return finite.length ? finite.reduce((sum, value) => sum + value, 0) / finite.length : NaN;
}

/* blocks: [{ condition, scored, measured: [measureTrial results], throughput }].
 * Only scored blocks count. Movement time compares hits only, since a miss
 * that was quick is not a fast movement. Differences are B minus A, so a
 * negative time difference means B was faster. */
export function compareConditions(blocks, { random = seededRandom(11) } = {}) {
  const scored = blocks.filter((block) => block.scored);
  const side = (condition) => {
    const own = scored.filter((block) => block.condition === condition);
    const measured = own.flatMap((block) => block.measured);
    return {
      summary: summarize(measured),
      throughput: mean(own.map((block) => block.throughput)),
      times: measured.filter(({ hit }) => hit).map(({ movementTime }) => movementTime),
      overshoots: measured.map(({ overshoot }) => overshoot),
      measured,
    };
  };
  const a = side("A");
  const b = side("B");
  return {
    A: { summary: a.summary, throughput: a.throughput },
    B: { summary: b.summary, throughput: b.throughput },
    time: bootstrapDifference(a.times, b.times, { random }),
    overshoot: bootstrapDifference(a.overshoots, b.overshoots, { random }),
    measured: { A: a.measured, B: b.measured },
  };
}

function signed(value, digits) {
  const rounded = Number(value.toFixed(digits));
  if (rounded === 0) return (0).toFixed(digits);
  return `${rounded > 0 ? "+" : ""}${rounded.toFixed(digits)}`;
}

/* The comparison in words, B against A. It describes the interval; it
 * doesn't call a winner, because one session is a small sample. */
export function describeComparison(result) {
  const lines = [];
  const { time, overshoot } = result;
  if (time) {
    const clear = time.high < 0 || time.low > 0;
    lines.push(`Movement time, B minus A: ${signed(time.difference, 0)} ms (95% interval ${signed(time.low, 0)} to ${signed(time.high, 0)}). `
      + `B was faster in ${Math.round((1 - time.probabilityBAhead) * 100)}% of resamples. `
      + (clear ? "The interval doesn't cross zero." : "The interval crosses zero, so this session can't tell them apart on time."));
  }
  if (overshoot) {
    lines.push(`Overshoot, B minus A: ${signed(overshoot.difference * 100, 1)}% of the distance (95% interval ${signed(overshoot.low * 100, 1)} to ${signed(overshoot.high * 100, 1)}).`);
  }
  if (Number.isFinite(result.A.throughput) && Number.isFinite(result.B.throughput)) {
    lines.push(`Throughput: A ${result.A.throughput.toFixed(2)}, B ${result.B.throughput.toFixed(2)} bit/s.`);
  }
  return lines;
}

/* How the stated preference lines up with what was measured. */
export function describePreference(preference, labels, result) {
  if (!preference) return "No preference was given.";
  if (preference === "same") return "You felt no difference.";
  const preferred = labels.A === preference ? "A" : "B";
  const time = result.time;
  if (!time || (time.low <= 0 && time.high >= 0)) {
    return `You preferred ${preference}, which was ${preferred}. The times couldn't tell them apart.`;
  }
  const faster = time.high < 0 ? "B" : "A";
  return faster === preferred
    ? `You preferred ${preference}, which was ${preferred}, and it was also faster.`
    : `You preferred ${preference}, which was ${preferred}, but ${faster} was faster.`;
}
