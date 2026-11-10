import {
  useRef,
  useCallback,
  useState,
  useEffect,
  type WheelEvent,
} from "react";
import type { PlotModes, PlotData, YScale, View } from "./types";
import { StatsIndex, type Stats } from "@/utils/stats";
import { StatsBar } from "./components/StatsBar";
import { useWebSocket, type ParsedFrame } from "./hooks/useWebSocket";
import { WGLPlot } from "./components/WGLPlot";
import { StatusDot } from "./components/StatusDot";
import { PlotterControls } from "./components/PlotterControls";
import { generateSineData } from "./utils/sineData";
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
  MAX_SAMPS,
  DEFAULT_SAMPLE_RATE,
  DEFAULT_ADC_MAX,
  LIVE_WINDOW_SIZE,
  LIVE_WINDOW_MIN,
  LIVE_WINDOW_MAX,
  LIVE_WINDOW_STEP_SIZE,
  APP_VERSION,
  DEFAULT_REF_VOLTAGE,
  DEV_TOOLS_ENABLED,
} from "@/config/constants";
import { toast } from "sonner";
import { SettingsSheet } from "./components/SettingsSheet";
import { InfoCard } from "./components/InfoCard";
import { Label } from "./components/ui/label";
import { Checkbox } from "./components/ui/checkbox";
import {
  Select,
  SelectContent,
  SelectItem,
  SelectTrigger,
  SelectValue,
} from "./components/ui/select";

