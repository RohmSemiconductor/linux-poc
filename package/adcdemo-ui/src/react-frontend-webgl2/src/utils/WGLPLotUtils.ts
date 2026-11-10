import type { View } from "../types";
import { VERT, FRAG, compileShader } from "./glsl";
import { PAD } from "./plotDraw";

export interface WGLPlotCanvasSetup {
  glCanvas: HTMLCanvasElement;
  axisCanvas: HTMLCanvasElement;
  hoverCanvas: HTMLCanvasElement;
  dpr: number;
  resizeObserver: ResizeObserver;
}

export function createWGLPlotCanvases(
  wrap: HTMLDivElement,
  onResize: (width: number, height: number) => void,
): WGLPlotCanvasSetup {
  const glCanvas = document.createElement("canvas");
  const axisCanvas = document.createElement("canvas");
  const hoverCanvas = document.createElement("canvas");

  for (const canvas of [glCanvas, axisCanvas, hoverCanvas]) {
    Object.assign(canvas.style, {
      position: "absolute",
      top: "0",
      left: "0",
      width: "100%",
      height: "100%",
    });
  }
  axisCanvas.style.pointerEvents = "none";
  hoverCanvas.style.pointerEvents = "none";
  glCanvas.style.cursor = "crosshair";

  wrap.appendChild(axisCanvas);
  wrap.appendChild(glCanvas);
  wrap.appendChild(hoverCanvas);

  const dpr = window.devicePixelRatio || 1;

  function resize() {
    const rect = wrap.getBoundingClientRect();
    const width = Math.round(rect.width * dpr);
    const height = Math.round(rect.height * dpr);
    glCanvas.width = axisCanvas.width = hoverCanvas.width = width;
    glCanvas.height = axisCanvas.height = hoverCanvas.height = height;
    onResize(width, height);
  }

  resize();
  const resizeObserver = new ResizeObserver(resize);
  resizeObserver.observe(wrap);

  return { glCanvas, axisCanvas, hoverCanvas, dpr, resizeObserver };
}

interface WGLPlotBuffer {
  vao: WebGLVertexArrayObject;
  vbo: WebGLBuffer;
  capacity: number;
}

interface WGLPlotLocations {
  a_y: number;
  iOffset: WebGLUniformLocation | null;
  xMin: WebGLUniformLocation | null;
  xMax: WebGLUniformLocation | null;
  yMin: WebGLUniformLocation | null;
  yMax: WebGLUniformLocation | null;
  color: WebGLUniformLocation | null;
}

export interface WGLPlotWebGLResources {
  program: WebGLProgram;
  locations: WGLPlotLocations;
  sample: WGLPlotBuffer;
  fft: WGLPlotBuffer;
  histogram: WGLPlotBuffer;
  decimation: WGLPlotBuffer;
}

export interface WGLPlotDrawOptions {
  viewport: {
    x: number;
    y: number;
    width: number;
    height: number;
  };
  xMin: number;
  xMax: number;
  yMin: number;
  yMax: number;
  color: Float32Array;
  vao: WebGLVertexArrayObject;
  vertexCount: number;
}

export function createWGLPlotWebGLContext(
  canvas: HTMLCanvasElement,
): WebGL2RenderingContext | null {
  return canvas.getContext("webgl2", {
    antialias: true,
    preserveDrawingBuffer: false,
    depth: false,
    stencil: false,
  });
}

function createWGLPlotBuffer(
  gl: WebGL2RenderingContext,
  attributeLocation: number,
  capacity: number,
  errorMessage: string,
): WGLPlotBuffer {
  const vao = gl.createVertexArray();
  const vbo = gl.createBuffer();
  if (!vao || !vbo) throw new Error(errorMessage);

  gl.bindVertexArray(vao);
  gl.bindBuffer(gl.ARRAY_BUFFER, vbo);
  gl.bufferData(gl.ARRAY_BUFFER, capacity * 4, gl.DYNAMIC_DRAW);
  gl.enableVertexAttribArray(attributeLocation);
  gl.vertexAttribPointer(attributeLocation, 1, gl.FLOAT, false, 0, 0);
  gl.bindVertexArray(null);

  return { vao, vbo, capacity };
}

