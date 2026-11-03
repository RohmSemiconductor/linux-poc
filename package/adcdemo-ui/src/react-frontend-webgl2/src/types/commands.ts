export type Command =
  | { command: "start"; sampleRate: number; durationMs?: number }
  | { command: "stop" };

export type ServerMessage =
  | { type: "started" }
  | { type: "stopped"; actualDurationMs: number }
  | { type: "actual_sample_rate"; value: number }
  | { type: "error"; message: string };
