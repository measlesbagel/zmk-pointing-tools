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
