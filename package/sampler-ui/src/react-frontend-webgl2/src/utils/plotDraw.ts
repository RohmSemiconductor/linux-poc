import type { View, HoverPhys, YScale } from "../types";
import type { ThemeColors } from "./themeColors";
import { MAX_SAMPS } from "../config/constants";
import { FFT_DB_FLOOR, type FFTResult } from "./fft";

export const PAD = { l: 62, b: 34, t: 10, r: 12 } as const;

/**
 * Interpolate a sample index to a microsecond timestamp using chunk boundaries.
 * Returns null when no chunk data is available.
 */
function sampleToUsecs(
  idx: number,
  chunkUsecs: Float64Array,
  chunkCount: number,
): number | null {
  if (chunkCount === 0) return null;
  const ci = Math.floor(idx / MAX_SAMPS);
  const frac = (idx % MAX_SAMPS) / MAX_SAMPS;

  if (ci < 0) {
    if (chunkCount >= 2) {
      const rate = chunkUsecs[1] - chunkUsecs[0];
      return chunkUsecs[0] + ci * rate + frac * rate;
    }
    return chunkUsecs[0];
  }
  if (ci + 1 < chunkCount) {
    return chunkUsecs[ci] + frac * (chunkUsecs[ci + 1] - chunkUsecs[ci]);
  }
  if (ci < chunkCount) {
    if (chunkCount >= 2) {
      const rate = chunkUsecs[chunkCount - 1] - chunkUsecs[chunkCount - 2];
      return chunkUsecs[ci] + frac * rate;
    }
    return chunkUsecs[0];
  }
  // beyond last chunk — extrapolate
  if (chunkCount >= 2) {
    const rate = chunkUsecs[chunkCount - 1] - chunkUsecs[chunkCount - 2];
    return chunkUsecs[chunkCount - 1] + (ci - chunkCount + 1 + frac) * rate;
  }
  return chunkUsecs[0];
}

function timeUnit(rangeUs: number): [number, string] {
  if (rangeUs < 1000) return [1, "µs"];
  if (rangeUs < 1_000_000) return [1e-3, "ms"];
  return [1e-6, "s"];
}

function formatTime(us: number, mult: number, unit: string): string {
  return (us * mult).toFixed(2).replace(/\.?0+$/, "") + " " + unit;
}

function formatSampleIdx(idx: number): string {
  const abs = Math.abs(idx);
  if (abs >= 1e6) return (idx / 1e6).toFixed(2) + "M";
  if (abs >= 1e3) return (idx / 1e3).toFixed(1) + "k";
  return Math.round(idx).toString();
}

function niceStep(range: number, targetCount: number): number {
  if (targetCount < 1) targetCount = 1;
  const rough = range / targetCount;
  const mag = Math.pow(10, Math.floor(Math.log10(rough)));
  const norm = rough / mag;
  const nice = norm < 1.5 ? 1 : norm < 3.5 ? 2 : norm < 7.5 ? 5 : 10;
  return nice * mag;
}

/** Round value UP to the nearest 1/2/5/10... nice number. */
function niceValue(value: number): number {
  if (value <= 0) return 1;
  const mag = Math.pow(10, Math.floor(Math.log10(value)));
  const norm = value / mag;
  const nice = norm <= 1 ? 1 : norm <= 2 ? 2 : norm <= 5 ? 5 : 10;
  return nice * mag;
}

function drawGrid(
  ctx: CanvasRenderingContext2D,
  W: number,
  H: number,
  dpr: number,
  xMin: number,
  xMax: number,
  xStep: number,
  colors: ThemeColors,
): void {
  const pl = PAD.l * dpr,
    pb = PAD.b * dpr,
    pt = PAD.t * dpr,
    pr = PAD.r * dpr;
  const pw = W - pl - pr;
  const ph = H - pt - pb;

  ctx.save();
  ctx.strokeStyle = colors.foreground40;
  ctx.lineWidth = dpr;
  ctx.setLineDash([3 * dpr, 4 * dpr]);

  // vertical lines — always in sample-index space so grid lines land on data points
  const xStart = Math.ceil(xMin / xStep) * xStep;
  for (let xi = xStart; xi <= xMax; xi += xStep) {
    const px = pl + ((xi - xMin) / (xMax - xMin)) * pw;
    ctx.beginPath();
    ctx.moveTo(px, pt);
    ctx.lineTo(px, pt + ph);
    ctx.stroke();
  }

  // horizontal lines — match the fixed y-labels [0, 0.25, 0.5, 0.75, 1]
  for (const f of [0, 0.25, 0.5, 0.75, 1]) {
    const py = pt + ph * (1 - f);
    ctx.beginPath();
    ctx.moveTo(pl, py);
    ctx.lineTo(pl + pw, py);
    ctx.stroke();
  }

  ctx.restore();
}

