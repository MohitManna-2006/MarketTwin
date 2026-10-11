import { ChevronFirst, ChevronLast, Pause, Play, StepBack, StepForward } from "lucide-react";
import type { ReactNode } from "react";

import { Tooltip } from "@/components/ui/Tooltip";
import { cx } from "@/lib/cx";
import { SAMPLES_PER_SECOND_AT_1X, SPEEDS, usePlayback, type Speed } from "@/state/playback";

function Control({
  label,
  shortcut,
  onClick,
  disabled,
  active,
  children,
}: {
  label: string;
  shortcut: string;
  onClick: () => void;
  disabled?: boolean;
  active?: boolean;
  children: ReactNode;
}) {
  return (
    <Tooltip
      content={
        <span>
          {label} <kbd className="num ml-1 text-fg-3">{shortcut}</kbd>
        </span>
      }
    >
      <button
        type="button"
        aria-label={label}
        disabled={disabled}
        onClick={onClick}
        className={cx(
          "transition-hover inline-flex size-6 items-center justify-center border-r border-line last:border-r-0",
          active ? "bg-accent-wash text-accent" : "text-fg-2 hover:bg-surface-hover hover:text-fg",
          "disabled:text-fg-off disabled:hover:bg-transparent",
        )}
      >
        {children}
      </button>
    </Tooltip>
  );
}

/**
 * Transport for recorded samples. The speed is a viewing pace: it changes how fast the
 * cursor walks the recording, never anything about the run.
 */
export function PlaybackControls() {
  const playing = usePlayback((state) => state.playing);
  const speed = usePlayback((state) => state.speed);
  const count = usePlayback((state) => state.count);
  const atStart = usePlayback((state) => state.index <= 0);
  const atEnd = usePlayback((state) => state.index >= state.count - 1);
  const { seek, step, toggle, setSpeed } = usePlayback.getState();
  const empty = count === 0;

  return (
    <div className="flex items-center gap-2">
      <div className="flex border border-line" role="group" aria-label="Playback">
        <Control
          label="First sample"
          shortcut="Home"
          onClick={() => seek(0)}
          disabled={empty || atStart}
        >
          <ChevronFirst size={13} aria-hidden />
        </Control>
        <Control
          label="Previous sample"
          shortcut="←"
          onClick={() => step(-1)}
          disabled={empty || atStart}
        >
          <StepBack size={12} aria-hidden />
        </Control>
        <Control
          label={playing ? "Pause" : "Play"}
          shortcut="Space"
          onClick={toggle}
          disabled={count < 2}
          active={playing}
        >
          {playing ? <Pause size={12} aria-hidden /> : <Play size={12} aria-hidden />}
        </Control>
        <Control label="Next sample" shortcut="→" onClick={() => step(1)} disabled={empty || atEnd}>
          <StepForward size={12} aria-hidden />
        </Control>
        <Control
          label="Last sample"
          shortcut="End"
          onClick={() => seek(count - 1)}
          disabled={empty || atEnd}
        >
          <ChevronLast size={13} aria-hidden />
        </Control>
      </div>
      <Tooltip
        content={`Viewing pace only. 1× steps ${SAMPLES_PER_SECOND_AT_1X} recorded samples per second; the run itself is already complete.`}
      >
        <label className="flex h-6 items-center border border-line">
          <span className="label border-r border-line px-1.5 text-fg-3">Pace</span>
          <select
            aria-label="Playback pace"
            value={speed}
            onChange={(event) => setSpeed(Number(event.target.value) as Speed)}
            className="num h-full appearance-none bg-transparent px-1.5 text-xs text-fg outline-none"
          >
            {SPEEDS.map((option) => (
              <option key={option} value={option}>
                {option}×
              </option>
            ))}
          </select>
        </label>
      </Tooltip>
    </div>
  );
}
