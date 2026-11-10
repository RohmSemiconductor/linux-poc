export interface Stats {
  min: number;
  max: number;
  mean: number;
  std: number;
  pp: number;
  n: number;
}

const STATS_BLOCK_SIZE = 256;
const STATS_FANOUT = 8;

interface StatsLevel {
  min: Float32Array;
  max: Float32Array;
  mean: Float64Array;
  m2: Float64Array;
  length: number;
  span: number;
  sampleCount: number;
}

interface StatsSummary {
  min: number;
  max: number;
  mean: number;
  m2: number;
  n: number;
}

function createStatsLevel(span: number, sampleCount: number): StatsLevel {
  return {
    min: new Float32Array(0),
    max: new Float32Array(0),
    mean: new Float64Array(0),
    m2: new Float64Array(0),
    length: 0,
    span,
    sampleCount,
  };
}

function ensureStatsCapacity(level: StatsLevel, required: number): void {
  if (required <= level.min.length) return;
  const capacity = Math.max(required, Math.max(256, Math.ceil(level.min.length * 1.25)));
  const min = new Float32Array(capacity);
  const max = new Float32Array(capacity);
  const mean = new Float64Array(capacity);
  const m2 = new Float64Array(capacity);
  min.set(level.min.subarray(0, level.length));
  max.set(level.max.subarray(0, level.length));
  mean.set(level.mean.subarray(0, level.length));
  m2.set(level.m2.subarray(0, level.length));
  level.min = min;
  level.max = max;
  level.mean = mean;
  level.m2 = m2;
}

function mergeStats(a: StatsSummary, b: StatsSummary): StatsSummary {
  const n = a.n + b.n;
  const delta = b.mean - a.mean;
  return {
    min: b.min < a.min ? b.min : a.min,
    max: b.max > a.max ? b.max : a.max,
    mean: a.mean + (delta * b.n) / n,
    m2: a.m2 + b.m2 + ((delta * delta * a.n) / n) * b.n,
    n,
  };
}

function isNearRoundingBoundary(
  value: number,
  displayScale: number,
  precision: number,
): boolean {
  if (!Number.isFinite(value) || !Number.isFinite(displayScale)) return false;

  const scaledValue = value * displayScale * 10 ** precision;
  if (!Number.isFinite(scaledValue)) return false;
  const fraction = scaledValue - Math.floor(scaledValue);
  const errorBound =
    256 *
    Number.EPSILON *
    Math.max(1, Math.abs(value)) *
    Math.abs(displayScale * 10 ** precision);
  return Math.abs(fraction - 0.5) <= errorBound;
}

/**
 * Incremental block summaries for fast statistics over large visible ranges.
 * Small ranges use computeStats directly to preserve its exact calculation.
 */
export class StatsIndex {
  private sampleCount = 0;
  private partialCount = 0;
  private partialMin = 0;
  private partialMax = 0;
  private partialMean = 0;
  private partialM2 = 0;
  private levels: StatsLevel[] = [
    createStatsLevel(1, STATS_BLOCK_SIZE),
  ];

  reset(): void {
    this.sampleCount = 0;
    this.partialCount = 0;
    this.partialMin = 0;
    this.partialMax = 0;
    this.partialMean = 0;
    this.partialM2 = 0;
    this.levels = [createStatsLevel(1, STATS_BLOCK_SIZE)];
  }

  update(ys: Float32Array, count: number): void {
    if (count < this.sampleCount) this.reset();

    ensureStatsCapacity(
      this.levels[0],
      Math.ceil(count / STATS_BLOCK_SIZE),
    );

    let index = this.sampleCount;
    if (index % STATS_BLOCK_SIZE !== 0) {
      const blockIndex = Math.floor(index / STATS_BLOCK_SIZE);
      const blockEnd = Math.min(
        count,
        (blockIndex + 1) * STATS_BLOCK_SIZE,
      );
      for (let i = index; i < blockEnd; i++) {
        this.appendPartial(ys[i]);
      }
      index = blockEnd;
      if (this.partialCount === STATS_BLOCK_SIZE) {
        this.completeBlock();
      }
    }

    const completeEnd = count - (count % STATS_BLOCK_SIZE);
    while (index < completeEnd) {
      const blockEnd = index + STATS_BLOCK_SIZE;
      let min = ys[index];
      let max = min;
      let sum = 0;
      for (let i = index; i < blockEnd; i++) {
        const value = ys[i];
        if (value < min) min = value;
        if (value > max) max = value;
        sum += value;
      }

      const mean = sum / STATS_BLOCK_SIZE;
      let m2 = 0;
      for (let i = index; i < blockEnd; i++) {
        const delta = ys[i] - mean;
        m2 += delta * delta;
      }
      this.storeBlock(min, max, mean, m2);
      index = blockEnd;
    }

    for (let i = index; i < count; i++) {
      this.appendPartial(ys[i]);
    }

    this.sampleCount = count;
  }