export function drawAxes(
  ctx: CanvasRenderingContext2D,
  W: number,
  H: number,
  dpr: number,
  count: number,
  xMin: number,
  xMax: number,
  adcMax: number,
  colors: ThemeColors,
  chunkUsecs: Float64Array,
  chunkCount: number,
  y_scale: YScale,
  voltage: number,
): void {
  const pl = PAD.l * dpr,
    pb = PAD.b * dpr,
    pt = PAD.t * dpr,
    pr = PAD.r * dpr;
  const pw = W - pl - pr;
  const ph = H - pt - pb;

  ctx.clearRect(0, 0, W, H);
  ctx.save();

  const xRange = Math.max(xMax - xMin, 1);

  // Compute grid x step here so labels align with grid lines
  const xStep = Math.max(
    1,
    Math.round(niceStep(xRange, Math.max(1, Math.floor(pw / (80 * dpr))))),
  );
  drawGrid(ctx, W, H, dpr, xMin, xMax, xStep, colors);

  ctx.strokeStyle = colors.border;
  ctx.lineWidth = dpr;
  ctx.strokeRect(pl, pt, pw, ph);

  const fs = 11 * dpr;
  ctx.font = `${fs}px monospace`;
  ctx.fillStyle = colors.foreground;
  ctx.strokeStyle = "black";
  ctx.lineWidth = 3 * dpr;
  ctx.lineJoin = "round";

  // Y-axis labels
  const yLabels = [0, 0.25, 0.5, 0.75, 1].map((f) => Math.round(f * adcMax));
  ctx.textAlign = "right";
  ctx.textBaseline = "middle";
  for (const v of yLabels) {
    const py = pt + ph * (1 - v / adcMax);

    const label =
      y_scale === "voltage"
        ? `${((v / adcMax) * voltage).toFixed(2)}V`
        : String(v);

    ctx.strokeText(label, pl - 5 * dpr, py);
    ctx.fillText(label, pl - 5 * dpr, py);
    ctx.beginPath();
    ctx.moveTo(pl - 3 * dpr, py);
    ctx.lineTo(pl, py);
    ctx.strokeStyle = colors.border;
    ctx.lineWidth = dpr;
    ctx.stroke();
    ctx.strokeStyle = "black";
    ctx.lineWidth = 3 * dpr;
  }

  // X-axis labels — Saleae-style: absolute time at major boundaries, relative offsets between
  ctx.textBaseline = "top";
  const baseUs = sampleToUsecs(0, chunkUsecs, chunkCount);
  const clampedXMin = Math.max(0, xMin);
  const clampedXMax = Math.min(count - 1, xMax);
  const tMinUs = sampleToUsecs(clampedXMin, chunkUsecs, chunkCount);
  const tMaxUs = sampleToUsecs(clampedXMax, chunkUsecs, chunkCount);
  const hasTime =
    chunkCount >= 2 &&
    baseUs !== null &&
    tMinUs !== null &&
    tMaxUs !== null &&
    tMaxUs > tMinUs;
  const xStart = Math.ceil(xMin / xStep) * xStep;

  if (hasTime) {
    // Estimate the time span of one grid step
    const xi0 = Math.max(0, Math.min(count - 1, xStart));
    const xi1 = Math.max(0, Math.min(count - 1, xStart + xStep));
    const stepUs =
      Math.abs(
        sampleToUsecs(xi1, chunkUsecs, chunkCount)! -
          sampleToUsecs(xi0, chunkUsecs, chunkCount)!,
      ) || 1;

    // Major interval: next nice value >= ~6x the grid step
    const majorUs = niceValue(stepUs * 6);
    const [absMult, absUnit] = timeUnit(majorUs);
    const [deltaMult, deltaUnit] = timeUnit(stepUs);

    let prevMajorIdx = -Infinity; // tracks which major segment we're in
    let anchorTAbs = 0; // actual timestamp of the last anchor grid line

    for (let xi = xStart; xi <= xMax; xi += xStep) {
      const px = pl + ((xi - xMin) / xRange) * pw;
      const clampedIdx = Math.max(0, Math.min(count - 1, xi));
      const tAbs = sampleToUsecs(clampedIdx, chunkUsecs, chunkCount)! - baseUs!;
      const majorIdx = Math.floor(tAbs / majorUs);
      const isAnchor = majorIdx !== prevMajorIdx;
      const majorBoundaryUs = majorIdx * majorUs;

      let label: string;
      const frac = (xi - xMin) / xRange;
      if (isAnchor) {
        // Anchor label: absolute time of the major boundary, e.g. "1.00 ms"
        // Drawn lower on the axis to visually separate it from offset labels
        label = formatTime(majorBoundaryUs, absMult, absUnit);
        prevMajorIdx = majorIdx;
        anchorTAbs = tAbs; // record actual time of this grid line as the offset base
      } else {
        // Offset label: time elapsed since the last anchor, e.g. "+10.00 µs"
        // Dimmed and drawn higher so anchors stand out
        const delta = tAbs - anchorTAbs;
        label = "+" + formatTime(delta, deltaMult, deltaUnit);
      }

      ctx.textAlign = frac < 0.05 ? "left" : frac > 0.95 ? "right" : "center";
      ctx.fillStyle = isAnchor ? colors.foreground : colors.foreground40;
      ctx.strokeStyle = "black";
      ctx.lineWidth = 3 * dpr;
      ctx.strokeText(label, px, pt + ph + (isAnchor ? 18 : 4) * dpr);
      ctx.fillText(label, px, pt + ph + (isAnchor ? 18 : 4) * dpr);
      ctx.fillStyle = colors.foreground;
      ctx.strokeStyle = colors.border;
      ctx.lineWidth = dpr;
      ctx.beginPath();
      ctx.moveTo(px, pt + ph);
      ctx.lineTo(px, pt + ph + (isAnchor ? 5 : 3) * dpr); // longer tick for anchors
      ctx.stroke();
    }
  } else {
    // Fallback: sample indices
    for (let xi = xStart; xi <= xMax; xi += xStep) {
      const px = pl + ((xi - xMin) / xRange) * pw;
      const frac = (xi - xMin) / xRange;
      ctx.textAlign = frac < 0.05 ? "left" : frac > 0.95 ? "right" : "center";
      ctx.strokeStyle = "black";
      ctx.lineWidth = 3 * dpr;
      ctx.strokeText(formatSampleIdx(xi), px, pt + ph + 4 * dpr);
      ctx.fillText(formatSampleIdx(xi), px, pt + ph + 4 * dpr);
      ctx.strokeStyle = colors.border;
      ctx.lineWidth = dpr;
      ctx.beginPath();
      ctx.moveTo(px, pt + ph);
      ctx.lineTo(px, pt + ph + 3 * dpr);
      ctx.stroke();
    }
  }

  ctx.restore();
}

