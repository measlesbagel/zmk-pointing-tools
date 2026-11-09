/* Game results kept in this browser, and the plain-text form people and
 * agents copy. Storage can be missing or full; games still run without it. */

const HISTORY_KEY = "zpt-game-history";
const HISTORY_LIMIT = 200;

function readAll(storage) {
  try {
    const saved = JSON.parse(storage?.getItem(HISTORY_KEY) ?? "[]");
    return Array.isArray(saved) ? saved : [];
  } catch {
    return [];
  }
}

export function loadHistory(gameId, storage = globalThis.localStorage) {
  const runs = readAll(storage);
  return gameId ? runs.filter((run) => run.game === gameId) : runs;
}

export function saveRun(run, storage = globalThis.localStorage) {
  const runs = [...readAll(storage), run].slice(-HISTORY_LIMIT);
  try {
    storage?.setItem(HISTORY_KEY, JSON.stringify(runs));
    return true;
  } catch {
    return false;
  }
}

export function clearHistory(gameId, storage = globalThis.localStorage) {
  try {
    storage?.setItem(HISTORY_KEY, JSON.stringify(gameId ? readAll(storage).filter((run) => run.game !== gameId) : []));
  } catch {
    /* Nothing to clear. */
  }
}

function format(value, digits = 2) {
  return Number.isFinite(value) ? Number(value.toFixed(digits)).toString() : "n/a";
}

function summaryLine(summary) {
  return [
    ...(Number.isFinite(summary.throughput) ? [`throughput ${format(summary.throughput)} bit/s`] : []),
    `trials ${summary.trials}`,
    `hit ${format(summary.hitRate * 100, 0)}%`,
    `time ${format(summary.movementTime, 0)} ms`,
    `endpoint ${format(summary.signedError, 1)} px`,
    `re-entries ${format(summary.reEntries)}`,
    `overshoot ${format(summary.overshoot * 100, 1)}%`,
    `submovements ${format(summary.submovements, 1)}`,
    `re-rolls ${format(summary.rerolls)}`,
  ].join(", ");
}

/* A run as stable, labelled text: what was played, with which settings, and
 * every measure. */
export function formatRun(run) {
  const lines = [
    `Game: ${run.title} (${run.game})`,
    `Played: ${run.finishedAt}${run.label ? ` · label ${run.label}` : ""}`,
  ];
  if (Number.isFinite(run.throughput)) lines.push(`Throughput: ${format(run.throughput)} bit/s`);
  if (run.summary) lines.push(`Overall: ${summaryLine(run.summary)}`);
  for (const [name, groups] of Object.entries(run.groups ?? {})) {
    for (const [group, summary] of Object.entries(groups)) lines.push(`${name} ${group}: ${summaryLine(summary)}`);
  }
  for (const note of run.notes ?? []) lines.push(`Note: ${note}`);
  if (run.settings?.length) {
    lines.push("Settings:");
    for (const setting of run.settings) {
      lines.push(`  ${setting.target} / ${setting.key ?? setting.label} = ${setting.value}${setting.unit ? ` ${setting.unit}` : ""}`);
    }
  }
  return lines.join("\n");
}
