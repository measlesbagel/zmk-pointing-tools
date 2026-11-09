/* Which games a keyboard can play, worked out from what its firmware
 * reports. Pure, so it runs under node:test without a page. */

/* A stage is recognized by a parameter key only it has. Keys come from
 * parameter metadata, so a target is unclassified until that arrives. */
const STAGE_SIGNATURES = Object.freeze([
  ["orientation", "swap-xy"],
  ["keypress-guard", "suppress-after-keypress"],
  ["motion-gate", "coherence"],
  ["cursor-transfer", "scale-multiplier"],
  ["cursor-acceleration", "speed-rise"],
  ["cursor-coalescer", "usb-report-interval"],
  ["scroll", "steps-per-meter"],
  ["text", "horizontal-threshold"],
  ["axis-intent", "engage-ratio"],
  ["axis-constraint", "discard-unclassified"],
]);

export function classifyStage(target) {
  const keys = new Set(target.parameters.map(({ key }) => key).filter(Boolean));
  return STAGE_SIGNATURES.find(([, key]) => keys.has(key))?.[0];
}

/* Each game names the stage it is about, the playground activity that
 * plays it, the one or two settings that matter most, and the rest that
 * affect it. Settings are [stage, parameter key]; a key ending in "*"
 * matches a family such as curve points. Order is the first-setup order:
 * direction, base speed, then each capability. */
export const GAMES = Object.freeze([
  {
    id: "direction",
    title: "Compass start",
    drives: "cursor",
    stage: "orientation",
    runner: "compass",
    activity: "cursor-path",
    summary: "Click the centre, then roll toward the target that appears. Only the first few pixels count, so it works even when the axes are wrong, and it says which settings fix them.",
    main: [["orientation", "swap-xy"], ["orientation", "invert-x"], ["orientation", "invert-y"]],
    related: [],
  },
  {
    id: "cursor-speed",
    title: "Throughput ring",
    drives: "cursor",
    stage: "cursor-transfer",
    /* Played by the tuner's own runner rather than a playground activity. */
    runner: "ring",
    activity: "cursor-targets",
    summary: "Click targets around a ring as quickly and accurately as is comfortable. Throughput combines speed and accuracy, so a setting that only makes you sloppier doesn't win.",
    main: [["cursor-transfer", "scale-multiplier"], ["cursor-transfer", "scale-divisor"]],
    related: [["cursor-acceleration", "curve-speed-*"], ["cursor-acceleration", "curve-sensitivity-*"]],
  },
  {
    id: "distance-flicks",
    title: "Distance flicks",
    drives: "cursor",
    stage: "cursor-acceleration",
    runner: "flicks",
    activity: "cursor-targets",
    summary: "Flick from the centre to small targets near, mid and far. Each distance is scored on its own, so a curve that wins overall but lands short on long moves still shows.",
    main: [["cursor-acceleration", "curve-speed-*"], ["cursor-acceleration", "curve-sensitivity-*"]],
    related: [["cursor-transfer", "scale-multiplier"], ["cursor-transfer", "scale-divisor"],
      ["cursor-acceleration", "speed-rise"], ["cursor-acceleration", "speed-fall"]],
  },
  {
    id: "cursor-precision",
    title: "Pixel hunt",
    drives: "cursor",
    stage: "cursor-acceleration",
    runner: "pixel",
    activity: "cursor-precision",
    summary: "Rest the pointer on a 3, 2 and then 1 pixel target for 300 ms. Shows whether the slow end of the curve lets you reach a single pixel at all.",
    main: [["cursor-acceleration", "curve-sensitivity-*"]],
    related: [["cursor-acceleration", "curve-speed-*"], ["cursor-transfer", "scale-divisor"],
      ["motion-gate", "activation-distance"], ["motion-gate", "coherence"]],
  },
  {
    id: "pursuit",
    title: "Pursuit",
    drives: "cursor",
    stage: "cursor-acceleration",
    runner: "pursuit",
    activity: "cursor-path",
    summary: "Keep the pointer on a target that sweeps back and forth, slowly and then fast. Time on target, lag and overshoot at each reversal are reported apart.",
    main: [["cursor-acceleration", "speed-rise"], ["cursor-acceleration", "speed-fall"]],
    related: [["cursor-acceleration", "curve-speed-*"], ["cursor-acceleration", "curve-sensitivity-*"],
      ["cursor-coalescer", "usb-report-interval"], ["cursor-coalescer", "ble-report-interval"]],
  },
  {
    id: "still-hands",
    title: "Still hands",
    drives: "cursor",
    stage: "keypress-guard",
    runner: "still",
    activity: "click-drag",
    summary: "Click targets as they appear, then rest with hands off. Slip while clicking and the wait before you can move again are shown together, since one setting trades them.",
    main: [["keypress-guard", "suppress-after-keypress"]],
    related: [["motion-gate", "activation-distance"], ["motion-gate", "coherence"],
      ["motion-gate", "qualification-timeout"], ["motion-gate", "idle-timeout"]],
  },
  {
    id: "click-drag",
    title: "Click and drag",
    drives: "cursor",
    stage: "keypress-guard",
    activity: "click-drag",
    summary: "Click and drag without the cursor slipping when a key is pressed.",
    main: [["keypress-guard", "suppress-after-keypress"]],
    related: [["motion-gate", "activation-distance"], ["motion-gate", "coherence"]],
  },
  {
    id: "scroll-speed",
    title: "Scroll to the band",
    drives: "scroll",
    stage: "scroll",
    runner: "scroll-band",
    activity: "scroll-vertical",
    summary: "Scroll a line into the band and hold it there, first with its position shown, then hidden until it comes into view. Reports scroll throughput, overshoot, reversals and settle time.",
    main: [["scroll", "steps-per-meter"]],
    related: [["scroll", "report-interval"]],
  },
  {
    id: "axis-lock",
    title: "Rail and corner",
    drives: "scroll",
    stage: "axis-intent",
    runner: "rail",
    activity: "scroll-two-axis",
    summary: "Scroll a dot along straight tracks and round L corners. Shows wobble leaking onto the other axis, locks onto the wrong axis, and how long a corner takes to switch.",
    main: [["axis-intent", "engage-ratio"], ["axis-intent", "release-ratio"]],
    related: [["axis-intent", "activation-distance"], ["axis-intent", "window"], ["axis-intent", "idle-timeout"],
      ["axis-constraint", "discard-unclassified"], ["axis-constraint", "fold-interval"]],
  },
  {
    id: "text-steps",
    title: "Text movement",
    drives: "text",
    stage: "text",
    activity: "text-selection",
    summary: "Move the caret word by word and line by line. Each step should take the same roll.",
    main: [["text", "horizontal-threshold"], ["text", "vertical-threshold"]],
    related: [["text", "idle-timeout"]],
  },
]);

