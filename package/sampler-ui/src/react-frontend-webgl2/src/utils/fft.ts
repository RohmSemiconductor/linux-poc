import FFT from "fft.js";

export const MAX_FFT_SIZE = 16384;
export const FFT_DB_FLOOR = -120;

export interface FFTResult {
  /** Magnitude in dB for each frequency bin [0 .. N/2], length = fftSize/2 + 1. */
  magnitudes: Float32Array;
  /** Width of one frequency bin in Hz. */
  freqBinHz: number;
  /** Number of valid bins (= fftSize/2 + 1). */
  binCount: number;
}

// Cache the FFT instance so we don't pay table-construction cost every frame.
let _fft: FFT | null = null;
let _fftSize = 0;
let _complexOut: number[] | null = null;

function getFFT(size: number): { fft: FFT; complexOut: number[] } {
  if (_fftSize !== size || _fft === null || _complexOut === null) {
    _fft = new FFT(size);
    _complexOut = _fft.createComplexArray();
    _fftSize = size;
  }
  return { fft: _fft, complexOut: _complexOut };
}

/**
 * Compute FFT magnitude spectrum of the most recent samples in `ys`.
 *
 * - Uses `realTransform` (real input, ~40% faster than complex).
 * - FFT size is the largest power-of-two <= min(count, MAX_FFT_SIZE).
 * - Takes the LAST `fftSize` samples so the spectrum reflects the newest data.
 * - Magnitudes are in dBFS: 20*log10(|bin| / fftSize), floored at FFT_DB_FLOOR.
 *
 * @param ys       Raw ADC sample buffer (Float32Array, uint16 values).
 * @param count    Number of valid samples in `ys`.
 * @param sampleRate  Sample rate in Hz (used to compute freqBinHz).
 * @param windowFn Optional per-sample weight function (index, fftSize) => weight.
 *                 Defaults to rectangular window (all weights = 1).
 */
export function computeFFT(
  ys: Float32Array,
  count: number,
  sampleRate: number,
  adcMax: number,
  windowFn?: (i: number, n: number) => number,
): FFTResult | null {
  if (count < 2) return null;

  // Largest power-of-two that fits in the available samples.
  const raw = Math.min(count, MAX_FFT_SIZE);
  let fftSize = 1;
  while (fftSize * 2 <= raw) fftSize *= 2;
  // fft.js requires size > 1
  if (fftSize < 2) return null;

  const { fft, complexOut } = getFFT(fftSize);

  // Build real input array from the LAST fftSize samples.
  const offset = count - fftSize;
  const realIn = new Array<number>(fftSize);
  for (let i = 0; i < fftSize; i++) {
    const w = windowFn ? windowFn(i, fftSize) : 1;
    realIn[i] = (ys[offset + i] / adcMax) * w;
  }

  fft.realTransform(complexOut, realIn);
  // fft.completeSpectrum(complexOut);

  const binCount = fftSize / 2 + 1;
  const magnitudes = new Float32Array(binCount);
  for (let k = 0; k < binCount; k++) {
    const re = complexOut[2 * k];
    const im = complexOut[2 * k + 1];
    const linear = Math.sqrt(re * re + im * im) / fftSize;
    const db = linear > 0 ? 20 * Math.log10(linear) : FFT_DB_FLOOR;
    magnitudes[k] = Math.max(db, FFT_DB_FLOOR);
  }

  return {
    magnitudes,
    freqBinHz: sampleRate / fftSize,
    binCount,
  };
}

/** Hann window weight — use as the `windowFn` argument to reduce spectral leakage. */
export function hannWindow(i: number, n: number): number {
  return 0.5 * (1 - Math.cos((2 * Math.PI * i) / (n - 1)));
}
