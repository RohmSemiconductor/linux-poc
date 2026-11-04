import { Button } from "./ui/button";
import {
  Sheet,
  SheetContent,
  SheetHeader,
  SheetTitle,
  SheetTrigger,
} from "./ui/sheet";
import { GearSixIcon } from "@phosphor-icons/react";
import {
  Select,
  SelectContent,
  SelectItem,
  SelectTrigger,
  SelectValue,
} from "./ui/select";
import { Label } from "./ui/label";
import { ADC_OPTIONS, REF_VOLTAGE_OPTIONS, BACKENDS } from "@/config/constants";
import type { PlotModes, YScale } from "@/types";
import { NumberInput } from "./ui/number-input";

export function SettingsSheet({
  adcMax,
  setAdcMax,
  refVoltage,
  setRefVoltage,
  sampleRate,
  setSampleRate,
  sampleRates,
  streaming,
  yScale,
  setYScale,
  plotMode,
  setPlotMode,
  device,
  setDevice,
  devices,
  channel,
  setChannel,
  channels,
  backend,
  setBackend,
}: {
  adcMax: number;
  setAdcMax: (v: number) => void;
  refVoltage: number;
  setRefVoltage: (v: number) => void;
  sampleRate: number;
  setSampleRate: (v: number) => void;
  sampleRates: number[];
  streaming: boolean;
  yScale: YScale;
  setYScale: (v: YScale) => void;
  plotMode: PlotModes;
  setPlotMode: (v: PlotModes) => void;
  device: string;
  setDevice: (v: string) => void;
  devices: string[];
  channel: string;
  setChannel: (v: string) => void;
  channels: string[];
  backend: string;
  setBackend: (v: string) => void;
}) {
  return (
    <Sheet>
      <SheetTrigger asChild>
        <Button title="Settings" variant="outline" size="icon">
          <GearSixIcon />
        </Button>
      </SheetTrigger>
      <SheetContent className="px-4 w-72" aria-describedby={undefined}>
        <SheetHeader className="mb-4">
          <SheetTitle>Settings</SheetTitle>
        </SheetHeader>

        <div className="grid grid-cols-[1fr_auto] items-center gap-x-4 gap-y-3">
          <Label htmlFor="backends" className="text-sm">
            Backend
          </Label>
          <Select
            value={backend}
            onValueChange={(v) => setBackend(v)}
          >
            <SelectTrigger id="backends" className="w-40 h-8 text-sm">
              <SelectValue />
            </SelectTrigger>
            <SelectContent position="popper">
              {BACKENDS.map((e) => (
                <SelectItem key={e.value} value={e.value}>
                  {e.label}
                </SelectItem>
              ))}
            </SelectContent>
          </Select>

          <Label htmlFor="devices" className="text-sm">
            IIO Device
          </Label>
          <Select
            value={device}
            onValueChange={(v) => setDevice(v)}
            disabled={backend === "uio"}
          >
            <SelectTrigger id="devices" className="w-40 h-8 text-sm">
              <SelectValue />
            </SelectTrigger>
            <SelectContent position="popper">
              {devices.map((e) => (
                <SelectItem key={e} value={e}>
                  {e}
                </SelectItem>
              ))}
            </SelectContent>
          </Select>

          <Label htmlFor="channels" className="text-sm">
            IIO Channel
          </Label>
          <Select
            value={channel}
            onValueChange={(v) => setChannel(v)}
            disabled={backend === "uio"}
          >
            <SelectTrigger id="channels" className="w-40 h-8 text-sm">
              <SelectValue />
            </SelectTrigger>
            <SelectContent position="popper">
              {channels.map((e) => (
                <SelectItem key={e} value={e}>
                  {e}
                </SelectItem>
              ))}
            </SelectContent>
          </Select>

          <Label htmlFor="adc-select" className="text-sm">
            ADC Resolution
          </Label>
          <Select
            value={String(adcMax)}
            onValueChange={(v) => setAdcMax(Number(v))}
          >
            <SelectTrigger id="adc-select" className="w-40 h-8 text-sm">
              <SelectValue />
            </SelectTrigger>
            <SelectContent position="popper">
              {ADC_OPTIONS.map((opt) => (
                <SelectItem key={opt.value} value={String(opt.value)}>
                  {opt.label}
                </SelectItem>
              ))}
            </SelectContent>
          </Select>

          <Label htmlFor="yscale-select" className="text-sm">
            Y Scale
          </Label>
          <Select value={yScale} onValueChange={(v) => setYScale(v as YScale)}>
            <SelectTrigger id="yscale-select" className="w-40 h-8 text-sm">
              <SelectValue />
            </SelectTrigger>
            <SelectContent position="popper">
              <SelectItem value="raw">Raw Value</SelectItem>
              <SelectItem value="voltage">Voltage</SelectItem>
            </SelectContent>
          </Select>

          <Label htmlFor="vol-select" className="text-sm">
            Reference Voltage
          </Label>
          <Select
            value={String(refVoltage)}
            onValueChange={(v) => setRefVoltage(Number(v))}
            disabled={yScale === "raw"}
          >
            <SelectTrigger id="vol-select" className="w-40 h-8 text-sm">
              <SelectValue />
            </SelectTrigger>
            <SelectContent position="popper">
              {REF_VOLTAGE_OPTIONS.map((opt) => (
                <SelectItem key={opt.value} value={String(opt.value)}>
                  {opt.label}
                </SelectItem>
              ))}
            </SelectContent>
          </Select>

          <Label htmlFor="samplerate" className="text-sm">
            Sample Rate (S/s)
          </Label>
          {backend === "uio" ?
            <NumberInput
              id="samplerate"
              min={1000}
              step={1000}
              fallback={1000}
              value={sampleRate}
              onChange={setSampleRate}
              className="w-40 h-8 text-sm"
              disabled={streaming}
            />
            :
            <Select
              value={String(sampleRate)}
              onValueChange={(v) => setSampleRate(Number(v))}
            >
              <SelectTrigger id="samplerate" className="w-40 h-8 text-sm">
                <SelectValue />
              </SelectTrigger>
              <SelectContent position="popper">
                {sampleRates.map((e) => (
                  <SelectItem key={e} value={String(e)}>
                    {e}
                  </SelectItem>
                ))}
              </SelectContent>
            </Select>
          }

          <Label htmlFor="plotmode" className="text-sm">
            Plot Mode
          </Label>
          <Select
            value={plotMode}
            onValueChange={(v) => setPlotMode(v as PlotModes)}
          >
            <SelectTrigger id="plotmode-select" className="w-40 h-8 text-sm">
              <SelectValue />
            </SelectTrigger>
            <SelectContent position="popper">
              <SelectItem value="time">Time</SelectItem>
              <SelectItem value="fft">FFT</SelectItem>
            </SelectContent>
          </Select>
        </div>
      </SheetContent>
    </Sheet>
  );
}