function keyMatches(pattern, key) {
  return pattern.endsWith("*") ? key?.startsWith(pattern.slice(0, -1)) : key === pattern;
}

function resolveSettings(settings, stages) {
  const resolved = [];
  for (const [stage, pattern] of settings) {
    for (const target of stages.get(stage) ?? []) {
      for (const parameter of target.parameters) {
        if (keyMatches(pattern, parameter.key)) {
          resolved.push({ targetId: target.id, parameterId: parameter.id });
        }
      }
    }
  }
  return resolved;
}

/* targets is an iterable of tuning targets with parameter keys. Returns
 * one entry per game the keyboard has the stage for, with the settings
 * resolved to target and parameter ids. */
export function applicableGames(targets) {
  const stages = new Map();
  for (const target of targets) {
    const stage = classifyStage(target);
    if (!stage) continue;
    if (!stages.has(stage)) stages.set(stage, []);
    stages.get(stage).push(target);
  }
  return GAMES.filter((game) => stages.has(game.stage)).map((game) => ({
    ...game,
    targets: stages.get(game.stage).map(({ id, label }) => ({ id, label })),
    main: resolveSettings(game.main, stages),
    related: resolveSettings(game.related, stages),
  }));
}

/* The game ids that show a setting, for links back from the settings
 * menu. */
export function gamesForSetting(games, targetId, parameterId) {
  return games
    .filter((game) => [...game.main, ...game.related].some(
      (setting) => setting.targetId === targetId && setting.parameterId === parameterId))
    .map(({ id, title }) => ({ id, title }));
}
