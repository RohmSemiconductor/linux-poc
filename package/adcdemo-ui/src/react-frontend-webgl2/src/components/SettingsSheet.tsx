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
import { ADC_OPTIONS, REF_VOLTAGE_OPTIONS } from "@/config/constants";
import type { PlotModes, YScale } from "@/types";
import { NumberInput } from "./ui/number-input";

export function SettingsSheet({
  adcMax,
  setAdcMax,
  refVoltage,
  setRefVoltage,
  sampleRate,
  setSampleRate,
  streaming,
  yScale,
  setYScale,
}: {
  adcMax: number;
  setAdcMax: (v: number) => void;
  refVoltage: number;
  setRefVoltage: (v: number) => void;
  sampleRate: number;
  setSampleRate: (v: number) => void;
  streaming: boolean;
  yScale: YScale;
  setYScale: (v: YScale) => void;
  plotMode: PlotModes;
  setPlotMode: (v: PlotModes) => void;
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
          <h3 className="col-span-2 text-sm font-medium">General</h3>
          <Label htmlFor="adc-select" className="text-sm">
            ADC Resolution
          </Label>
          <Select
            value={String(adcMax)}
            onValueChange={(v) => setAdcMax(Number(v))}
          >
            <SelectTrigger id="adc-select" className="w-32 h-8 text-sm">
              <SelectValue />
            </SelectTrigger>
            <SelectContent>
              {ADC_OPTIONS.map((opt) => (
                <SelectItem key={opt.value} value={String(opt.value)}>
                  {opt.label}
                </SelectItem>
              ))}
            </SelectContent>
          </Select>
          <Label htmlFor="samplerate" className="text-sm">
            Sample Rate (S/s)
          </Label>
          <NumberInput
            id="samplerate"
            min={1000}
            step={1000}
            fallback={1000}
            value={sampleRate}
            onChange={setSampleRate}
            className="w-32 h-8 text-sm"
            disabled={streaming}
          />

          <h3 className="col-span-2 mt-2 border-t pt-3 text-sm font-medium">
            Time &amp; Histogram
          </h3>
          <Label htmlFor="yscale-select" className="text-sm">
            Y Scale
          </Label>
          <Select value={yScale} onValueChange={(v) => setYScale(v as YScale)}>
            <SelectTrigger id="yscale-select" className="w-32 h-8 text-sm">
              <SelectValue />
            </SelectTrigger>
            <SelectContent>
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
            <SelectTrigger id="vol-select" className="w-32 h-8 text-sm">
              <SelectValue />
            </SelectTrigger>
            <SelectContent>
              {REF_VOLTAGE_OPTIONS.map((opt) => (
                <SelectItem key={opt.value} value={String(opt.value)}>
                  {opt.label}
                </SelectItem>
              ))}
            </SelectContent>
          </Select>
        </div>
      </SheetContent>
    </Sheet>
  );
}
