import { scaleLinear } from "d3-scale";

import { useElementSize } from "@/hooks/useElementSize";

export interface IntervalRow {
  key: string;
  label: string;
  /** What kind of interval this is, such as "Student t" or "bootstrap". */
  method: string;
  /** Point estimate and interval in plot units. Null when the core reported none. */
  mean: number | null;
  interval: readonly [number, number] | null;
  /** The same numbers as the core printed them, for the labels. */
  meanText: string;
  intervalText: string;
  color: string;
  /** Rows that are not differences are drawn lighter and never against the zero line. */
  emphasis?: boolean;
}

export interface ConfidenceIntervalPlotProps {
  rows: IntervalRow[];
  /** Draws a reference at zero and keeps it in view: for differences. */
  zero?: boolean;
  format: (value: number) => string;
  unit: string;
  label: string;
}

const ROW = 30;
const LABEL = 178;
const VALUE = 196;
const AXIS = 22;

/**
 * Point estimates with their 95% intervals on one axis. An interval that crosses the
 * zero line is drawn exactly like one that does not: whether zero is inside is for the
 * reader to see, not for a colour to announce.
 */
export function ConfidenceIntervalPlot({
  rows,
  zero = false,
  format,
  unit,
  label,
}: ConfidenceIntervalPlotProps) {
  const [ref, size] = useElementSize<HTMLDivElement>();
  const values = rows.flatMap((row) => [
    ...(row.interval ?? []),
    ...(row.mean === null ? [] : [row.mean]),
  ]);
  if (zero) values.push(0);
  const low = Math.min(...values);
  const high = Math.max(...values);
  const span = high - low || Math.abs(high) || 1;
  const plotWidth = Math.max(80, size.width - LABEL - VALUE - 16);
  const x = scaleLinear()
    .domain([low - span * 0.08, high + span * 0.08])
    .range([0, plotWidth]);
  const height = rows.length * ROW + AXIS;
  const ticks = x.ticks(Math.max(2, Math.floor(plotWidth / 90)));

  return (
    <div ref={ref} className="w-full">
      {size.width > 0 && values.length > 0 ? (
        <svg width={size.width} height={height} role="img" aria-label={label} className="block">
          <title>{label}</title>
          <g transform={`translate(${LABEL}, 0)`}>
            {ticks.map((tick) => (
              <g key={tick} transform={`translate(${x(tick)}, 0)`}>
                <line y1={0} y2={rows.length * ROW} stroke="var(--chart-grid)" />
                <text
                  y={rows.length * ROW + 14}
                  textAnchor="middle"
                  className="num"
                  fontSize={10.5}
                  fill="var(--chart-axis)"
                >
                  {format(tick)}
                </text>
              </g>
            ))}
            {zero ? (
              <g transform={`translate(${x(0)}, 0)`}>
                <line
                  y1={0}
                  y2={rows.length * ROW}
                  stroke="var(--border-strong)"
                  strokeWidth={1.25}
                />
              </g>
            ) : null}
          </g>
          {rows.map((row, index) => {
            const y = index * ROW + ROW / 2;
            const weight = row.emphasis === false ? 1.25 : 2;
            return (
              <g key={row.key}>
                {index > 0 ? (
                  <line
                    x1={0}
                    x2={size.width}
                    y1={index * ROW}
                    y2={index * ROW}
                    stroke="var(--border-subtle)"
                  />
                ) : null}
                <text x={12} y={y - 2} fontSize={12} fill="var(--text-primary)">
                  {row.label}
                </text>
                <text
                  x={12}
                  y={y + 10}
                  fontSize={10}
                  fill="var(--text-tertiary)"
                  style={{ letterSpacing: "0.06em" }}
                >
                  {row.method.toUpperCase()}
                </text>
                <g transform={`translate(${LABEL}, 0)`}>
                  {row.interval ? (
                    <g stroke={row.color} strokeWidth={weight}>
                      <line x1={x(row.interval[0])} x2={x(row.interval[1])} y1={y} y2={y} />
                      <line x1={x(row.interval[0])} x2={x(row.interval[0])} y1={y - 5} y2={y + 5} />
                      <line x1={x(row.interval[1])} x2={x(row.interval[1])} y1={y - 5} y2={y + 5} />
                    </g>
                  ) : null}
                  {row.mean !== null ? (
                    <rect
                      x={x(row.mean) - 3.5}
                      y={y - 3.5}
                      width={7}
                      height={7}
                      transform={`rotate(45 ${x(row.mean)} ${y})`}
                      fill={row.color}
                      stroke="var(--surface-0)"
                    />
                  ) : null}
                </g>
                <text
                  x={size.width - 12}
                  y={y - 2}
                  textAnchor="end"
                  className="num"
                  fontSize={12}
                  fill="var(--text-primary)"
                >
                  {row.meanText}
                </text>
                <text
                  x={size.width - 12}
                  y={y + 10}
                  textAnchor="end"
                  className="num"
                  fontSize={10.5}
                  fill="var(--text-tertiary)"
                >
                  {row.intervalText}
                </text>
              </g>
            );
          })}
          <text
            x={12}
            y={rows.length * ROW + 14}
            fontSize={10}
            fill="var(--text-tertiary)"
            style={{ letterSpacing: "0.06em" }}
          >
            {unit.toUpperCase()}
          </text>
        </svg>
      ) : null}
    </div>
  );
}
