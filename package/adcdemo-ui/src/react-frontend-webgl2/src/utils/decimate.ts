const MIN_MAX_BLOCK_SIZE = 16;
const PYRAMID_FANOUT = 8;

interface MinMaxLevel {
  min: Float32Array;
  max: Float32Array;
  length: number;
  span: number;
}

interface MinMaxRange {
  min: number;
  max: number;
}

function createMinMaxLevel(span: number): MinMaxLevel {
  return {
    min: new Float32Array(0),
    max: new Float32Array(0),
    length: 0,
    span,
  };
}

function ensureMinMaxCapacity(level: MinMaxLevel, required: number): void {
  if (required <= level.min.length) return;
  const capacity = Math.max(required, Math.max(256, Math.ceil(level.min.length * 1.25)));
  const min = new Float32Array(capacity);
  const max = new Float32Array(capacity);
  min.set(level.min.subarray(0, level.length));
  max.set(level.max.subarray(0, level.length));
  level.min = min;
  level.max = max;
}

/**
 * Incremental min/max summaries for exact range queries over appended samples.
 * The base summaries cover 16 samples; each higher level combines groups of 8.
 */
export class MinMaxPyramid {
  private sampleCount = 0;
  private levels: MinMaxLevel[] = [createMinMaxLevel(1)];

  reset(): void {
    this.sampleCount = 0;
    this.levels = [createMinMaxLevel(1)];
  }

  update(ys: Float32Array, count: number): void {
    if (count < this.sampleCount) this.reset();

    const leaves = this.levels[0];
    ensureMinMaxCapacity(
      leaves,
      Math.ceil(count / MIN_MAX_BLOCK_SIZE),
    );

    let index = this.sampleCount;
    if (index % MIN_MAX_BLOCK_SIZE !== 0) {
      const blockIndex = Math.floor(index / MIN_MAX_BLOCK_SIZE);
      const blockEnd = Math.min(
        count,
        (blockIndex + 1) * MIN_MAX_BLOCK_SIZE,
      );
      let min = leaves.min[blockIndex];
      let max = leaves.max[blockIndex];
      for (let i = index; i < blockEnd; i++) {
        const value = ys[i];
        if (value < min) min = value;
        else if (value > max) max = value;
      }
      leaves.min[blockIndex] = min;
      leaves.max[blockIndex] = max;
      index = blockEnd;
      if (index % MIN_MAX_BLOCK_SIZE === 0) {
        this.completeBlock(blockIndex);
      }
    }

    const completeEnd = count - (count % MIN_MAX_BLOCK_SIZE);
    while (index < completeEnd) {
      const blockIndex = index / MIN_MAX_BLOCK_SIZE;
      let min = ys[index];
      let max = min;
      const blockEnd = index + MIN_MAX_BLOCK_SIZE;
      for (let i = index + 1; i < blockEnd; i++) {
        const value = ys[i];
        if (value < min) min = value;
        else if (value > max) max = value;
      }
      leaves.min[blockIndex] = min;
      leaves.max[blockIndex] = max;
      leaves.length = blockIndex + 1;
      if (blockIndex % PYRAMID_FANOUT === PYRAMID_FANOUT - 1) {
        this.completeBlock(blockIndex);
      }
      index = blockEnd;
    }

    if (index < count) {
      const blockIndex = Math.floor(index / MIN_MAX_BLOCK_SIZE);
      let min = ys[index];
      let max = min;
      for (let i = index + 1; i < count; i++) {
        const value = ys[i];
        if (value < min) min = value;
        else if (value > max) max = value;
      }
      leaves.min[blockIndex] = min;
      leaves.max[blockIndex] = max;
      leaves.length = blockIndex + 1;
    }

    this.sampleCount = count;
  }

