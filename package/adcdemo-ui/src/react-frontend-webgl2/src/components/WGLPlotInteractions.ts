import type { HoverPhys, PlotModes, View } from "../types";
import { MIN_VISIBLE, ZOOM_FACTOR } from "../config/constants";
import { PAD } from "../utils/plotDraw";
import {
  clampFFTView,
  clampHistView,
  clampView,
  tFromPhysX,
} from "../utils/WGLPLotUtils";

export interface WGLPlotInteractionState {
  view: View;
  autoFit: boolean;
  fftView: View;
  fftAutoFit: boolean;
  histView: View;
  histAutoFit: boolean;
}

interface WGLPlotInteractionProps {
  plotMode: PlotModes;
  live: boolean;
  fitAll: boolean;
}

interface MutableRef<T> {
  current: T;
}

interface WGLPlotInteractionOptions {
  canvas: HTMLCanvasElement;
  dpr: number;
  getCanvasWidth: () => number;
  getPlotProps: () => WGLPlotInteractionProps;
  getSampleCount: () => number;
  getFFTBinCount: () => number | null;
  getHistogramBinCount: () => number | null;
  state: WGLPlotInteractionState;
  viewRef: MutableRef<View>;
  fftViewRef: MutableRef<View>;
  histViewRef: MutableRef<View>;
  autoFitRef: MutableRef<boolean>;
  requestRender: () => void;
  requestHoverRender: () => void;
}

export interface WGLPlotInteractionHandlers {
  onWheel(event: WheelEvent): void;
  onMouseDown(event: MouseEvent): void;
  onMouseMove(event: MouseEvent): void;
  onMouseUp(): void;
  onMouseLeave(): void;
  onDblClick(): void;
  readonly hoverPhys: HoverPhys;
  reset(): void;
}

interface DragState {
  startClientX: number;
  startView: View;
}

function zoomViewAtCursor(
  view: View,
  cursorFraction: number,
  deltaY: number,
  count: number,
  clamp: (view: View, count: number) => View,
): View | null {
  const pivot = view.xMin + cursorFraction * (view.xMax - view.xMin);
  const factor = deltaY > 0 ? ZOOM_FACTOR : 1 / ZOOM_FACTOR;
  const xMin = pivot + (view.xMin - pivot) * factor;
  const xMax = pivot + (view.xMax - pivot) * factor;
  if (xMax - xMin < MIN_VISIBLE) return null;
  return clamp({ xMin, xMax }, count);
}