  compute(
    ys: Float32Array,
    start: number,
    end: number,
    displayScale = 1,
    displayPrecision = 2,
  ): Stats | null {
    const n = end - start;
    if (n < 1) return null;
    if (n <= STATS_BLOCK_SIZE * 2) return computeStats(ys, start, end);

    const firstFullBlock = Math.ceil(start / STATS_BLOCK_SIZE);
    const endFullBlock = Math.floor(end / STATS_BLOCK_SIZE);
    if (firstFullBlock >= endFullBlock) return computeStats(ys, start, end);

    let summary: StatsSummary | null = null;
    const headEnd = Math.min(end, firstFullBlock * STATS_BLOCK_SIZE);
    if (start < headEnd) {
      summary = this.summarizeRange(ys, start, headEnd);
    }

    let block = firstFullBlock;
    while (block < endFullBlock) {
      let levelIndex = 0;
      for (let i = this.levels.length - 1; i >= 0; i--) {
        const level = this.levels[i];
        if (
          block % level.span === 0 &&
          block + level.span <= endFullBlock &&
          block / level.span < level.length
        ) {
          levelIndex = i;
          break;
        }
      }

      const level = this.levels[levelIndex];
      const index = block / level.span;
      const next: StatsSummary = {
        min: level.min[index],
        max: level.max[index],
        mean: level.mean[index],
        m2: level.m2[index],
        n: level.sampleCount,
      };
      summary = summary ? mergeStats(summary, next) : next;
      block += level.span;
    }

    const tailStart = Math.max(headEnd, endFullBlock * STATS_BLOCK_SIZE);
    if (tailStart < end) {
      const tail = this.summarizeRange(ys, tailStart, end);
      summary = summary ? mergeStats(summary, tail) : tail;
    }

    if (!summary) return computeStats(ys, start, end);
    const stats = {
      min: summary.min,
      max: summary.max,
      mean: summary.mean,
      std: Math.sqrt(summary.m2 / summary.n),
      pp: summary.max - summary.min,
      n: summary.n,
    };
    if (
      isNearRoundingBoundary(stats.mean, displayScale, displayPrecision) ||
      isNearRoundingBoundary(stats.std, displayScale, displayPrecision)
    ) {
      return computeStats(ys, start, end);
    }
    return stats;
  }

  private summarizeRange(
    ys: Float32Array,
    start: number,
    end: number,
  ): StatsSummary {
    let min = ys[start];
    let max = min;
    let mean = min;
    let m2 = 0;
    let n = 1;

    for (let i = start + 1; i < end; i++) {
      const value = ys[i];
      if (value < min) min = value;
      if (value > max) max = value;
      n++;
      const delta = value - mean;
      mean += delta / n;
      m2 += delta * (value - mean);
    }

    return { min, max, mean, m2, n };
  }

  private appendPartial(value: number): void {
    if (this.partialCount === 0) {
      this.partialMin = value;
      this.partialMax = value;
      this.partialMean = value;
      this.partialM2 = 0;
      this.partialCount = 1;
      return;
    }

    if (value < this.partialMin) this.partialMin = value;
    if (value > this.partialMax) this.partialMax = value;
    this.partialCount++;
    const delta = value - this.partialMean;
    this.partialMean += delta / this.partialCount;
    this.partialM2 += delta * (value - this.partialMean);
  }

  private completeBlock(): void {
    this.storeBlock(
      this.partialMin,
      this.partialMax,
      this.partialMean,
      this.partialM2,
    );
    this.partialCount = 0;
    this.partialMean = 0;
    this.partialM2 = 0;
  }

  private storeBlock(
    min: number,
    max: number,
    mean: number,
    m2: number,
  ): void {
    const leaves = this.levels[0];
    const blockIndex = leaves.length;
    ensureStatsCapacity(leaves, blockIndex + 1);
    leaves.min[blockIndex] = min;
    leaves.max[blockIndex] = max;
    leaves.mean[blockIndex] = mean;
    leaves.m2[blockIndex] = m2;
    leaves.length = blockIndex + 1;

    this.completeParents(blockIndex);
  }

  private completeParents(blockIndex: number): void {
    let childIndex = blockIndex;
    let childLevelIndex = 0;
    while (childIndex % STATS_FANOUT === STATS_FANOUT - 1) {
      const children = this.levels[childLevelIndex];
      const parentIndex = Math.floor(childIndex / STATS_FANOUT);
      const firstChild = parentIndex * STATS_FANOUT;
      const childCount = children.sampleCount;
      let min = children.min[firstChild];
      let max = children.max[firstChild];
      let mean = children.mean[firstChild];
      let m2 = children.m2[firstChild];
      let mergedCount = childCount;

      for (let i = firstChild + 1; i < firstChild + STATS_FANOUT; i++) {
        const nextMean = children.mean[i];
        const delta = nextMean - mean;
        const nextCount = mergedCount + childCount;
        m2 +=
          children.m2[i] +
          ((delta * delta * mergedCount) / nextCount) * childCount;
        mean += (delta * childCount) / nextCount;
        mergedCount = nextCount;
        if (children.min[i] < min) min = children.min[i];
        if (children.max[i] > max) max = children.max[i];
      }

      if (!this.levels[childLevelIndex + 1]) {
        this.levels.push(
          createStatsLevel(
            children.span * STATS_FANOUT,
            children.sampleCount * STATS_FANOUT,
          ),
        );
      }
      const parent = this.levels[childLevelIndex + 1];
      ensureStatsCapacity(parent, parentIndex + 1);
      parent.min[parentIndex] = min;
      parent.max[parentIndex] = max;
      parent.mean[parentIndex] = mean;
      parent.m2[parentIndex] = m2;
      parent.length = parentIndex + 1;

      childIndex = parentIndex;
      childLevelIndex++;
    }
  }
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
