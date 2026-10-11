import type { ReactNode } from "react";

import { cx } from "@/lib/cx";
import { UNAVAILABLE, type Tone } from "@/lib/formatting";

const TONE: Record<Tone, string> = {
  positive: "text-pos",
  negative: "text-neg",
  neutral: "text-fg",
};

export interface MetricReadoutProps {
  label: string;
  /** Already formatted. Pass UNAVAILABLE for a value the run does not define. */
  value: string;
  unit?: string;
  /** Direction of a signed financial quantity. The sign is in the value itself. */
  tone?: Tone;
  /** Where the number comes from or what it qualifies, shown under it. */
  note?: ReactNode;
  size?: "lg" | "md" | "sm";
  title?: string;
  className?: string;
}

const SIZE = {
  lg: "text-xl leading-6",
  md: "text-md leading-5",
  sm: "text-base leading-4",
};

/** An instrument-style numeric readout: label, value, unit. */
export function MetricReadout({
  label,
  value,
  unit,
  tone = "neutral",
  note,
  size = "md",
  title,
  className,
}: MetricReadoutProps) {
  const missing = value === UNAVAILABLE;
  return (
    <div className={cx("flex min-w-0 flex-col gap-0.5", className)} title={title}>
      <span className="label truncate text-fg-3">{label}</span>
      <span className="flex items-baseline gap-1">
        <span
          className={cx("num font-medium", SIZE[size], missing ? "text-fg-3" : TONE[tone])}
          aria-label={missing ? `${label}: unavailable` : undefined}
        >
          {value}
        </span>
        {unit && !missing ? <span className="text-2xs text-fg-3">{unit}</span> : null}
      </span>
      {note ? <span className="truncate text-2xs text-fg-3">{note}</span> : null}
    </div>
  );
}