export function createWGLPlotInteractionHandlers(
  options: WGLPlotInteractionOptions,
): WGLPlotInteractionHandlers {
  const {
    canvas,
    dpr,
    getCanvasWidth,
    getPlotProps,
    getSampleCount,
    getFFTBinCount,
    getHistogramBinCount,
    state,
    viewRef,
    fftViewRef,
    histViewRef,
    autoFitRef,
    requestRender,
    requestHoverRender,
  } = options;
  let drag: DragState | null = null;
  let hoverPhys: HoverPhys = null;

  function getCursorFraction(clientX: number): number {
    const rect = canvas.getBoundingClientRect();
    return tFromPhysX(
      (clientX - rect.left) * dpr,
      getCanvasWidth(),
      dpr,
    );
  }

  function getPlotWidth(): number {
    return (getCanvasWidth() - (PAD.l + PAD.r) * dpr) / dpr;
  }

  function panView(
    startView: View,
    startClientX: number,
    clientX: number,
    count: number,
    clamp: (view: View, count: number) => View,
  ): View {
    const dx =
      ((startClientX - clientX) / getPlotWidth()) *
      (startView.xMax - startView.xMin);
    return clamp(
      {
        xMin: startView.xMin + dx,
        xMax: startView.xMax + dx,
      },
      count,
    );
  }

  function onWheel(event: WheelEvent): void {
    event.preventDefault();
    const currentPlotMode = getPlotProps().plotMode;
    if (currentPlotMode === "fft") {
      const binCount = getFFTBinCount();
      if (binCount === null) return;
      const nextView = zoomViewAtCursor(
        state.fftView,
        getCursorFraction(event.clientX),
        event.deltaY,
        binCount,
        clampFFTView,
      );
      if (!nextView) return;
      state.fftAutoFit = false;
      state.fftView = nextView;
      fftViewRef.current = nextView;
      requestRender();
      return;
    }
    if (currentPlotMode === "histogram") {
      const binCount = getHistogramBinCount();
      if (binCount === null) return;
      const nextView = zoomViewAtCursor(
        state.histView,
        getCursorFraction(event.clientX),
        event.deltaY,
        binCount,
        clampHistView,
      );
      if (!nextView) return;
      state.histAutoFit = false;
      state.histView = nextView;
      histViewRef.current = nextView;
      requestRender();
      return;
    }
    const currentProps = getPlotProps();
    if (currentProps.live && currentProps.fitAll) return;
    const count = getSampleCount();
    if (count < 2) return;

    const nextView = zoomViewAtCursor(
      state.view,
      getCursorFraction(event.clientX),
      event.deltaY,
      count,
      clampView,
    );
    if (!nextView) return;
    state.autoFit = false;
    state.view = nextView;
    autoFitRef.current = false;
    viewRef.current = nextView;
    requestRender();
  }

  function onMouseDown(event: MouseEvent): void {
    if (event.button !== 0) return;
    const currentPlotMode = getPlotProps().plotMode;
    drag = {
      startClientX: event.clientX,
      startView:
        currentPlotMode === "fft"
          ? { ...state.fftView }
          : currentPlotMode === "histogram"
            ? { ...state.histView }
            : { ...state.view },
    };
    hoverPhys = null;
    requestHoverRender();
    canvas.style.cursor = "grabbing";
  }

  function onMouseMove(event: MouseEvent): void {
    if (!drag) {
      const rect = canvas.getBoundingClientRect();
      const nextHover = {
        x: (event.clientX - rect.left) * dpr,
        y: (event.clientY - rect.top) * dpr,
      };
      if (
        !hoverPhys ||
        hoverPhys.x !== nextHover.x ||
        hoverPhys.y !== nextHover.y
      ) {
        hoverPhys = nextHover;
        requestHoverRender();
      }
    }
    if (!drag) return;

    const currentPlotMode = getPlotProps().plotMode;
    if (currentPlotMode === "fft") {
      const binCount = getFFTBinCount();
      if (binCount === null) return;
      state.fftAutoFit = false;
      state.fftView = panView(
        drag.startView,
        drag.startClientX,
        event.clientX,
        binCount,
        clampFFTView,
      );
      fftViewRef.current = state.fftView;
      requestRender();
      return;
    }
    if (currentPlotMode === "histogram") {
      const binCount = getHistogramBinCount();
      if (binCount === null) return;
      state.histAutoFit = false;
      state.histView = panView(
        drag.startView,
        drag.startClientX,
        event.clientX,
        binCount,
        clampHistView,
      );
      histViewRef.current = state.histView;
      requestRender();
      return;
    }

    const count = getSampleCount();
    if (count < 2) return;
    const currentProps = getPlotProps();
    if (currentProps.live && currentProps.fitAll) return;
    state.autoFit = false;
    state.view = panView(
      drag.startView,
      drag.startClientX,
      event.clientX,
      count,
      clampView,
    );
    autoFitRef.current = false;
    viewRef.current = state.view;
    requestRender();
  }

  function onMouseUp(): void {
    drag = null;
    canvas.style.cursor = "crosshair";
  }

  function onMouseLeave(): void {
    drag = null;
    hoverPhys = null;
    requestHoverRender();
    canvas.style.cursor = "crosshair";
  }

  function onDblClick(): void {
    const currentPlotMode = getPlotProps().plotMode;
    if (currentPlotMode === "fft") {
      const binCount = getFFTBinCount();
      if (binCount === null) return;
      state.fftAutoFit = true;
      state.fftView = { xMin: 0, xMax: binCount - 1 };
      fftViewRef.current = state.fftView;
      requestRender();
      return;
    }
    if (currentPlotMode === "histogram") {
      const binCount = getHistogramBinCount();
      if (binCount === null) return;
      state.histAutoFit = true;
      state.histView = { xMin: 0, xMax: binCount - 1 };
      histViewRef.current = state.histView;
      requestRender();
      return;
    }
    const currentProps = getPlotProps();
    if (currentProps.live && currentProps.fitAll) return;
    const count = getSampleCount();
    state.autoFit = true;
    if (count >= 2) state.view = { xMin: 0, xMax: count - 1 };
    autoFitRef.current = true;
    viewRef.current = state.view;
    requestRender();
  }

  return {
    onWheel,
    onMouseDown,
    onMouseMove,
    onMouseUp,
    onMouseLeave,
    onDblClick,
    get hoverPhys() {
      return hoverPhys;
    },
    reset() {
      drag = null;
      hoverPhys = null;
      canvas.style.cursor = "crosshair";
    },
  };
}
