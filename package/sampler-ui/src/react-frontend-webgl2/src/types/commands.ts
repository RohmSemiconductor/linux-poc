export type Command =
  | {
      command: "start";
      durationMs?: number;
      device?: string;
      channel?: string;
      sampleRate: number;
    }
  | { command: "stop" }
  | { command: "get_devices"; }
  | { command: "get_channels"; device: string }
  | { command: "get_sample_rates"; device: string; channel: string; }

export type ServerMessage =
  | { type: "started" }
  | { type: "stopped"; actualDurationMs: number }
  | { type: "actual_sample_rate"; value: number }
  | { type: "error"; message: string }
  | { type: "get_devices"; devices: string[] }
  | { type: "get_channels"; channels: string[] }
  | { type: "get_sample_rates"; sampleRates: number[] }
