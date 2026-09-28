export type Command =
  | {
      command: "start";
      durationMs?: number;
      uri: string;
      device: string;
      channel: string;
      samplingFrequency: string;
    }
  | { command: "stop" }
  | { command: "get_devices"; uri: string }
  | { command: "get_channels"; uri: string; device: string }
  | { command: "get_sampling_frequencies"; uri: string; device: string; channel: string; }

export type ServerMessage =
  | { type: "started" }
  | { type: "stopped"; actualDurationMs: number }
  | { type: "error"; message: string }
  | { type: "get_devices"; devices: string[] }
  | { type: "get_channels"; channels: string[] }
  | { type: "get_sampling_frequencies"; sampling_frequencies: string[] }
