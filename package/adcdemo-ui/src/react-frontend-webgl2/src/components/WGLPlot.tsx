import { useEffect, useRef } from "react";
import type { PlotData, View, PlotModes, YScale } from "../types";
import {
  PAD,
  drawAxes,
  drawCrosshair,
  drawOverlays,
  drawFFTAxes,
  drawFFTCrosshair,
  drawHistogramAxes,
  drawHistogramCrosshair,
} from "../utils/plotDraw";
import { getThemeColors } from "../utils/themeColors";
import {
  computeFFT,
  createFFTWorkspace,
  FFT_DB_FLOOR,
} from "../utils/fft";
import type { FFTResult } from "../utils/fft";
import { computeHistogram, updateHistogram } from "../utils/histogram";
import type { HistogramResult } from "../utils/histogram";
import {
  buildMinMaxDecimation,
  MinMaxPyramid,
} from "../utils/decimate";
import { saveCanvasesAsImage } from "../utils/saveImage";
import {
  clampFFTView,
  clampHistView,
  createWGLPlotCanvases,
  createWGLPlotWebGLContext,
  drawWGLPlotSeries,
  disposeWGLPlotWebGL,
  initializeWGLPlotWebGL,
} from "../utils/WGLPLotUtils";
import { createWGLPlotInteractionHandlers } from "./WGLPlotInteractions";
import type { WGLPlotInteractionState } from "./WGLPlotInteractions";

/* ── component ───────────────────────────────────────────────────────────── */

const TIME_COLOR = new Float32Array([57 / 255, 255 / 255, 110 / 255]);
const FFT_COLOR = new Float32Array([255 / 255, 200 / 255, 50 / 255]);
const HISTOGRAM_COLOR = new Float32Array([100 / 255, 200 / 255, 255 / 255]);

interface WGLPlotProps {
  id: string;
  dataRef: React.RefObject<PlotData>;
  saveRequestRef?: React.RefObject<number>;
  height?: number | string;
  style?: React.CSSProperties;
  sampleRate?: number;
  actualSampleRate?: number | null;
  adcMax?: number;
  live?: boolean;
  windowSize?: number;
  plotMode: PlotModes;
  y_scale: YScale;
  voltageRef: number;
  /** When true, always show the full data range and ignore zoom/pan. */
  fitAll?: boolean;
  onWheel?: (e: React.WheelEvent<HTMLDivElement>) => void;
  /** Optional ref that WGLPlot keeps up-to-date with the current visible view (sample-index space). */
  viewRef?: React.RefObject<View>;
}

