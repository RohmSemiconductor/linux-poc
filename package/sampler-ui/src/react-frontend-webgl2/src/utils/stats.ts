export interface Stats {
  min: number;
  max: number;
  mean: number;
  std: number;
  pp: number;
  n: number;
}

/**
 * Compute basic statistics over ys[start..end) (exclusive end).
 * Values are raw ADC counts — callers convert to voltage for display.
 * Returns null when the range is empty.
 */
export function computeStats(
  ys: Float32Array,
  start: number,
  end: number,
): Stats | null {
  const n = end - start;
  if (n < 1) return null;

  let min = ys[start];
  let max = ys[start];
  let sum = 0;

  for (let i = start; i < end; i++) {
    const v = ys[i];
    if (v < min) min = v;
    if (v > max) max = v;
    sum += v;
  }

  const mean = sum / n;
  let variance = 0;
  for (let i = start; i < end; i++) {
    const d = ys[i] - mean;
    variance += d * d;
  }

  return { min, max, mean, std: Math.sqrt(variance / n), pp: max - min, n };
}