export function initializeWGLPlotWebGL(
  gl: WebGL2RenderingContext,
): WGLPlotWebGLResources {
  const program = gl.createProgram();
  if (!program) throw new Error("Failed to create WebGL program");
  gl.attachShader(program, compileShader(gl, gl.VERTEX_SHADER, VERT));
  gl.attachShader(program, compileShader(gl, gl.FRAGMENT_SHADER, FRAG));
  gl.linkProgram(program);
  if (!gl.getProgramParameter(program, gl.LINK_STATUS))
    throw new Error(gl.getProgramInfoLog(program) ?? "Program link failed");

  const locations: WGLPlotLocations = {
    a_y: gl.getAttribLocation(program, "a_y"),
    iOffset: gl.getUniformLocation(program, "u_iOffset"),
    xMin: gl.getUniformLocation(program, "u_xMin"),
    xMax: gl.getUniformLocation(program, "u_xMax"),
    yMin: gl.getUniformLocation(program, "u_yMin"),
    yMax: gl.getUniformLocation(program, "u_yMax"),
    color: gl.getUniformLocation(program, "u_color"),
  };

  const sample = createWGLPlotBuffer(
    gl,
    locations.a_y,
    1,
    "Failed to create VAO/VBO",
  );
  const fft = createWGLPlotBuffer(
    gl,
    locations.a_y,
    4,
    "Failed to create FFT VAO/VBO",
  );
  const histogram = createWGLPlotBuffer(
    gl,
    locations.a_y,
    4,
    "Failed to create histogram VAO/VBO",
  );
  const decimation = createWGLPlotBuffer(
    gl,
    locations.a_y,
    4,
    "Failed to create decimate VAO/VBO",
  );

  return { program, locations, sample, fft, histogram, decimation };
}

export function disposeWGLPlotWebGL(
  gl: WebGL2RenderingContext,
  resources: WGLPlotWebGLResources,
): void {
  for (const buffer of [
    resources.sample,
    resources.fft,
    resources.histogram,
    resources.decimation,
  ]) {
    gl.deleteVertexArray(buffer.vao);
    gl.deleteBuffer(buffer.vbo);
  }
  gl.deleteProgram(resources.program);
  gl.getExtension("WEBGL_lose_context")?.loseContext();
}

export function drawWGLPlotSeries(
  gl: WebGL2RenderingContext,
  locations: WGLPlotWebGLResources["locations"],
  options: WGLPlotDrawOptions,
): void {
  const { viewport, xMin, xMax, yMin, yMax, color, vao, vertexCount } =
    options;

  gl.viewport(viewport.x, viewport.y, viewport.width, viewport.height);
  gl.uniform1i(locations.iOffset, 0);
  gl.uniform1f(locations.xMin, xMin);
  gl.uniform1f(locations.xMax, xMax);
  gl.uniform1f(locations.yMin, yMin);
  gl.uniform1f(locations.yMax, yMax);
  gl.uniform3fv(locations.color, color);
  gl.bindVertexArray(vao);
  gl.drawArrays(gl.LINE_STRIP, 0, vertexCount);
  gl.bindVertexArray(null);
}

export function tFromPhysX(
  physX: number,
  canvasWidth: number,
  dpr: number,
): number {
  const plotLeft = PAD.l * dpr;
  const plotWidth = canvasWidth - (PAD.l + PAD.r) * dpr;
  return Math.max(0, Math.min(1, (physX - plotLeft) / plotWidth));
}

export function clampView(view: View, count: number): View {
  let { xMin, xMax } = view;
  const range = xMax - xMin;
  if (xMin < 0) {
    xMin = 0;
    xMax = range;
  }
  if (xMax > count - 1) {
    xMax = count - 1;
    xMin = xMax - range;
  }
  xMin = Math.max(0, xMin);
  xMax = Math.min(count - 1, xMax);
  return { xMin, xMax };
}

export function clampHistView(view: View, binCount: number): View {
  return clampFFTView(view, binCount);
}

export function clampFFTView(view: View, binCount: number): View {
  let { xMin, xMax } = view;
  const range = xMax - xMin;
  if (xMin < 0) {
    xMin = 0;
    xMax = range;
  }
  if (xMax > binCount - 1) {
    xMax = binCount - 1;
    xMin = xMax - range;
  }
  xMin = Math.max(0, xMin);
  xMax = Math.min(binCount - 1, xMax);
  return { xMin, xMax };
}