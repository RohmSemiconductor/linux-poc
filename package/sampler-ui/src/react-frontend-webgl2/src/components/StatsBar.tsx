import type { Stats } from "@/utils/stats";
import type { YScale } from "@/types";
import { Separator } from "./ui/separator";

interface StatsBarProps {
  stats: Stats | null;
  yScale: YScale;
  adcMax: number;
  voltageRef: number;
  total: number;
}

export function StatsBar({
  stats,
  yScale,
  adcMax,
  voltageRef,
  total,
}: StatsBarProps) {
  if (!stats) {
    return (
      <div className="flex items-center gap-4 text-xs text-muted-foreground py-1 px-1 font-mono">
        <Stat label="total" value={total.toLocaleString()} />
      </div>
    );
  }

  const scale = yScale === "voltage" ? voltageRef / adcMax : 1;

  function fmt(raw: number): string {
    const v = raw * scale;
    if (yScale === "voltage") return v.toFixed(4) + " V";
    return v.toFixed(2);
  }

  return (
    <div className="flex items-center gap-4 text-xs text-muted-foreground py-1 px-1 font-mono">
      <Stat label="total" value={total.toLocaleString()} />
      <Separator orientation="vertical" />
      <Stat label="n" value={stats.n.toLocaleString()} />
      <Stat label="min" value={fmt(stats.min)} />
      <Stat label="max" value={fmt(stats.max)} />
      <Stat label="mean" value={fmt(stats.mean)} />
      <Stat label="std" value={fmt(stats.std)} />
      <Stat label="p-p" value={fmt(stats.pp)} />
      {/* <span className="ml-auto">
        <Stat label="total" value={total.toLocaleString()} />
      </span> */}
    </div>
  );
}

function Stat({ label, value }: { label: string; value: string }) {
  return (
    <span>
      <span className="opacity-80">{label}: </span>
      <span className="text-foreground">{value}</span>
    </span>
  );
}
