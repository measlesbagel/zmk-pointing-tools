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
    title: "Direction check",
    drives: "cursor",
    stage: "orientation",
    activity: "cursor-path",
    summary: "Trace the diagonal and the circle. The cursor should follow the ball the way you expect.",
    main: [["orientation", "swap-xy"], ["orientation", "invert-x"], ["orientation", "invert-y"]],
    related: [],
  },
  {
    id: "cursor-speed",
    title: "Base cursor speed",
    drives: "cursor",
    stage: "cursor-transfer",
    activity: "cursor-targets",
    summary: "Reach targets across the screen. Find the speed where a comfortable roll crosses it.",
    main: [["cursor-transfer", "scale-multiplier"], ["cursor-transfer", "scale-divisor"]],
    related: [["cursor-acceleration", "curve-speed-*"], ["cursor-acceleration", "curve-sensitivity-*"]],
  },
  {
    id: "cursor-precision",
    title: "Small-target precision",
    drives: "cursor",
    stage: "cursor-acceleration",
    activity: "cursor-precision",
    summary: "Land on small targets without overshooting. Slow motion is shaped by the acceleration curve.",
    main: [["cursor-acceleration", "curve-sensitivity-*"]],
    related: [["cursor-acceleration", "curve-speed-*"], ["cursor-acceleration", "speed-rise"],
      ["cursor-acceleration", "speed-fall"], ["motion-gate", "activation-distance"]],
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
    title: "Scroll speed",
    drives: "scroll",
    stage: "scroll",
    activity: "scroll-vertical",
    summary: "Scroll to the marked lines. Find a speed that reaches far without skipping near.",
    main: [["scroll", "steps-per-meter"]],
    related: [["scroll", "report-interval"]],
  },
  {
    id: "axis-lock",
    title: "Axis lock",
    drives: "scroll",
    stage: "axis-intent",
    activity: "scroll-two-axis",
    summary: "Scroll straight down, then straight across. A wobble shouldn't drift onto the other axis.",
    main: [["axis-intent", "engage-ratio"], ["axis-intent", "release-ratio"]],
    related: [["axis-intent", "activation-distance"], ["axis-intent", "window"],
      ["axis-constraint", "discard-unclassified"]],
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
