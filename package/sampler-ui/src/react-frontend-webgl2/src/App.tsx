import {
  useRef,
  useCallback,
  useState,
  useEffect,
  type WheelEvent,
} from "react";
import type { PlotModes, PlotData, YScale, View } from "./types";
import { computeStats, type Stats } from "@/utils/stats";
import { StatsBar } from "./components/StatsBar";
import { resetUsecs, useWebSocket, type ParsedFrame } from "./hooks/useWebSocket";
import { WGLPlot } from "./components/WGLPlot";
import { StatusDot } from "./components/StatusDot";
import { PlotterControls } from "./components/PlotterControls";
// import { generateSineData } from "./utils/sineData";
import { saveCanvasesAsImage } from "./utils/saveImage";
import { saveAsCsv } from "./utils/saveCsv";
import { loadCsvFile } from "./utils/loadCsv";
import { Button } from "@/components/ui/button";
import { Separator } from "@/components/ui/separator";
import { Toaster } from "@/components/ui/sonner";
import {
  FloppyDiskIcon,
  UploadSimpleIcon,
  DownloadSimpleIcon,
} from "@phosphor-icons/react";
import {
  INIT_CAP,
  MAX_SAMPS_IIO,
  MAX_SAMPS_MVARING,
  DEFAULT_SAMPLE_RATE,
  DEFAULT_ADC_MAX,
  LIVE_WINDOW_SIZE,
  LIVE_WINDOW_MIN,
  LIVE_WINDOW_MAX,
  LIVE_WINDOW_STEP_SIZE,
  APP_VERSION,
  DEFAULT_REF_VOLTAGE,
  DEFAULT_BACKEND,
} from "@/config/constants";
import { toast } from "sonner";
import { SettingsSheet } from "./components/SettingsSheet";
import { InfoCard } from "./components/InfoCard";

// shadcn preset: --preset b4hIZmq00

