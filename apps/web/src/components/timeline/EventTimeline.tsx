import { useRef } from "react";

import type { ChartBand, ChartEvent, XRange } from "@/components/charts/TimeSeriesChart";
import { useHotkeys } from "@/hooks/useHotkeys";
import { formatElapsed, formatInteger } from "@/lib/formatting";
import { usePlayback } from "@/state/playback";

import { PlaybackControls } from "./PlaybackControls";

export interface EventTimelineProps {
  /** Elapsed simulation time of every recorded sample, ns. */
  times: readonly number[];
  events?: readonly ChartEvent[];
  bands?: readonly ChartBand[];
  /** The chart's visible range, drawn as a bracket on the track. */
  xRange?: XRange | null;
  note?: string;
}

/** Keys that belong to a focused control are left to it. */
function ownsKeys(target: EventTarget | null): boolean {
  if (!(target instanceof HTMLElement)) return false;
  if (target.closest("[data-timeline]")) return false;
  return (
    target.closest("button, a, [role='table'], [role='radio'], [role='menuitem'], summary") !== null
  );
}

/**
 * The timeline: the one control every panel of the workspace follows. Its position is an
 * index into the recorded samples, so the cursor is always exactly on an observation.
 */
export function EventTimeline({
  times,
  events = [],
  bands = [],
  xRange,
  note,
}: EventTimelineProps) {
  const index = usePlayback((state) => state.index);
  const count = times.length;
  const scope = useRef<HTMLDivElement>(null);
  const first = times[0] ?? 0;
  const last = times[count - 1] ?? first;
  const span = Math.max(1, last - first);
  const percent = (ns: number) => `${Math.min(100, Math.max(0, ((ns - first) / span) * 100))}%`;
  const now = times[index] ?? null;

  const guard = (action: () => void) => (event: KeyboardEvent) => {
    if (ownsKeys(event.target)) return;
    // The scrubber handles its own arrows; only Space and the pace keys reach it from here.
    const onScrubber = event.target instanceof HTMLInputElement && event.target.type === "range";
    if (onScrubber && event.key !== " " && event.key !== "[" && event.key !== "]") return;
    event.preventDefault();
    action();
  };
  const playback = usePlayback.getState();
  useHotkeys(
    {
      Space: guard(() => playback.toggle()),
      ArrowLeft: guard(() => playback.step(-1)),
      ArrowRight: guard(() => playback.step(1)),
      "Shift+ArrowLeft": guard(() => playback.step(-60)),
      "Shift+ArrowRight": guard(() => playback.step(60)),
      Home: guard(() => playback.seek(0)),
      End: guard(() => playback.seek(count - 1)),
      "[": guard(() => playback.slower()),
      "]": guard(() => playback.faster()),
    },
    { enabled: count > 0 },
  );

  return (
    <div
      ref={scope}
      data-timeline
      className="flex min-h-10 shrink-0 flex-wrap items-center gap-x-3 gap-y-1 border-y border-line bg-surface-1 px-3 max-md:py-1.5"
    >
      <PlaybackControls />
      <div className="flex min-w-[168px] flex-col leading-tight">
        <span className="num text-md font-medium text-fg" aria-live="off">
          {formatElapsed(now, "ms")}
        </span>
        <span className="label text-fg-3">
          Elapsed · sample{" "}
          <span className="num tracking-normal text-fg-2">
            {count === 0 ? "—" : formatInteger(index + 1)}
          </span>{" "}
          of <span className="num tracking-normal">{formatInteger(count)}</span>
        </span>
      </div>
      <div className="relative h-6 min-w-0 flex-1 max-md:basis-full has-[:focus-visible]:outline has-[:focus-visible]:outline-1 has-[:focus-visible]:outline-offset-2 has-[:focus-visible]:outline-focus">
        <div className="absolute inset-x-0 top-1/2 h-2.5 -translate-y-1/2 border border-line-strong bg-canvas">
          {bands.map((band, position) => (
            <span
              key={position}
              className="absolute inset-y-0 bg-warn/20"
              style={{ left: percent(band.from), right: `calc(100% - ${percent(band.to)})` }}
            />
          ))}
          {xRange ? (
            <span
              className="absolute -inset-y-px border-x border-fg-3 bg-fg/5"
              style={{ left: percent(xRange[0]), right: `calc(100% - ${percent(xRange[1])})` }}
              title="Time range shown in the charts"
            />
          ) : null}
          <span
            className="absolute inset-y-0 left-0 bg-accent/25"
            style={{ width: percent(now ?? first) }}
          />
        </div>
        {events.map((event, position) => (
          <span
            key={position}
            title={`${event.label} · scheduled at ${formatElapsed(event.ns, "ms")}`}
            className="absolute top-0 bottom-0 w-px bg-warn"
            style={{ left: percent(event.ns) }}
          />
        ))}
        <span
          aria-hidden
          className="pointer-events-none absolute top-0 bottom-0 -ml-px w-0.5 bg-accent"
          style={{ left: percent(now ?? first) }}
        />
        <input
          type="range"
          aria-label="Timeline: recorded sample under the cursor"
          aria-valuetext={
            now === null
              ? "no samples"
              : `${formatElapsed(now, "ms")} elapsed, sample ${index + 1} of ${count}`
          }
          min={0}
          max={Math.max(0, count - 1)}
          step={1}
          value={index}
          disabled={count === 0}
          onChange={(event) => {
            playback.pause();
            playback.seek(Number(event.target.value));
          }}
          className="absolute inset-0 h-full w-full cursor-ew-resize opacity-0"
        />
      </div>
      <div className="hidden min-w-[150px] flex-col items-end leading-tight xl:flex">
        <span className="num text-xs text-fg-2">{formatElapsed(last, "s")} recorded</span>
        <span className="label text-fg-3">{note ?? "elapsed simulation time"}</span>
      </div>
    </div>
  );
}