  getMinMax(
    ys: Float32Array,
    start: number,
    end: number,
    result: MinMaxRange,
  ): void {
    let hasValue = false;
    let min = 0;
    let max = 0;

    const include = (nextMin: number, nextMax: number) => {
      if (!hasValue) {
        min = nextMin;
        max = nextMax;
        hasValue = true;
      } else {
        if (nextMin < min) min = nextMin;
        if (nextMax > max) max = nextMax;
      }
    };

    const firstFullBlock = Math.ceil(start / MIN_MAX_BLOCK_SIZE);
    const endFullBlock = Math.floor(end / MIN_MAX_BLOCK_SIZE);
    const headEnd = Math.min(end, firstFullBlock * MIN_MAX_BLOCK_SIZE);

    for (let i = start; i < headEnd; i++) {
      include(ys[i], ys[i]);
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
      include(level.min[index], level.max[index]);
      block += level.span;
    }

    const tailStart = Math.max(headEnd, endFullBlock * MIN_MAX_BLOCK_SIZE);
    for (let i = tailStart; i < end; i++) {
      include(ys[i], ys[i]);
    }

    result.min = min;
    result.max = max;
  }

  private completeBlock(blockIndex: number): void {
    let childIndex = blockIndex;
    let childLevelIndex = 0;

    while (childIndex % PYRAMID_FANOUT === PYRAMID_FANOUT - 1) {
      const children = this.levels[childLevelIndex];
      const parentIndex = Math.floor(childIndex / PYRAMID_FANOUT);
      const firstChild = parentIndex * PYRAMID_FANOUT;
      let min = children.min[firstChild];
      let max = children.max[firstChild];

      for (let i = firstChild + 1; i < firstChild + PYRAMID_FANOUT; i++) {
        const childMin = children.min[i];
        const childMax = children.max[i];
        if (childMin < min) min = childMin;
        if (childMax > max) max = childMax;
      }

      if (!this.levels[childLevelIndex + 1]) {
        this.levels.push(
          createMinMaxLevel(children.span * PYRAMID_FANOUT),
        );
      }
      const parent = this.levels[childLevelIndex + 1];
      ensureMinMaxCapacity(parent, parentIndex + 1);
      parent.min[parentIndex] = min;
      parent.max[parentIndex] = max;
      parent.length = parentIndex + 1;

      childIndex = parentIndex;
      childLevelIndex++;
    }
  }
}

/**
 * Builds a min-max decimated buffer for display when sample count exceeds pixel count.
 *
 * For each of `numPairs` pixel columns, finds the min and max value in that column's
 * sample range and writes them as a consecutive pair into `out`. Drawn as LINE_STRIP,
 * this produces a waveform envelope that is visually identical to full rendering.
 *
 * Alternates (min,max) / (max,min) per pair so the connecting segment between adjacent
 * columns stays within the signal envelope rather than crossing it.
 *
 * @param ys       Source sample buffer.
 * @param from     First sample index (inclusive).
 * @param count    Number of samples to decimate.
 * @param numPairs Number of pixel columns (output has 2*numPairs entries).
 * @param out      Pre-allocated output buffer, length >= 2*numPairs.
 */
export function buildMinMaxDecimation(
  ys: Float32Array,
  from: number,
  count: number,
  numPairs: number,
  out: Float32Array,
  pyramid?: MinMaxPyramid,
): void {
  if (count < 1 || numPairs < 1) return;
  const step = count / numPairs;
  const range = { min: 0, max: 0 };
  for (let i = 0; i < numPairs; i++) {
    const s0 = from + Math.floor(i * step);
    const s1 = from + Math.min(count, Math.ceil((i + 1) * step));
    let mn: number;
    let mx: number;
    if (pyramid) {
      pyramid.getMinMax(ys, s0, s1, range);
      mn = range.min;
      mx = range.max;
    } else {
      mn = ys[s0];
      mx = mn;
      for (let j = s0 + 1; j < s1; j++) {
        const v = ys[j];
        if (v < mn) mn = v;
        else if (v > mx) mx = v;
      }
    }
    if (i % 2 === 0) {
      out[2 * i] = mn;
      out[2 * i + 1] = mx;
    } else {
      out[2 * i] = mx;
      out[2 * i + 1] = mn;
    }
  }
}
