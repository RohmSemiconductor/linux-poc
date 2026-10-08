declare const __APP_VERSION__: string;
export const APP_VERSION = __APP_VERSION__;

export const WS_URL = `ws://${window.location.hostname}:8080/ws`;
export const NOT_IMPLEMENTED = true;

export const MAX_SAMPS = 36864;

// CPU / GPU buffer pre-allocation
export const INIT_CAP = 1_000_000;
export const INIT_GPU_CAP = 1_000_000;

// Plot interaction
export const ZOOM_FACTOR = 1.15;
export const MIN_VISIBLE = 10; // minimum samples visible when zoomed in

// Streaming defaults
export const LIVE_WINDOW_SIZE = 50_000;
export const LIVE_WINDOW_MIN = 100;
export const LIVE_WINDOW_MAX = 1_000_000;
export const LIVE_WINDOW_STEP_SIZE = 100;
