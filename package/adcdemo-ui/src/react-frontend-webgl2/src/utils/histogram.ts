export interface HistogramResult {
  counts: Float32Array;
  binCount: number;
  binWidth: number; // ADC counts per bin
  maxCount: number;
  yMax: number; // nice-rounded Y axis ceiling (>= maxCount)
}

function niceValue(value: number): number {
  if (value <= 0) return 1;
  const mag = Math.pow(10, Math.floor(Math.log10(value)));
  const norm = value / mag;
  const nice = norm <= 1 ? 1 : norm <= 2 ? 2 : norm <= 5 ? 5 : 10;
  return nice * mag;
}

export function computeHistogram(
  ys: Float32Array,
  count: number,
  adcMax: number,
): HistogramResult | null {
  if (count < 1) return null;
  const binCount = Math.min(adcMax + 1, 4096);
  const binWidth = (adcMax + 1) / binCount;
  const counts = new Float32Array(binCount);
  for (let i = 0; i < count; i++) {
    const bin = Math.min(binCount - 1, Math.floor(ys[i] / binWidth));
    counts[bin]++;
  }
  let maxCount = 0;
  for (let k = 0; k < binCount; k++) {
    if (counts[k] > maxCount) maxCount = counts[k];
  }
  return { counts, binCount, binWidth, maxCount, yMax: niceValue(maxCount) };
}

// Adds samples ys[fromIdx..toIdx) into an existing result in place.
export function updateHistogram(
  result: HistogramResult,
  ys: Float32Array,
  fromIdx: number,
  toIdx: number,
): void {
  const { counts, binCount, binWidth } = result;
  let maxCount = result.maxCount;
  for (let i = fromIdx; i < toIdx; i++) {
    const bin = Math.min(binCount - 1, Math.floor(ys[i] / binWidth));
    const c = ++counts[bin];
    if (c > maxCount) maxCount = c;
  }
  result.maxCount = maxCount;
  result.yMax = niceValue(maxCount);
}