// shadcn preset: --preset b4hIZmq00
function App() {
  // All sample data lives in a plain ref — no state, no re-renders on arrival.
  // Float32Array stores Uint16 values exactly (0–65535).
  const dataRef = useRef<PlotData>({
    ys: new Float32Array(INIT_CAP),
    count: 0,
    generation: 0,
    chunkUsecs: new Float64Array(Math.ceil(INIT_CAP / MAX_SAMPS)),
    chunkCount: 0,
  });

  const importInputRef = useRef<HTMLInputElement>(null);
  const saveRequestRef = useRef(0);

  const [live, setLive] = useState(false);
  const [fitAll, setFitAll] = useState(false);
  const [windowSize, setWindowSize] = useState(LIVE_WINDOW_SIZE);
  const [sampleRate, setSampleRate] = useState(DEFAULT_SAMPLE_RATE);
  const [actualSampleRate, setActualSampleRate] = useState<number | null>(null);
  const [adcMax, setAdcMax] = useState<number>(DEFAULT_ADC_MAX);
  const [refVoltage, setRefVoltage] = useState<number>(DEFAULT_REF_VOLTAGE);
  const [rampCheckEnabled, setRampCheckEnabled] = useState(DEV_TOOLS_ENABLED);
  const [rampErrorCount, setRampErrorCount] = useState(0);
  const rampErrorCountRef = useRef(0);
  const previousRampSampleRef = useRef<number | null>(null);
  // null = continuous, otherwise duration in ms
  const [durationMs, setDurationMs] = useState<number | null>(null);
  // y-scale: "voltage" | "raw"
  const [yScale, setYScale] = useState<YScale>("raw");
  const [plotMode, setPlotMode] = useState<PlotModes>("time");
  const [stats, setStats] = useState<Stats | null>(null);
  const [totalCount, setTotalCount] = useState(0);
  const viewRef = useRef<View>({ xMin: 0, xMax: 1 });
  const [statsIndex] = useState(() => new StatsIndex());
  const statsSourceRef = useRef<PlotData | null>(null);
  const statsGenerationRef = useRef(-1);
  const statsIndexedBufferRef = useRef<Float32Array | null>(null);
  const statsIndexedCountRef = useRef(0);
  const statsRequestRef = useRef<{
    source: PlotData;
    ys: Float32Array;
    generation: number;
    start: number;
    end: number;
    displayScale: number;
    displayPrecision: number;
  } | null>(null);

  // Recompute stats every 200 ms from the visible window
  useEffect(() => {
    const id = setInterval(() => {
      const { ys, count } = dataRef.current;
      setTotalCount(count);
      setRampErrorCount(rampErrorCountRef.current);
      if (plotMode !== "time") {
        statsRequestRef.current = null;
        setStats(null);
        return;
      }

      const source = dataRef.current;
      if (
        statsSourceRef.current !== source ||
        statsGenerationRef.current !== source.generation ||
        (statsIndexedBufferRef.current !== ys &&
          statsIndexedCountRef.current === count)
      ) {
        statsIndex.reset();
        statsSourceRef.current = source;
        statsGenerationRef.current = source.generation;
        statsIndexedBufferRef.current = null;
        statsIndexedCountRef.current = 0;
        statsRequestRef.current = null;
      }

      const start = Math.max(0, Math.floor(viewRef.current.xMin));
      const end = Math.min(count, Math.ceil(viewRef.current.xMax) + 1);
      const displayScale = yScale === "voltage" ? refVoltage / adcMax : 1;
      const displayPrecision = yScale === "voltage" ? 4 : 2;
      const previousRequest = statsRequestRef.current;
      if (
        previousRequest?.source === source &&
        previousRequest.ys === ys &&
        previousRequest.generation === source.generation &&
        previousRequest.start === start &&
        previousRequest.end === end &&
        previousRequest.displayScale === displayScale &&
        previousRequest.displayPrecision === displayPrecision
      ) {
        return;
      }

      statsIndex.update(ys, count);
      statsIndexedBufferRef.current = ys;
      statsIndexedCountRef.current = count;
      statsRequestRef.current = {
        source,
        ys,
        generation: source.generation,
        start,
        end,
        displayScale,
        displayPrecision,
      };
      setStats(
        statsIndex.compute(ys, start, end, displayScale, displayPrecision),
      );
    }, 200);
    return () => clearInterval(id);
  }, [plotMode, statsIndex, yScale, adcMax, refVoltage]);

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

  const handleData = useCallback(
    (frame: ParsedFrame) => {
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
          d.generation++;
          previousRampSampleRef.current = null;
          rampErrorCountRef.current = 0;
          setRampErrorCount(0);
        }
      }

      const chunk = frame.samples;
      if (rampCheckEnabled) {
        let previous = previousRampSampleRef.current;
        for (const sample of chunk) {
          const isExactRollover = previous === adcMax && sample === 0;
          if (previous !== null && sample < previous && !isExactRollover) {
            rampErrorCountRef.current++;
          }
          previous = sample;
        }
        previousRampSampleRef.current = previous;
      }

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
    },
    [adcMax, rampCheckEnabled],
  );

  const { status, streaming, sendCommand } = useWebSocket({
    onData: handleData,
    adcMax,
    onInfo: (msg) => {
      if (msg.type === "actual_sample_rate") {
        console.log("Actual sample-rate:", msg.value, "Hz");
        setActualSampleRate(msg.value);
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

  function handleClear() {
    dataRef.current.count = 0;
    dataRef.current.generation++;
    dataRef.current.ys = new Float32Array(INIT_CAP);
    dataRef.current.chunkCount = 0;
    dataRef.current.chunkUsecs = new Float64Array(
      Math.ceil(INIT_CAP / MAX_SAMPS),
    );
    previousRampSampleRef.current = null;
    rampErrorCountRef.current = 0;
    setRampErrorCount(0);
  }

  function handleGenerateSineData(sampleCount: number) {
    dataRef.current = generateSineData(sampleCount);
    previousRampSampleRef.current = null;
    rampErrorCountRef.current = 0;
    setRampErrorCount(0);
  }

  function handleRampCheckChange(enabled: boolean) {
    previousRampSampleRef.current = null;
    rampErrorCountRef.current = 0;
    setRampErrorCount(0);
    setRampCheckEnabled(enabled);
  }

  function handlePlotModeChange(mode: PlotModes) {
    setPlotMode(mode);
    if (mode === "fft") setLive(false);
    if (mode === "histogram") setLive(false);
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
        {DEV_TOOLS_ENABLED && (
          <>
            <Separator orientation="vertical" />
            <Label
              title="Count samples below the previous sample. Repeated values and an exact max-to-zero rollover are allowed."
              className="gap-1.5"
            >
              <Checkbox
                checked={rampCheckEnabled}
                onCheckedChange={(checked) =>
                  handleRampCheckChange(checked === true)
                }
              />
              Linear counter check
            </Label>
            {rampCheckEnabled && (
              <>
                <Separator orientation="vertical" />
                <span
                  className={
                    rampErrorCount > 0
                      ? "text-rose-400"
                      : "text-muted-foreground"
                  }
                >
                  counter errors: {rampErrorCount.toLocaleString()}
                </span>
              </>
            )}
            <Button
              variant="outline"
              size="xs"
              title="Generate 5 million sine samples"
              disabled={streaming}
              onClick={() => handleGenerateSineData(5_000_000)}
            >
              Sine 5M
            </Button>
            <Button
              variant="outline"
              size="xs"
              title="Generate 80 million sine samples"
              disabled={streaming}
              onClick={() => handleGenerateSineData(80_000_000)}
            >
              Sine 80M
            </Button>
          </>
        )}
        {/* <Separator orientation="vertical" />
        elapsed:
        <strong>
          {elapsedMs === null ? "—" : (elapsedMs / 1000).toFixed(2) + " s"}
        </strong> */}
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
              saveAsCsv(dataRef.current, (e) =>
                toast.error(`Export failed: ${e}`, {
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
            onClick={() => {
              saveRequestRef.current++;
            }}
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
        saveRequestRef={saveRequestRef}
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
          <Label htmlFor="plotmode" className="text-sm">
            Plot Mode
          </Label>
          <Select
            value={plotMode}
            onValueChange={(v) => setPlotMode(v as PlotModes)}
          >
            <SelectTrigger id="plotmode-select" className="w-32 h-8 text-sm">
              <SelectValue />
            </SelectTrigger>
            <SelectContent>
              <SelectItem value="time">Time</SelectItem>
              <SelectItem value="fft">FFT</SelectItem>
              <SelectItem value="histogram">Histogram</SelectItem>
            </SelectContent>
          </Select>
          <InfoCard plotMode={plotMode} />
          <SettingsSheet
            adcMax={adcMax}
            setAdcMax={setAdcMax}
            refVoltage={refVoltage}
            setRefVoltage={setRefVoltage}
            sampleRate={sampleRate}
            setSampleRate={setSampleRate}
            streaming={streaming}
            yScale={yScale}
            setYScale={setYScale}
            plotMode={plotMode}
            setPlotMode={handlePlotModeChange}
          />
        </div>
      </div>
      <Toaster />
    </div>
  );
}

export default App;
