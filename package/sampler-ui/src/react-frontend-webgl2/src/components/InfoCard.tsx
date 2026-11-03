import { InfoIcon } from "@phosphor-icons/react";
import { Button } from "./ui/button";
import { HoverCard, HoverCardContent, HoverCardTrigger } from "./ui/hover-card";
import type { PlotModes } from "@/types";

interface InfoCardProps {
  plotMode: PlotModes;
}

export function InfoCard({ plotMode }: InfoCardProps) {
  return (
    <HoverCard openDelay={10} closeDelay={100}>
      <HoverCardTrigger asChild>
        <Button variant="outline">
          <InfoIcon />
        </Button>
      </HoverCardTrigger>
      <HoverCardContent className="flex w-80 flex-col gap-2 text-sm">
        <div className="font-semibold">Controls</div>
        <div className="flex flex-col gap-1">
          <div>
            <span className="font-medium">Duration</span> — fixed capture length
            (5-100 s) or Continuous for unlimited streaming.
          </div>
          <div>
            <span className="font-medium">Start / Stop</span> — begin or end ADC
            data streaming from the server.
          </div>
          <div>
            <span className="font-medium">Clear</span> — discard all currently
            recorded data.
          </div>
        </div>
        {plotMode === "time" && (
          <>
            <div className="border-t pt-2 flex flex-col gap-1">
              <div>
                <span className="font-medium">Live</span> — lock the view to the
                latest N samples and auto-scroll as new data arrives.
              </div>
              <div>
                <span className="font-medium">Fit all</span> — zoom out to show
                all recorded data (pan/zoom disabled). Only available when Live
                is on.
              </div>
              <div>
                <span className="font-medium">Window size</span> — number of
                samples shown in the live window.
              </div>
            </div>
            <div className="border-t pt-2 text-xs text-muted-foreground">
              <span className="font-medium">Tip:</span> Double-click the plot
              when Live is off to fit all recorded data into view.
            </div>
          </>
        )}
      </HoverCardContent>
    </HoverCard>
  );
}