function App() {
  const importInputRef = useRef<HTMLInputElement>(null);

  const [live, setLive] = useState(false);
  const [fitAll, setFitAll] = useState(false);
  const [windowSize, setWindowSize] = useState(LIVE_WINDOW_SIZE);
  const [adcMax, setAdcMax] = useState<number>(DEFAULT_ADC_MAX);
  const [refVoltage, setRefVoltage] = useState<number>(DEFAULT_REF_VOLTAGE);
  // null = continuous, otherwise duration in ms
  const [durationMs, setDurationMs] = useState<number | null>(null);
  // y-scale: "voltage" | "raw"
  const [yScale, setYScale] = useState<YScale>("raw");
  const [plotMode, setPlotMode] = useState<PlotModes>("time");
  const [stats, setStats] = useState<Stats | null>(null);
  const [totalCount, setTotalCount] = useState(0);
  const viewRef = useRef<View>({ xMin: 0, xMax: 1 });

  const [backend, setBackend] = useState(DEFAULT_BACKEND);
  const maxSampsRef = useRef(MAX_SAMPS_MVARING);

  // All sample data lives in a plain ref — no state, no re-renders on arrival.
  // Float32Array stores Uint16 values exactly (0–65535).
  const dataRef = useRef<PlotData>({
    ys: new Float32Array(INIT_CAP),
    count: 0,
    chunkUsecs: new Float64Array(Math.ceil(INIT_CAP / maxSampsRef.current)),
    chunkCount: 0,
  });

  const [device, setDevice] = useState("");
  const [devices, setDevices] = useState<string[]>([]);
  const [channel, setChannel] = useState("");
  const [channels, setChannels] = useState<string[]>([]);

  const [sampleRate, setSampleRate] = useState(DEFAULT_SAMPLE_RATE);
  const [sampleRates, setSampleRates] = useState<number[]>([]);
  // The IIO backend does not care about this.
  const [actualSampleRate, setActualSampleRate] = useState<number | null>(null);

  // Recompute stats every 200 ms from the visible window
  useEffect(() => {
    const id = setInterval(() => {
      const { ys, count } = dataRef.current;
      setTotalCount(count);
      if (plotMode !== "time") {
        setStats(null);
        return;
      }
      if (count < 1) {
        setStats(null);
        return;
      }
      const start = Math.max(0, Math.floor(viewRef.current.xMin));
      const end = Math.min(count, Math.ceil(viewRef.current.xMax) + 1);
      setStats(computeStats(ys, start, end));
    }, 200);
    return () => clearInterval(id);
  }, [plotMode]);

  // const [elapsedMs, setElapsedMs] = useState<number | null>(null);
  // const timerRef = useRef<ReturnType<typeof setInterval> | null>(null);
  // const streamStartRef = useRef<number | null>(null);

  // function startTimer() {
  //   streamStartRef.current = performance.now();
  //   setElapsedMs(0);
  //   timerRef.current = setInterval(() => {
  //     setElapsedMs(performance.now() - streamStartRef.current!);
  //   }, 100);
  // }

  // function stopTimer() {
  //   if (timerRef.current !== null) clearInterval(timerRef.current);
  //   if (streamStartRef.current !== null)
  //     setElapsedMs(performance.now() - streamStartRef.current);
  // }

  const handleData = useCallback((frame: ParsedFrame) => {
    const d = dataRef.current;

    // Detect stream restart: rpi_adc_stream resets its hardware clock on each
    // start, so new timestamps begin at 0. Any new timestamp that is less than
    // the last stored timestamp means a new session began — clear the buffer so
    // the x-axis stays coherent.
    if (d.chunkCount > 0 && frame.chunkUsecs.length > 0) {
      const lastTs = d.chunkUsecs[d.chunkCount - 1];
      const newTs = frame.chunkUsecs[0];
      if (newTs < lastTs) {
        d.count = 0;
        d.chunkCount = 0;
      }
    }

    const chunk = frame.samples;
    const needed = d.count + chunk.length;

    if (needed > d.ys.length) {
      const newCap = Math.max(needed * 2, d.ys.length * 2);
      const grown = new Float32Array(newCap);
      grown.set(d.ys.subarray(0, d.count));
      d.ys = grown;
    }

    d.ys.set(chunk, d.count);
    d.count += chunk.length;
    // Append chunk timestamps
    const newChunkCount = d.chunkCount + frame.chunkUsecs.length;
    if (newChunkCount > d.chunkUsecs.length) {
      const newCap = Math.max(newChunkCount * 2, d.chunkUsecs.length * 2);
      const grown = new Float64Array(newCap);
      grown.set(d.chunkUsecs.subarray(0, d.chunkCount));
      d.chunkUsecs = grown;
    }
    for (let i = 0; i < frame.chunkUsecs.length; i++) {
      d.chunkUsecs[d.chunkCount + i] = frame.chunkUsecs[i];
    }
    d.chunkCount = newChunkCount;
  }, []);

  const { status, streaming, sendCommand } = useWebSocket({
    onData: handleData,
    adcMax,
    onInfo: (msg) => {
      switch (msg.type) {
        case "get_devices":
          setDevices(msg.devices);
          if (msg.devices.length) {
            setDevice(msg.devices[0]);
          }
          break;
        case "get_channels":
          setChannels(msg.channels);
          if (msg.channels.length)
            setChannel(msg.channels[0]);
          break;
        case "get_sample_rates":
          setSampleRates(msg.sampleRates);
          if (msg.sampleRates.length)
            setSampleRate(Number(msg.sampleRates[msg.sampleRates.length - 1]))
          break;
        case "actual_sample_rate":
          console.log("Actual sample-rate:", msg.value, "Hz");
          setActualSampleRate(msg.value);
          break;
      }
    },
  });

  const handleWheel = useCallback(
    (e: WheelEvent<HTMLDivElement>) => {
      if (!live) return;

      setWindowSize((prev) => {
        const delta =
          e.deltaY > 0 ? LIVE_WINDOW_STEP_SIZE : -LIVE_WINDOW_STEP_SIZE;
        return Math.min(
          LIVE_WINDOW_MAX,
          Math.max(LIVE_WINDOW_MIN, prev + delta),
        );
      });
    },
    [live],
  );

  useEffect(() => {
    if (backend !== "iio" || status !== "connected") {
      setDevices([]);
      setDevice("");
      setChannels([]);
      setChannel("");
      setSampleRates([]);
    } else {
      sendCommand({
        command: "get_devices",
      });
    }
  }, [backend, status]);

  useEffect(() => {
    if (backend === "iio")
      maxSampsRef.current = MAX_SAMPS_IIO;
    else
      maxSampsRef.current = MAX_SAMPS_MVARING;
  }, [backend]);

  useEffect(() => {
    if (device) {
      sendCommand({
        command: "get_channels",
        device: device,
      });
    }
  }, [device]);

  useEffect(() => {
    if (channel) {
      sendCommand({
        command: "get_sample_rates",
        device: device,
        channel: channel,
      });
    }
  }, [channel]);

  function handleClear() {
    dataRef.current.count = 0;
    dataRef.current.ys = new Float32Array(INIT_CAP);
    dataRef.current.chunkCount = 0;
    dataRef.current.chunkUsecs = new Float64Array(
      Math.ceil(INIT_CAP / maxSampsRef.current),
    );
    resetUsecs();
  }

  function handlePlotModeChange(mode: PlotModes) {
    setPlotMode(mode);
    if (mode === "fft") setLive(false);
  }

  return (
    <div className="flex h-screen flex-col p-4">
      <div className="flex h-5 items-center gap-2 mb-4 text-xs text-muted-foreground">
        <span>ADC Plotter v{APP_VERSION}</span>
        <Separator orientation="vertical" />
        <StatusDot status={status} streaming={streaming} />
        status:
        <strong
          className={
            status === "connected" ? "text-emerald-400" : "text-orange-400"
          }
        >
          {status}
        </strong>
        {/* <Separator orientation="vertical" />
        elapsed:
        <strong>
          {elapsedMs === null ? "—" : (elapsedMs / 1000).toFixed(2) + " s"}
        </strong> */}
        {/* <Button
          onClick={() => {
            dataRef.current = generateSineData(5_000_000);
          }}
        >
          Sine 5M
        </Button>
        <Button
          onClick={() => {
            dataRef.current = generateSineData(10_000_000);
          }}
        >
          Sine 10M
        </Button> */}
        <div className="ml-auto flex gap-2">
          <input
            ref={importInputRef}
            type="file"
            accept=".csv"
            className="hidden"
            onChange={(e) => {
              const file = e.target.files?.[0];
              if (!file) return;
              loadCsvFile(
                file,
                maxSampsRef.current,
                (data) => {
                  dataRef.current = data;
                  toast.success("Data imported successfully", {
                    position: "top-center",
                  });
                },
                (e) =>
                  toast.error(`Import failed: ${e}`, {
                    position: "top-center",
                  }),
              );
              e.target.value = "";
            }}
          />
          <Button
            variant="outline"
            title="Import CSV file"
            onClick={() => importInputRef.current?.click()}
          >
            <DownloadSimpleIcon data-icon="inline-start" />
            Import
          </Button>
          <Button
            variant="outline"
            title="Export all samples to CSV"
            onClick={() =>
              saveAsCsv(
                dataRef.current,
                maxSampsRef.current,
                (e) => toast.error(`Export failed: ${e}`, {
                  position: "top-center",
                }),
              )
            }
          >
            <UploadSimpleIcon data-icon="inline-start" />
            Export
          </Button>
          <Button
            title="Save current view as image"
            onClick={() => saveCanvasesAsImage("plotter")}
          >
            <FloppyDiskIcon data-icon="inline-start"></FloppyDiskIcon>
            Save
          </Button>
        </div>
      </div>

      <WGLPlot
        onWheel={handleWheel}
        id={"plotter"}
        dataRef={dataRef}
        style={{ flex: 1, minHeight: 0 }}
        sampleRate={sampleRate}
        actualSampleRate={actualSampleRate}
        adcMax={adcMax}
        live={live}
        fitAll={fitAll}
        windowSize={windowSize}
        plotMode={plotMode}
        y_scale={yScale}
        voltageRef={refVoltage}
        viewRef={viewRef}
        maxSamps={maxSampsRef.current}
      />

      <StatsBar
        stats={stats}
        yScale={yScale}
        adcMax={adcMax}
        voltageRef={refVoltage}
        total={totalCount}
      />

      <div className="mt-2 flex flex-wrap items-center gap-3">
        <PlotterControls
          durationMs={durationMs}
          setDurationMs={setDurationMs}
          streaming={streaming}
          status={status}
          sendCommand={sendCommand}
          device={device}
          channel={channel}
          sampleRate={sampleRate}
          handleClear={handleClear}
          live={live}
          setLive={setLive}
          fitAll={fitAll}
          setFitAll={setFitAll}
          windowSize={windowSize}
          setWindowSize={setWindowSize}
          plotMode={plotMode}
        />

        <div className="ml-auto flex items-center gap-2">
          <InfoCard plotMode={plotMode} />
          <SettingsSheet
            adcMax={adcMax}
            setAdcMax={setAdcMax}
            refVoltage={refVoltage}
            setRefVoltage={setRefVoltage}
            sampleRate={sampleRate}
            setSampleRate={setSampleRate}
            sampleRates={sampleRates}
            streaming={streaming}
            yScale={yScale}
            setYScale={setYScale}
            plotMode={plotMode}
            setPlotMode={handlePlotModeChange}
            device={device}
            setDevice={setDevice}
            devices={devices}
            channel={channel}
            setChannel={setChannel}
            channels={channels}
            backend={backend}
            setBackend={setBackend}
          />
        </div>
      </div>
      <Toaster />
    </div>
  );
}

export default App;