export function drawOverlays(
  ctx: CanvasRenderingContext2D,
  W: number,
  H: number,
  dpr: number,
  actualSampleRate: number | null,
  sampleRate: number,
  colors: ThemeColors,
): void {
  const pl = PAD.l * dpr,
    pb = PAD.b * dpr,
    pt = PAD.t * dpr,
    pr = PAD.r * dpr;
  const pw = W - pl - pr;
  const ph = H - pt - pb;

  ctx.save();
  const fs = 11 * dpr;
  ctx.font = `${fs}px monospace`;
  ctx.lineJoin = "round";

  if (actualSampleRate !== null && actualSampleRate !== sampleRate) {
    // Actual sample rate mismatch warning — pulses red when rate differs from requested
    ctx.textAlign = "right";
    ctx.textBaseline = "bottom";
    ctx.strokeStyle = "black";
    ctx.lineWidth = 3 * dpr;
    const srLabel = `actual sample rate = ${actualSampleRate.toLocaleString()} Hz`;
    const rx = pl + pw - 4 * dpr;
    const ry = pt + ph - 2 * dpr;
    const pulse = 0.5 + 0.5 * Math.sin(performance.now() / 300);
    const fillColor = pulse > 0.5 ? colors.foreground : "#ff6060";
    ctx.strokeText(srLabel, rx, ry);
    ctx.fillStyle = fillColor;
    ctx.fillText(srLabel, rx, ry);
  }

  ctx.restore();
}