export function WGLPlot({
  id,
  dataRef,
  saveRequestRef,
  height = 400,
  style,
  sampleRate = 1,
  actualSampleRate = null,
  adcMax = 65535,
  live = false,
  windowSize = 50_000,
  fitAll = false,
  plotMode = "time",
  y_scale = "raw",
  voltageRef = 3.3,
  onWheel,
  viewRef: externalViewRef,
}: WGLPlotProps) {
  const wrapRef = useRef<HTMLDivElement>(null);
  const _internalViewRef = useRef<View>({ xMin: 0, xMax: 1 });
  const viewRef = externalViewRef ?? _internalViewRef;
  const autoFitRef = useRef(true);
  const wasLiveRef = useRef(false);
  const plotPropsRef = useRef({
    live,
    fitAll,
    windowSize,
    sampleRate,
    actualSampleRate,
    adcMax,
    plotMode,
    y_scale,
    voltageRef,
  });
  const invalidatePlotRef = useRef<
    ((resetDerivedState: boolean, currentLive: boolean) => void) | null
  >(null);
  const fftViewRef = useRef<View>({ xMin: 0, xMax: -1 }); // xMax=-1 = uninitialized
  const histViewRef = useRef<View>({ xMin: 0, xMax: -1 });

  useEffect(() => {
    const previous = plotPropsRef.current;
    const current = {
      live,
      fitAll,
      windowSize,
      sampleRate,
      actualSampleRate,
      adcMax,
      plotMode,
      y_scale,
      voltageRef,
    };
    plotPropsRef.current = current;

    const resetDerivedState =
      previous.live !== current.live ||
      previous.sampleRate !== current.sampleRate ||
      previous.adcMax !== current.adcMax ||
      previous.plotMode !== current.plotMode ||
      previous.y_scale !== current.y_scale ||
      previous.voltageRef !== current.voltageRef;
    if (resetDerivedState) {
      invalidatePlotRef.current?.(true, current.live);
    } else if (previous.actualSampleRate !== current.actualSampleRate) {
      invalidatePlotRef.current?.(false, current.live);
    }
  }, [
    live,
    fitAll,
    windowSize,
    sampleRate,
    actualSampleRate,
    adcMax,
    plotMode,
    y_scale,
    voltageRef,
  ]);

  useEffect(() => {
    const _wrap = wrapRef.current;
    if (!_wrap) return;
    const wrap: HTMLDivElement = _wrap;

    let W = 0,
      H = 0;
    let dirty = true;
    let hoverDirty = true;
    const {
      glCanvas,
      axisCanvas,
      hoverCanvas,
      dpr,
      resizeObserver: ro,
    } = createWGLPlotCanvases(wrap, (width, height) => {
      W = width;
      H = height;
      dirty = true;
      hoverDirty = true;
    });

    /* ── WebGL2 setup ────────────────────────────────────────────────── */
    const _gl = createWGLPlotWebGLContext(glCanvas);
    if (!_gl) {
      const message = document.createElement("p");
      message.textContent = "WebGL2 is required but not supported.";
      Object.assign(message.style, {
        color: "salmon",
        padding: "1rem",
        fontFamily: "monospace",
      });
      wrap.appendChild(message);
      return () => {
        ro.disconnect();
        message.remove();
        glCanvas.remove();
        axisCanvas.remove();
        hoverCanvas.remove();
      };
    }
    const gl: WebGL2RenderingContext = _gl;
    const colors = getThemeColors();

    const resources = initializeWGLPlotWebGL(gl);
    const { program: prog, locations: locs } = resources;
    const { vao, vbo } = resources.sample;
    const { vao: fftVao, vbo: fftVbo } = resources.fft;
    const { vao: histVao, vbo: histVbo } = resources.histogram;
    const { vao: decimVao, vbo: decimVbo } = resources.decimation;
    let sampleGpuCapacity = resources.sample.capacity;
    let fftGpuCapacity = resources.fft.capacity;
    let histGpuCapacity = resources.histogram.capacity;
    let decimGpuCapacity = resources.decimation.capacity;

    let lastFFTResult: FFTResult | null = null;
    let lastFFTInput: {
      ys: Float32Array;
      count: number;
      sampleRate: number;
      adcMax: number;
    } | null = null;
    const fftWorkspace = createFFTWorkspace();

    let lastHistResult: HistogramResult | null = null;
    let histUploadedCount = -1;
    let lastDataSource = dataRef.current;
    let lastDataGeneration = lastDataSource.generation;
    let lastDataCount = lastDataSource.count;
    let lastDataBuffer = lastDataSource.ys;

    /* view state (sample-index space) */
    let view: View = viewRef.current;
    let autoFit = wasLiveRef.current && !plotPropsRef.current.live
      ? false
      : autoFitRef.current;
    wasLiveRef.current = plotPropsRef.current.live;

    /* FFT view state (bin-index space). Always start at full view when (re-)entering FFT mode. */
    let fftView: View = { xMin: 0, xMax: 0 };
    let fftAutoFit = true;

    /* Histogram view state (bin-index space). */
    let histView: View = { xMin: 0, xMax: 0 };
    let histAutoFit = true;

    const interactionState: WGLPlotInteractionState = {
      get view() {
        return view;
      },
      set view(next: View) {
        view = next;
      },
      get autoFit() {
        return autoFit;
      },
      set autoFit(next: boolean) {
        autoFit = next;
      },
      get fftView() {
        return fftView;
      },
      set fftView(next: View) {
        fftView = next;
      },
      get fftAutoFit() {
        return fftAutoFit;
      },
      set fftAutoFit(next: boolean) {
        fftAutoFit = next;
      },
      get histView() {
        return histView;
      },
      set histView(next: View) {
        histView = next;
      },
      get histAutoFit() {
        return histAutoFit;
      },
      set histAutoFit(next: boolean) {
        histAutoFit = next;
      },
    };
    const interactions = createWGLPlotInteractionHandlers({
      canvas: glCanvas,
      dpr,
      getCanvasWidth: () => W,
      getPlotProps: () => plotPropsRef.current,
      getSampleCount: () => dataRef.current.count,
      getFFTBinCount: () => lastFFTResult?.binCount ?? null,
      getHistogramBinCount: () => lastHistResult?.binCount ?? null,
      state: interactionState,
      viewRef,
      fftViewRef,
      histViewRef,
      autoFitRef,
      requestRender: () => {
        dirty = true;
      },
      requestHoverRender: () => {
        hoverDirty = true;
      },
    });

    invalidatePlotRef.current = (resetDerivedState, currentLive) => {
      if (resetDerivedState) {
        autoFit =
          wasLiveRef.current && !currentLive
            ? false
            : autoFitRef.current;
        wasLiveRef.current = currentLive;
        fftView = { xMin: 0, xMax: 0 };
        fftAutoFit = true;
        histView = { xMin: 0, xMax: 0 };
        histAutoFit = true;
        lastFFTResult = null;
        lastFFTInput = null;
        lastHistResult = null;
        histUploadedCount = -1;
        interactions.reset();
      }
      dirty = true;
      hoverDirty = true;
    };

    let decimBuf: Float32Array | null = null;
    let decimationCache: {
      source: PlotData;
      ys: Float32Array;
      generation: number;
      drawFirst: number;
      drawCount: number;
      numPairs: number;
    } | null = null;
    const minMaxPyramid = new MinMaxPyramid();

    const _ctx2 = axisCanvas.getContext("2d");
    const _ctxHover = hoverCanvas.getContext("2d");
    if (!_ctx2 || !_ctxHover) throw new Error("Failed to get 2D contexts");
    const ctx2: CanvasRenderingContext2D = _ctx2;
    const ctxHover: CanvasRenderingContext2D = _ctxHover;

    glCanvas.addEventListener("wheel", interactions.onWheel, {
      passive: false,
    });
    glCanvas.addEventListener("mousedown", interactions.onMouseDown);
    glCanvas.addEventListener("mousemove", interactions.onMouseMove);
    glCanvas.addEventListener("mouseup", interactions.onMouseUp);
    glCanvas.addEventListener("mouseleave", interactions.onMouseLeave);
    glCanvas.addEventListener("dblclick", interactions.onDblClick);

    /* ── RAF render loop ─────────────────────────────────────────────── */
    let rafId: number;
    let lastOverlayActive = false;
    let lastOverlayPulse = false;

    function frame() {
      const currentProps = plotPropsRef.current;
      const captureRequested = (saveRequestRef?.current ?? 0) > 0;
      if (captureRequested) {
        dirty = true;
        hoverDirty = true;
      }
      const source = dataRef.current;
      const { ys, count } = source;
      const bufferChanged = ys !== lastDataBuffer;
      const sourceChanged =
        source !== lastDataSource ||
        source.generation !== lastDataGeneration;
      const countDecreased = count < lastDataCount;
      const bufferReplacedWithoutAppending =
        bufferChanged && count === lastDataCount;

      if (sourceChanged || countDecreased || bufferReplacedWithoutAppending) {
        minMaxPyramid.reset();
        decimationCache = null;
        fftAutoFit = true;
        lastFFTResult = null;
        lastFFTInput = null;
        histAutoFit = true;
        lastHistResult = null;
        histUploadedCount = -1;
        dirty = true;
        hoverDirty = true;
      }
      if (count !== lastDataCount || bufferChanged) dirty = true;
      lastDataSource = source;
      lastDataGeneration = source.generation;
      lastDataCount = count;
      lastDataBuffer = ys;

      if (currentProps.plotMode === "time") {
        minMaxPyramid.update(ys, count);
      }

      /* keep view in sync with streaming / auto-fit */
      if (count >= 2) {
        if (currentProps.live && currentProps.fitAll) {
          if (view.xMin !== 0 || view.xMax !== count - 1) {
            view = { xMin: 0, xMax: count - 1 };
            viewRef.current = view;
            dirty = true;
          }
        } else if (currentProps.live) {
          const newMin = Math.max(0, count - currentProps.windowSize);
          const newMax = count - 1;
          if (view.xMin !== newMin || view.xMax !== newMax) {
            view = { xMin: newMin, xMax: newMax };
            viewRef.current = view;
            dirty = true;
          }
        } else if (autoFit) {
          if (view.xMin !== 0 || view.xMax !== count - 1) {
            view = { xMin: 0, xMax: count - 1 };
            viewRef.current = view;
            dirty = true;
          }
        }
      }

      if (dirty && W > 0 && H > 0) {
        dirty = false;
        hoverDirty = true;

        gl.clearColor(0, 0, 0, 0);
        gl.clear(gl.COLOR_BUFFER_BIT);

        if (count >= 2) {
          const pl = PAD.l * dpr,
            pb = PAD.b * dpr,
            pt = PAD.t * dpr,
            pr = PAD.r * dpr;
          const pw = W - pl - pr;
          const viewport = {
            x: pl,
            y: pb,
            width: pw,
            height: H - pt - pb,
          };

          gl.useProgram(prog);
          switch (currentProps.plotMode) {
            case "time":
              {
                const { xMin, xMax } = view;

                const drawFirst = Math.max(0, Math.floor(xMin));
                const drawLast = Math.min(count - 1, Math.ceil(xMax));
                const drawCount = drawLast - drawFirst + 1;
                if (drawCount > 0) {
                  if (drawCount > pw) {
                    // More than 1 sample per device pixel — decimate to min/max pairs.
                    const numPairs = Math.max(2, Math.floor(pw));
                    const needed = numPairs * 2;
                    const cached = decimationCache;
                    if (
                      !cached ||
                      cached.source !== source ||
                      cached.ys !== ys ||
                      cached.generation !== source.generation ||
                      cached.drawFirst !== drawFirst ||
                      cached.drawCount !== drawCount ||
                      cached.numPairs !== numPairs
                    ) {
                      if (!decimBuf || decimBuf.length < needed)
                        decimBuf = new Float32Array(needed);
                      buildMinMaxDecimation(
                        ys,
                        drawFirst,
                        drawCount,
                        numPairs,
                        decimBuf,
                        minMaxPyramid,
                      );
                      if (needed > decimGpuCapacity) {
                        decimGpuCapacity = Math.max(
                          needed,
                          decimGpuCapacity * 2,
                        );
                        gl.bindBuffer(gl.ARRAY_BUFFER, decimVbo);
                        gl.bufferData(
                          gl.ARRAY_BUFFER,
                          decimGpuCapacity * 4,
                          gl.DYNAMIC_DRAW,
                        );
                      }
                      gl.bindBuffer(gl.ARRAY_BUFFER, decimVbo);
                      gl.bufferSubData(
                        gl.ARRAY_BUFFER,
                        0,
                        decimBuf,
                        0,
                        needed,
                      );
                      decimationCache = {
                        source,
                        ys,
                        generation: source.generation,
                        drawFirst,
                        drawCount,
                        numPairs,
                      };
                    }
                    drawWGLPlotSeries(gl, locs, {
                      viewport,
                      xMin: 0,
                      xMax: needed - 1,
                      yMin: 0,
                      yMax: currentProps.adcMax,
                      color: TIME_COLOR,
                      vao: decimVao,
                      vertexCount: needed,
                    });
                  } else {
                    if (drawCount > sampleGpuCapacity) {
                      sampleGpuCapacity = Math.max(
                        drawCount,
                        sampleGpuCapacity * 2,
                      );
                      gl.bindBuffer(gl.ARRAY_BUFFER, vbo);
                      gl.bufferData(
                        gl.ARRAY_BUFFER,
                        sampleGpuCapacity * 4,
                        gl.DYNAMIC_DRAW,
                      );
                    }
                    gl.bindBuffer(gl.ARRAY_BUFFER, vbo);
                    gl.bufferSubData(
                      gl.ARRAY_BUFFER,
                      0,
                      ys,
                      drawFirst,
                      drawCount,
                    );
                    drawWGLPlotSeries(gl, locs, {
                      viewport,
                      xMin: xMin - drawFirst,
                      xMax: xMax - drawFirst,
                      yMin: 0,
                      yMax: currentProps.adcMax,
                      color: TIME_COLOR,
                      vao,
                      vertexCount: drawCount,
                    });
                  }
                }
              }

              break;

            case "fft": {
              const effectiveSampleRate =
                currentProps.actualSampleRate ?? currentProps.sampleRate;
              if (
                !lastFFTInput ||
                lastFFTInput.ys !== ys ||
                lastFFTInput.count !== count ||
                lastFFTInput.sampleRate !== effectiveSampleRate ||
                lastFFTInput.adcMax !== currentProps.adcMax
              ) {
                lastFFTInput = {
                  ys,
                  count,
                  sampleRate: effectiveSampleRate,
                  adcMax: currentProps.adcMax,
                };
                lastFFTResult = computeFFT(
                  ys,
                  count,
                  effectiveSampleRate,
                  currentProps.adcMax,
                  undefined,
                  fftWorkspace,
                );
                if (lastFFTResult) {
                  const { magnitudes } = lastFFTResult;
                  if (magnitudes.length > fftGpuCapacity) {
                    fftGpuCapacity = Math.max(
                      magnitudes.length,
                      fftGpuCapacity * 2,
                    );
                    gl.bindBuffer(gl.ARRAY_BUFFER, fftVbo);
                    gl.bufferData(
                      gl.ARRAY_BUFFER,
                      fftGpuCapacity * 4,
                      gl.DYNAMIC_DRAW,
                    );
                  }
                  gl.bindBuffer(gl.ARRAY_BUFFER, fftVbo);
                  gl.bufferSubData(gl.ARRAY_BUFFER, 0, magnitudes);
                }
              }
              if (lastFFTResult) {
                const { binCount } = lastFFTResult;
                // Auto-fit: always show full spectrum until user zooms/pans
                if (fftAutoFit) {
                  fftView = { xMin: 0, xMax: binCount - 1 };
                  fftViewRef.current = fftView;
                } else if (fftView.xMax >= binCount) {
                  fftView = clampFFTView(fftView, binCount);
                  fftViewRef.current = fftView;
                }

                drawWGLPlotSeries(gl, locs, {
                  viewport,
                  xMin: fftView.xMin,
                  xMax: fftView.xMax,
                  yMin: FFT_DB_FLOOR,
                  yMax: 0,
                  color: FFT_COLOR,
                  vao: fftVao,
                  vertexCount: binCount,
                });
              }
              break;
            }

            case "histogram": {
              if (count !== histUploadedCount) {
                if (histUploadedCount === -1 || !lastHistResult) {
                  lastHistResult = computeHistogram(
                    ys,
                    count,
                    currentProps.adcMax,
                  );
                } else {
                  updateHistogram(lastHistResult, ys, histUploadedCount, count);
                }
                if (lastHistResult) {
                  histUploadedCount = count;
                  if (lastHistResult.counts.length > histGpuCapacity) {
                    histGpuCapacity = Math.max(
                      lastHistResult.counts.length,
                      histGpuCapacity * 2,
                    );
                    gl.bindBuffer(gl.ARRAY_BUFFER, histVbo);
                    gl.bufferData(
                      gl.ARRAY_BUFFER,
                      histGpuCapacity * 4,
                      gl.DYNAMIC_DRAW,
                    );
                  }
                  gl.bindBuffer(gl.ARRAY_BUFFER, histVbo);
                  gl.bufferSubData(gl.ARRAY_BUFFER, 0, lastHistResult.counts);
                }
              }
              if (lastHistResult) {
                const { binCount } = lastHistResult;
                if (histAutoFit) {
                  histView = { xMin: 0, xMax: binCount - 1 };
                  histViewRef.current = histView;
                } else if (histView.xMax >= binCount) {
                  histView = clampHistView(histView, binCount);
                  histViewRef.current = histView;
                }
                drawWGLPlotSeries(gl, locs, {
                  viewport,
                  xMin: histView.xMin,
                  xMax: histView.xMax,
                  yMin: 0,
                  yMax: lastHistResult.yMax,
                  color: HISTOGRAM_COLOR,
                  vao: histVao,
                  vertexCount: binCount,
                });
              }
              break;
            }

            default:
              break;
          }
        }

        if (currentProps.plotMode === "time") {
          drawAxes(
            ctx2,
            W,
            H,
            dpr,
            count,
            count >= 2 ? view.xMin : 0,
            count >= 2 ? view.xMax : 1,
            currentProps.adcMax,
            colors,
            source.chunkUsecs,
            source.chunkCount,
            currentProps.y_scale,
            currentProps.voltageRef,
          );
        } else if (currentProps.plotMode === "fft") {
          const fftAxesResult: FFTResult = lastFFTResult ?? {
            magnitudes: new Float32Array(0),
            freqBinHz:
              (currentProps.actualSampleRate ?? currentProps.sampleRate) / 2,
            binCount: 2,
          };
          const axisXMin = lastFFTResult ? fftView.xMin : 0;
          const axisXMax = lastFFTResult
            ? fftView.xMax
            : fftAxesResult.binCount - 1;
          drawFFTAxes(
            ctx2,
            W,
            H,
            dpr,
            fftAxesResult,
            axisXMin,
            axisXMax,
            colors,
          );
        } else if (currentProps.plotMode === "histogram") {
          const histBinCount = Math.min(currentProps.adcMax + 1, 4096);
          const histAxesResult: HistogramResult = lastHistResult ?? {
            counts: new Float32Array(0),
            binCount: histBinCount,
            binWidth: (currentProps.adcMax + 1) / histBinCount,
            maxCount: 1,
            yMax: 1,
          };
          const axisXMin = lastHistResult ? histView.xMin : 0;
          const axisXMax = lastHistResult
            ? histView.xMax
            : histAxesResult.binCount - 1;
          drawHistogramAxes(
            ctx2,
            W,
            H,
            dpr,
            histAxesResult,
            axisXMin,
            axisXMax,
            currentProps.adcMax,
            currentProps.y_scale,
            currentProps.voltageRef,
            colors,
          );
        }
      }

      const now = performance.now();
      const overlayActive =
        currentProps.actualSampleRate !== null &&
        currentProps.actualSampleRate !== currentProps.sampleRate;
      const overlayPulse = overlayActive && Math.sin(now / 300) > 0;
      if (
        overlayActive !== lastOverlayActive ||
        (overlayActive && overlayPulse !== lastOverlayPulse)
      ) {
        hoverDirty = true;
      }
      lastOverlayActive = overlayActive;
      lastOverlayPulse = overlayPulse;

      if (W > 0 && H > 0) {
        if (hoverDirty) {
          if (currentProps.plotMode === "time") {
            drawCrosshair(
              ctxHover,
              W,
              H,
              dpr,
              ys,
              count,
              view,
              interactions.hoverPhys,
              currentProps.sampleRate,
              currentProps.adcMax,
              colors,
              source.chunkUsecs,
              source.chunkCount,
              currentProps.y_scale,
              currentProps.voltageRef,
            );
          } else if (currentProps.plotMode === "fft" && lastFFTResult) {
            drawFFTCrosshair(
              ctxHover,
              W,
              H,
              dpr,
              lastFFTResult,
              interactions.hoverPhys,
              fftView.xMin,
              fftView.xMax,
              colors,
            );
          } else if (
            currentProps.plotMode === "histogram" &&
            lastHistResult
          ) {
            drawHistogramCrosshair(
              ctxHover,
              W,
              H,
              dpr,
              lastHistResult,
              interactions.hoverPhys,
              histView.xMin,
              histView.xMax,
              currentProps.adcMax,
              currentProps.y_scale,
              currentProps.voltageRef,
              colors,
            );
          } else {
            ctxHover.clearRect(0, 0, W, H);
          }
          drawOverlays(
            ctxHover,
            W,
            H,
            dpr,
            currentProps.actualSampleRate,
            currentProps.sampleRate,
            colors,
            now,
          );
          hoverDirty = false;
        }
      }

      if (captureRequested && W > 0 && H > 0 && saveRequestRef) {
        saveRequestRef.current--;
        saveCanvasesAsImage(id);
      }

      rafId = requestAnimationFrame(frame);
    }

    rafId = requestAnimationFrame(frame);

    /* ── cleanup ─────────────────────────────────────────────────────── */
    return () => {
      cancelAnimationFrame(rafId);
      ro.disconnect();
      glCanvas.removeEventListener("wheel", interactions.onWheel);
      glCanvas.removeEventListener("mousedown", interactions.onMouseDown);
      glCanvas.removeEventListener("mousemove", interactions.onMouseMove);
      glCanvas.removeEventListener("mouseup", interactions.onMouseUp);
      glCanvas.removeEventListener("mouseleave", interactions.onMouseLeave);
      glCanvas.removeEventListener("dblclick", interactions.onDblClick);
      disposeWGLPlotWebGL(gl, resources);
      invalidatePlotRef.current = null;
      glCanvas.remove();
      axisCanvas.remove();
      hoverCanvas.remove();
    };
  }, [dataRef, viewRef, id, saveRequestRef]);

  return (
    <div
      id={id}
      ref={wrapRef}
      className="relative w-full overflow-hidden rounded-sm bg-card"
      style={{ height, ...style }}
      onWheel={onWheel}
    />
  );
}
