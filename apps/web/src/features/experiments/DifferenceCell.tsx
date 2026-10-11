import type { StatSummary } from "@/lib/api/types";
import { UNIT_LABELS, formatMilliOf } from "@/lib/formatting";

import { metricLabel } from "./metrics";

interface Headline {
  metric: string;
  unit: string;
  pairs: number;
  difference: StatSummary | null;
}

/**
 * A mean paired difference with its interval, as the core reported it. The interval is
 * printed beside the mean so a difference is never read without its uncertainty.
 */
export function DifferenceCell({ headline }: { headline: unknown }) {
  const data = headline as Headline | null;
  if (!data || !data.difference) {
    return <span className="num text-fg-3">—</span>;
  }
  const { difference, unit } = data;
  const interval = difference.mean_t_interval_milli;
  return (
    <span className="num flex items-baseline justify-end gap-2">
      <span className="font-sans text-xs text-fg-3">{metricLabel(data.metric)}</span>
      <span className="font-medium text-fg">
        {formatMilliOf(unit, difference.mean_milli, { signed: true })}
      </span>
      <span className="text-fg-3">
        {interval
          ? `[${formatMilliOf(unit, interval[0], { signed: true })}, ${formatMilliOf(unit, interval[1], { signed: true })}]`
          : "no interval"}
      </span>
      <span className="text-2xs text-fg-3">{UNIT_LABELS[unit] ?? unit}</span>
    </span>
  );
}