export function drawCrosshair(
  ctx: CanvasRenderingContext2D,
  W: number,
  H: number,
  dpr: number,
  ys: Float32Array,
  count: number,
  view: View,
  hoverPhys: HoverPhys,
  _sampleRate: number,
  adcMax: number,
  colors: ThemeColors,
  chunkUsecs: Float64Array,
  chunkCount: number,
  y_scale: YScale,
  voltage: number,
): void {
  const pl = PAD.l * dpr,
    _pb = PAD.b * dpr,
    pt = PAD.t * dpr,
    _pr = PAD.r * dpr;
  const pw = W - pl - _pr;
  const ph = H - pt - _pb;

  ctx.clearRect(0, 0, W, H);
  if (!hoverPhys || count < 2 || pw <= 0 || ph <= 0) return;

  const { x: hx, y: hy } = hoverPhys;
  if (hx < pl || hx > pl + pw || hy < pt || hy > pt + ph) return;

  // cursor → nearest sample
  const t = (hx - pl) / pw;
  const sampleF = view.xMin + t * (view.xMax - view.xMin);
  const sampleIdx = Math.round(Math.max(0, Math.min(count - 1, sampleF)));
  const yVal = ys[sampleIdx] ?? 0;

  // physical coords of the actual data point
  const dataX = pl + pw * ((sampleIdx - view.xMin) / (view.xMax - view.xMin));
  const dataY = pt + ph * (1 - yVal / adcMax);

  ctx.save();
  ctx.lineWidth = dpr;
  ctx.setLineDash([4 * dpr, 4 * dpr]);
  ctx.strokeStyle = colors.foreground20;

  // vertical line
  ctx.beginPath();
  ctx.moveTo(hx, pt);
  ctx.lineTo(hx, pt + ph);
  ctx.stroke();

  // horizontal line at actual y
  ctx.beginPath();
  ctx.moveTo(pl, dataY);
  ctx.lineTo(pl + pw, dataY);
  ctx.stroke();
  ctx.setLineDash([]);

  // dot at (sampleIdx, yVal)
  ctx.fillStyle = colors.foreground;
  ctx.strokeStyle = colors.foreground60;
  ctx.lineWidth = 1.5 * dpr;
  ctx.beginPath();
  ctx.arc(dataX, dataY, 4 * dpr, 0, Math.PI * 2);
  ctx.fill();
  ctx.stroke();

  // tooltip
  const fs = 11 * dpr;
  ctx.font = `${fs}px monospace`;
  const baseUs = sampleToUsecs(0, chunkUsecs, chunkCount);
  const tUs = sampleToUsecs(sampleIdx, chunkUsecs, chunkCount);
  let label: string;
  const yDisplay =
    y_scale === "voltage"
      ? `${((yVal / adcMax) * voltage).toFixed(3)}V`
      : String(Math.round(yVal));

  if (baseUs !== null && tUs !== null) {
    const relUs = tUs - baseUs;
    const [mult, unit] = timeUnit(relUs || 1);
    label = `t: ${formatTime(relUs, mult, unit)}  y: ${yDisplay}`;
  } else {
    label = `x: ${sampleIdx}  y: ${yDisplay}`;
  }
  const tw = ctx.measureText(label).width + 14 * dpr;
  const th = fs + 10 * dpr;
  let tx = hx + 14 * dpr;
  let ty = dataY - th;
  if (tx + tw > pl + pw) tx = hx - tw - 14 * dpr;
  ty = Math.max(pt + 2 * dpr, Math.min(pt + ph - th - 2 * dpr, ty));

  ctx.fillStyle = colors.popover90;
  ctx.strokeStyle = colors.border;
  ctx.lineWidth = dpr;
  ctx.fillRect(tx, ty, tw, th);
  ctx.strokeRect(tx, ty, tw, th);

  ctx.fillStyle = colors.popoverForeground;
  ctx.textAlign = "left";
  ctx.textBaseline = "middle";
  ctx.fillText(label, tx + 7 * dpr, ty + th / 2);

  ctx.restore();
}

