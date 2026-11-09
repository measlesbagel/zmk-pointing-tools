/* Type-through: type a passage with hands resting near the ball, and count
 * what the ball did meanwhile. Caret steps, wheel events and pointer
 * motion are split into while typing and while resting between bursts.
 * Typing needs none of them, so any that arrive are unintended. The flag
 * thresholds are starting points. Pure, so it runs under node:test. */

export const TYPE_PASSAGE = "The quick brown fox jumps over the lazy dog. Pack my box with five dozen liquor jugs. "
  + "How vexingly quick daft zebras jump. Sphinx of black quartz, judge my vow.";
export const TYPE_TIMEOUT_MS = 180000;
/* A pause this long after a key ends a typing burst. */
export const TYPING_GAP_MS = 1000;

/* run: { passage, endedAt, events: [{ t, kind, correct?, distance? }] }
 * with kind "char", "backspace", "step" (an arrow key), "wheel" or
 * "pointer" (with the distance moved since the last pointer event). */
export function measureTypeThrough(run) {
  const { events } = run;
  const keys = events.filter(({ kind }) => kind === "char" || kind === "backspace").map(({ t }) => t);
  const typing = (t) => {
    /* Inside a burst: a key at or before t, no more than the gap ago. */
    let low = 0;
    let high = keys.length - 1;
    let latest;
    while (low <= high) {
      const mid = (low + high) >> 1;
      if (keys[mid] <= t) {
        latest = keys[mid];
        low = mid + 1;
      } else high = mid - 1;
    }
    return latest !== undefined && t - latest <= TYPING_GAP_MS;
  };
  const counts = {
    typing: { steps: 0, wheel: 0, pointer: 0 },
    resting: { steps: 0, wheel: 0, pointer: 0 },
  };
  let characters = 0;
  let errors = 0;
  let backspaces = 0;
  for (const event of events) {
    if (event.kind === "char") {
      characters += 1;
      if (!event.correct) errors += 1;
      continue;
    }
    if (event.kind === "backspace") {
      backspaces += 1;
      continue;
    }
    /* Before the first key the hands are still settling. */
    if (!keys.length || event.t < keys[0]) continue;
    const bucket = counts[typing(event.t) ? "typing" : "resting"];
    if (event.kind === "step") bucket.steps += 1;
    else if (event.kind === "wheel") bucket.wheel += 1;
    else if (event.kind === "pointer") bucket.pointer += event.distance ?? 0;
  }
  let typingMs = 0;
  for (let i = 0; i < keys.length; i += 1) {
    const next = keys[i + 1];
    typingMs += next !== undefined && next - keys[i] <= TYPING_GAP_MS ? next - keys[i] : 0;
  }
  const per100 = (value) => (characters ? (value / characters) * 100 : NaN);
  return {
    characters,
    errors: characters ? errors / characters : NaN,
    backspaces,
    finished: run.endedAt !== undefined,
    wordsPerMinute: typingMs ? (characters / 5) / (typingMs / 60000) : NaN,
    typing: {
      ...counts.typing,
      stepsPer100: per100(counts.typing.steps),
      wheelPer100: per100(counts.typing.wheel),
      pointerPer100: per100(counts.typing.pointer),
    },
    resting: counts.resting,
  };
}

export function analyseTypeThrough(run) {
  const result = measureTypeThrough(run);
  const notes = [];
  if (result.typing.stepsPer100 >= 1) {
    notes.push("The caret stepped while you typed: text thresholds may be low enough that resting on the ball makes steps, or suppress-after-keypress may be short.");
  }
  if (result.typing.pointerPer100 >= 20) {
    notes.push("The pointer moved while you typed: suppress-after-keypress may be short, or the motion gate (activation-distance, coherence) may be letting resting contact through.");
  }
  if (result.typing.wheelPer100 >= 1) {
    notes.push("Scroll events arrived while you typed: suppress-after-keypress may be short, or scroll may be reacting to resting contact.");
  }
  if (result.resting.steps + result.resting.wheel > 0 || result.resting.pointer >= 20) {
    notes.push("The ball produced output during pauses with hands resting: the motion gate may be letting resting contact through.");
  }
  return { typeThrough: result, notes };
}

export function formatTypeThrough(result) {
  const cell = (value, digits) => (Number.isFinite(value) ? value.toFixed(digits) : "n/a");
  return [
    `Typed ${result.characters} characters at ${cell(result.wordsPerMinute, 0)} wpm, ${cell(result.errors * 100, 1)}% wrong`,
    `While typing: caret steps ${result.typing.steps} (${cell(result.typing.stepsPer100, 1)} per 100 characters), `
      + `scroll events ${result.typing.wheel}, pointer travel ${cell(result.typing.pointer, 0)} px (${cell(result.typing.pointerPer100, 1)} per 100 characters)`,
    `While resting between bursts: caret steps ${result.resting.steps}, scroll events ${result.resting.wheel}, `
      + `pointer travel ${cell(result.resting.pointer, 0)} px`,
  ];
}
