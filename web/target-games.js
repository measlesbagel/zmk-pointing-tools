/* Target layouts for the cursor games. Pure, so the layouts are tested and
 * repeat exactly between runs and settings. */

/* The ISO 9241-9 ring: `count` targets (odd) on a circle of diameter
 * `amplitude`, visited across the circle each time, so every movement is
 * about one diameter long and the directions rotate all the way round. */
export function ringSequence({ amplitude, width, count = 13, centre = { x: 0, y: 0 } }) {
  if (count % 2 === 0) throw new Error("A ring needs an odd number of targets");
  const step = (count + 1) / 2;
  const targets = [];
  for (let i = 0; i <= count; i += 1) {
    const index = (i * step) % count;
    const angle = (2 * Math.PI * index) / count - Math.PI / 2;
    targets.push({
      x: centre.x + (amplitude / 2) * Math.cos(angle),
      y: centre.y + (amplitude / 2) * Math.sin(angle),
      width,
    });
  }
  /* count + 1 targets: the first click starts the sequence and isn't
   * scored, leaving `count` measured movements. */
  return targets;
}

/* Three sequences spanning easy to hard. Index of difficulty
 * log2(A/W + 1) runs from about 2.6 to 5.4 bits. Sizes scale with the
 * playing area so a small window still fits. */
export function ringLevels(size) {
  const amplitude = Math.min(size.width, size.height) * 0.8;
  return [
    { amplitude: amplitude * 0.5, width: Math.max(24, amplitude * 0.1) },
    { amplitude: amplitude * 0.75, width: Math.max(14, amplitude * 0.05) },
    { amplitude, width: Math.max(8, amplitude * 0.025) },
  ].map((level) => ({ ...level, id: Math.log2(level.amplitude / level.width + 1) }));
}

/* Distance flicks: from a home point at the centre to one small target at a
 * near, mid or far distance, in a fixed shuffled order. Every band gets the
 * same set of directions so bands compare fairly. The seed keeps the order
 * the same between runs. Distances are fractions of the smaller side, so
 * the far band still fits inside half of it. */
export const FLICK_BANDS = Object.freeze([
  { band: "near", fraction: 0.12 },
  { band: "mid", fraction: 0.3 },
  { band: "far", fraction: 0.46 },
]);

export function flickTrials(size, { perBand = 6, width = 16, random = Math.random } = {}) {
  const reach = Math.min(size.width, size.height);
  const centre = { x: size.width / 2, y: size.height / 2 };
  const trials = [];
  for (const { band, fraction } of FLICK_BANDS) {
    for (let i = 0; i < perBand; i += 1) {
      /* Spread directions round the circle, offset per band so no two bands
       * share an exact line. */
      const angle = (2 * Math.PI * i) / perBand + fraction;
      const radius = reach * fraction;
      trials.push({
        band,
        home: { ...centre, width: 28 },
        target: { x: centre.x + radius * Math.cos(angle), y: centre.y - radius * Math.sin(angle), width },
      });
    }
  }
  for (let i = trials.length - 1; i > 0; i -= 1) {
    const j = Math.floor(random() * (i + 1));
    [trials[i], trials[j]] = [trials[j], trials[i]];
  }
  return trials;
}

/* Hints for a band that did clearly worse than the others. They say what
 * the numbers show and which settings shape that distance; they never pick
 * a value. byBand maps band to a summary from measure.js. */
export function flickFlags(byBand, width) {
  const flags = [];
  const near = byBand.near;
  const far = byBand.far;
  if (far && far.rerolls >= 0.5) {
    flags.push("Far targets needed re-rolls: long moves may want more gain or a higher fast end of the curve.");
  }
  if (far && far.signedError < -width) {
    flags.push("Far targets landed short: the fast end of the curve may be low.");
  }
  if (near && near.signedError > width / 2) {
    flags.push("Near targets landed past the centre: the slow end of the curve may be high.");
  }
  if (near && near.submovements >= 3) {
    flags.push("Near targets took several corrections: the slow end of the curve or the gain may be high.");
  }
  const hits = Object.entries(byBand).filter(([, summary]) => Number.isFinite(summary.hitRate));
  const best = Math.max(...hits.map(([, summary]) => summary.hitRate));
  for (const [band, summary] of hits) {
    if (best - summary.hitRate >= 0.34) flags.push(`${band[0].toUpperCase()}${band.slice(1)} targets were missed much more often than the others.`);
  }
  return flags;
}
