import { bin, extent } from "d3-array";
import { scaleLinear } from "d3-scale";

import { useElementSize } from "@/hooks/useElementSize";

export interface HistogramGroup {
  key: string;
  label: string;
  color: string;
  /** Per-seed values in plot units, as the core reported them. */
  values: readonly number[];
  dashed?: boolean;
}

export interface HistogramProps {
  groups: HistogramGroup[];
  /** Draws a reference at zero and keeps it in view: for paired differences. */
  zero?: boolean;
  format: (value: number) => string;
  label: string;
  /** Optional vertical references, such as a mean the core reported. */
  references?: { value: number; label: string; color: string }[];
}

const MARGIN = { top: 10, right: 14, bottom: 24, left: 34 };

/**
 * Distribution of per-seed values. Bins are a drawing choice made here; the values are
 * the core's. Two groups share one set of bins so their overlap is visible, and each is
 * drawn as an outline over a light fill.
 */
export function Histogram({
  groups,
  zero = false,
  format,
  label,
  references = [],
}: HistogramProps) {
  const [ref, size] = useElementSize<HTMLDivElement>();
  const all = groups.flatMap((group) => [...group.values]);
  const [minimum = 0, maximum = 1] = extent(all);
  const low = zero ? Math.min(minimum, 0) : minimum;
  const high = zero ? Math.max(maximum, 0) : maximum;
  const span = high - low || 1;
  const width = Math.max(40, size.width - MARGIN.left - MARGIN.right);
  const height = Math.max(40, size.height - MARGIN.top - MARGIN.bottom);
  const x = scaleLinear()
    .domain([low - span * 0.04, high + span * 0.04])
    .nice()
    .range([0, width]);
  const count = Math.max(
    6,
    Math.min(28, Math.round(Math.sqrt(Math.max(1, all.length / Math.max(1, groups.length))) * 2)),
  );
  const binner = bin<number, number>()
    .domain(x.domain() as [number, number])
    .thresholds(x.ticks(count));
  const binned = groups.map((group) => ({ group, bins: binner([...group.values]) }));
  const peak = Math.max(1, ...binned.flatMap((item) => item.bins.map((bucket) => bucket.length)));
  const y = scaleLinear().domain([0, peak]).nice().range([height, 0]);

  function outline(bins: ReturnType<typeof binner>): string {
    let path = "";
    for (const bucket of bins) {
      const x0 = x(bucket.x0 ?? 0);
      const x1 = x(bucket.x1 ?? 0);
      const top = y(bucket.length);
      path += path === "" ? `M${x0},${height}V${top}H${x1}` : `V${top}H${x1}`;
    }
    return path === "" ? "" : `${path}V${height}`;
  }

  return (
    <div ref={ref} className="absolute inset-0">
      {size.width > 0 && all.length > 0 ? (
        <svg
          width={size.width}
          height={size.height}
          role="img"
          aria-label={label}
          className="block"
        >
          <title>{label}</title>
          <g transform={`translate(${MARGIN.left}, ${MARGIN.top})`}>
            {y.ticks(3).map((tick) => (
              <g key={tick} transform={`translate(0, ${y(tick)})`}>
                <line x2={width} stroke="var(--chart-grid)" />
                <text
                  x={-6}
                  dy="0.32em"
                  textAnchor="end"
                  className="num"
                  fontSize={10.5}
                  fill="var(--chart-axis)"
                >
                  {Number.isInteger(tick) ? tick : ""}
                </text>
              </g>
            ))}
            {x.ticks(Math.max(2, Math.floor(width / 80))).map((tick) => (
              <text
                key={tick}
                x={x(tick)}
                y={height + 15}
                textAnchor="middle"
                className="num"
                fontSize={10.5}
                fill="var(--chart-axis)"
              >
                {format(tick)}
              </text>
            ))}
            {binned.map(({ group, bins }) => (
              <path
                key={group.key}
                d={outline(bins)}
                fill={group.color}
                fillOpacity={0.12}
                stroke={group.color}
                strokeWidth={1.25}
                strokeDasharray={group.dashed ? "4 3" : undefined}
              />
            ))}
            {zero ? (
              <line
                x1={x(0)}
                x2={x(0)}
                y1={0}
                y2={height}
                stroke="var(--border-strong)"
                strokeWidth={1.25}
              />
            ) : null}
            {references.map((reference) => (
              <g key={reference.label} transform={`translate(${x(reference.value)}, 0)`}>
                <line y1={0} y2={height} stroke={reference.color} strokeDasharray="2 3" />
                <text
                  y={8}
                  x={4}
                  fontSize={9.5}
                  fill={reference.color}
                  style={{ letterSpacing: "0.06em" }}
                >
                  {reference.label.toUpperCase()}
                </text>
              </g>
            ))}
            <line x2={width} y1={height} y2={height} stroke="var(--border-default)" />
          </g>
        </svg>
      ) : null}
    </div>
  );
}