function formatFreq(hz: number): string {
  if (hz >= 1_000_000)
    return (hz / 1_000_000).toFixed(2).replace(/\.?0+$/, "") + " MHz";
  if (hz >= 1_000)
    return (hz / 1_000).toFixed(2).replace(/\.?0+$/, "") + " kHz";
  return hz.toFixed(1).replace(/\.?0+$/, "") + " Hz";
}

export function drawFFTAxes(
  ctx: CanvasRenderingContext2D,
  W: number,
  H: number,
  dpr: number,
  result: FFTResult,
  xMinBin: number,
  xMaxBin: number,
  colors: ThemeColors,
): void {
  const pl = PAD.l * dpr,
    pb = PAD.b * dpr,
    pt = PAD.t * dpr,
    pr = PAD.r * dpr;
  const pw = W - pl - pr;
  const ph = H - pt - pb;

  ctx.clearRect(0, 0, W, H);
  if (pw <= 0 || ph <= 0 || xMaxBin <= xMinBin) return;
  ctx.save();

  const { freqBinHz } = result;
  const visMinHz = xMinBin * freqBinHz;
  const visMaxHz = xMaxBin * freqBinHz;
  const visRange = xMaxBin - xMinBin;
  const dbMin = FFT_DB_FLOOR;
  const dbMax = 0;
  const dbRange = dbMax - dbMin;

  // --- grid ---
  const xStepBins = Math.max(
    1,
    Math.round(niceStep(visRange, Math.max(1, Math.floor(pw / (80 * dpr))))),
  );
  ctx.strokeStyle = colors.foreground40;
  ctx.lineWidth = dpr;
  ctx.setLineDash([3 * dpr, 4 * dpr]);

  const xStart = Math.ceil(xMinBin / xStepBins) * xStepBins;
  for (let bi = xStart; bi <= xMaxBin; bi += xStepBins) {
    const px = pl + ((bi - xMinBin) / visRange) * pw;
    ctx.beginPath();
    ctx.moveTo(px, pt);
    ctx.lineTo(px, pt + ph);
    ctx.stroke();
  }
  // horizontal grid at every 20 dB
  for (let db = dbMin; db <= dbMax; db += 20) {
    const py = pt + ph * (1 - (db - dbMin) / dbRange);
    ctx.beginPath();
    ctx.moveTo(pl, py);
    ctx.lineTo(pl + pw, py);
    ctx.stroke();
  }
  ctx.setLineDash([]);

  // --- border ---
  ctx.strokeStyle = colors.border;
  ctx.lineWidth = dpr;
  ctx.strokeRect(pl, pt, pw, ph);

  const fs = 11 * dpr;
  ctx.font = `${fs}px monospace`;
  ctx.fillStyle = colors.foreground;
  ctx.strokeStyle = "black";
  ctx.lineWidth = 3 * dpr;
  ctx.lineJoin = "round";

  // --- Y-axis (dB) ---
  ctx.textAlign = "right";
  ctx.textBaseline = "middle";
  for (let db = dbMin; db <= dbMax; db += 20) {
    const py = pt + ph * (1 - (db - dbMin) / dbRange);
    const label = `${db} dB`;
    ctx.strokeText(label, pl - 5 * dpr, py);
    ctx.fillText(label, pl - 5 * dpr, py);
    ctx.beginPath();
    ctx.strokeStyle = colors.border;
    ctx.lineWidth = dpr;
    ctx.moveTo(pl - 3 * dpr, py);
    ctx.lineTo(pl, py);
    ctx.stroke();
    ctx.strokeStyle = "black";
    ctx.lineWidth = 3 * dpr;
  }

  // --- X-axis (frequency) ---
  ctx.textBaseline = "top";
  const targetSteps = Math.max(1, Math.floor(pw / (80 * dpr)));
  const hzStep = niceStep(visMaxHz - visMinHz, targetSteps);
  const firstHz = Math.ceil(visMinHz / hzStep) * hzStep;

  for (let hz = firstHz; hz <= visMaxHz + hzStep * 0.5; hz += hzStep) {
    const bi = hz / freqBinHz;
    const px = pl + ((bi - xMinBin) / visRange) * pw;
    if (px < pl - 1 || px > pl + pw + 1) continue;
    const frac = (px - pl) / pw;
    ctx.textAlign = frac < 0.05 ? "left" : frac > 0.95 ? "right" : "center";
    const label = formatFreq(hz);
    ctx.strokeStyle = "black";
    ctx.lineWidth = 3 * dpr;
    ctx.strokeText(label, px, pt + ph + 4 * dpr);
    ctx.fillStyle = colors.foreground;
    ctx.fillText(label, px, pt + ph + 4 * dpr);
    ctx.strokeStyle = colors.border;
    ctx.lineWidth = dpr;
    ctx.beginPath();
    ctx.moveTo(px, pt + ph);
    ctx.lineTo(px, pt + ph + 3 * dpr);
    ctx.stroke();
    ctx.strokeStyle = "black";
    ctx.lineWidth = 3 * dpr;
  }

  ctx.restore();
}

