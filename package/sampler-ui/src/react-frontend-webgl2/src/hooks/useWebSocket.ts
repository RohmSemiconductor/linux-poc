import { useState, useEffect, useRef, useCallback } from "react";
import {
  WS_URL,
} from "../config/constants";
import type { Command, ServerMessage } from "../types/commands";

type ConnectionStatus = "connected" | "disconnected" | "error";

/**
 * Decoded result from one WebSocket binary frame.
 */
export interface ParsedFrame {
  samples: Int16Array;
  chunkUsecs: number[];
}

/**
 * Decode one WebSocket binary frame.
 */
function parseAdcFrame(buf: ArrayBuffer): ParsedFrame {
  const samples = new Int16Array(buf.byteLength / 2);
  const chunkUsecs: number[] = [0];
  const dv = new DataView(buf);

  for (let i = 0, j = 0; i < buf.byteLength; i += 2, j++)
    samples[j] = dv.getInt16(i, true);

  return { samples, chunkUsecs };
}

interface UseWebSocketParams {
  url?: string;
  onData: (frame: ParsedFrame) => void;
  /** Bitmask derived from the selected ADC bit depth, e.g. 2047 (11-bit) or 4095 (12-bit). */
  adcMax?: number;
  /** Called when the server sends a JSON info message. */
  onInfo?: (msg: ServerMessage) => void;
}

interface UseWebSocketReturn {
  status: ConnectionStatus;
  streaming: boolean;
  sendCommand: (cmd: Command) => void;
}

const RECONNECT_INTERVAL_MS = 2000;

export function useWebSocket({
  url = WS_URL,
  onData,
  onInfo,
}: UseWebSocketParams): UseWebSocketReturn {
  const [status, setStatus] = useState<ConnectionStatus>("disconnected");
  const [streaming, setStreaming] = useState(false);

  const wsRef = useRef<WebSocket | null>(null);
  const reconnectTimerRef = useRef<ReturnType<typeof setTimeout> | null>(null);

  // Refs so changes to onData/adcMax/onInfo never trigger WS reconnection
  const onDataRef = useRef(onData);
  useEffect(() => {
    onDataRef.current = onData;
  }, [onData]);
  const onInfoRef = useRef(onInfo);
  useEffect(() => {
    onInfoRef.current = onInfo;
  }, [onInfo]);

  useEffect(() => {
    let cancelled = false;

    function connect() {
      if (cancelled) return;

      const ws = new WebSocket(url);
      ws.binaryType = "arraybuffer";
      wsRef.current = ws;

      ws.onopen = () => {
        if (cancelled) return;
        setStatus("connected");
      };

      ws.onclose = () => {
        if (cancelled) return;
        setStatus("disconnected");
        setStreaming(false);
        reconnectTimerRef.current = setTimeout(connect, RECONNECT_INTERVAL_MS);
      };

      ws.onerror = () => {
        if (cancelled) return;
        setStatus("error");
      };

      ws.onmessage = (e: MessageEvent) => {
        if (cancelled) return;
        if (typeof e.data === "string") {
          try {
            const msg = JSON.parse(e.data) as ServerMessage;
            if (msg.type === "stopped") {
              setStreaming(false);
            }
            onInfoRef.current?.(msg);
          } catch {
            // ignore malformed text frames
          }
          return;
        }
        if (!(e.data instanceof ArrayBuffer)) return;
        const frame = parseAdcFrame(e.data);

        if (frame.samples.length > 0)
          onDataRef.current?.(frame);
      };
    }

    reconnectTimerRef.current = setTimeout(connect, 50);

    return () => {
      cancelled = true;
      if (reconnectTimerRef.current) clearTimeout(reconnectTimerRef.current);
      wsRef.current?.close();
    };
  }, [url]);

  const sendCommand = useCallback((cmd: Command) => {
    if (wsRef.current?.readyState !== WebSocket.OPEN) return;
    wsRef.current.send(JSON.stringify(cmd));
    if (cmd.command === "start") {
      setStreaming(true);
    } else {
      // Immediate UI fallback; authoritative false comes from { type: "stopped" }
      setStreaming(false);
    }
  }, []);

  return { status, streaming, sendCommand };
}
