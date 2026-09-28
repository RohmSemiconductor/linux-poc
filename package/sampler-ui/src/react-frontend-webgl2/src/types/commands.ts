export type Command =
  | {
      command: "start";
      durationMs?: number;
      device: string;
      channel: string;
      samplingFrequency: string;
    }
  | { command: "stop" }
  | { command: "get_devices"; }
  | { command: "get_channels"; device: string }
  | { command: "get_sampling_frequencies"; device: string; channel: string; }

export type ServerMessage =
  | { type: "started" }
  | { type: "stopped"; actualDurationMs: number }
  | { type: "error"; message: string }
  | { type: "get_devices"; devices: string[] }
  | { type: "get_channels"; channels: string[] }
  | { type: "get_sampling_frequencies"; sampling_frequencies: string[] }