export function drawFFTCrosshair(
  ctx: CanvasRenderingContext2D,
  W: number,
  H: number,
  dpr: number,
  result: FFTResult,
  hoverPhys: HoverPhys,
  xMinBin: number,
  xMaxBin: number,
  colors: ThemeColors,
): void {
  const pl = PAD.l * dpr,
    pb = PAD.b * dpr,
    pt = PAD.t * dpr,
    pr = PAD.r * dpr;
  const pw = W - pl - pr;
  const ph = H - pt - pb;

  ctx.clearRect(0, 0, W, H);
  if (!hoverPhys || pw <= 0 || ph <= 0 || xMaxBin <= xMinBin) return;

  const { x: hx, y: hy } = hoverPhys;
  if (hx < pl || hx > pl + pw || hy < pt || hy > pt + ph) return;

  const { magnitudes, binCount, freqBinHz } = result;
  const dbMin = FFT_DB_FLOOR;
  const dbMax = 0;
  const dbRange = dbMax - dbMin;
  const visRange = xMaxBin - xMinBin;

  const t = (hx - pl) / pw;
  const binF = xMinBin + t * visRange;
  const bin = Math.round(Math.max(0, Math.min(binCount - 1, binF)));
  const db = magnitudes[bin] ?? dbMin;
  const hz = bin * freqBinHz;

  const dataX = pl + ((bin - xMinBin) / visRange) * pw;
  const dataY = pt + ph * (1 - (db - dbMin) / dbRange);

  ctx.save();
  ctx.lineWidth = dpr;
  ctx.setLineDash([4 * dpr, 4 * dpr]);
  ctx.strokeStyle = colors.foreground20;

  ctx.beginPath();
  ctx.moveTo(hx, pt);
  ctx.lineTo(hx, pt + ph);
  ctx.stroke();

  ctx.beginPath();
  ctx.moveTo(pl, dataY);
  ctx.lineTo(pl + pw, dataY);
  ctx.stroke();
  ctx.setLineDash([]);

  ctx.fillStyle = colors.foreground;
  ctx.strokeStyle = colors.foreground60;
  ctx.lineWidth = 1.5 * dpr;
  ctx.beginPath();
  ctx.arc(dataX, dataY, 4 * dpr, 0, Math.PI * 2);
  ctx.fill();
  ctx.stroke();

  const fs = 11 * dpr;
  ctx.font = `${fs}px monospace`;
  const label = `f: ${formatFreq(hz)}  |  ${db.toFixed(1)} dB`;
  const tw = ctx.measureText(label).width + 14 * dpr;
  const th = fs + 10 * dpr;
  let tx = hx + 14 * dpr;
  let ty = dataY - th;
  if (tx + tw > pl + pw) tx = hx - tw - 14 * dpr;
  ty = Math.max(pt + 2 * dpr, Math.min(pt + ph - th - 2 * dpr, ty));

  ctx.fillStyle = colors.popover90;
  ctx.strokeStyle = colors.border;
  ctx.lineWidth = dpr;
  ctx.fillRect(tx, ty, tw, th);
  ctx.strokeRect(tx, ty, tw, th);

  ctx.fillStyle = colors.popoverForeground;
  ctx.textAlign = "left";
  ctx.textBaseline = "middle";
  ctx.fillText(label, tx + 7 * dpr, ty + th / 2);

  ctx.restore();
}
