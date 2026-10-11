import { extent } from "d3-array";
import { scaleLinear } from "d3-scale";

import { useElementSize } from "@/hooks/useElementSize";

export interface PairedPoint {
  seed: number;
  baseline: number;
  candidate: number;
}

export interface PairedScatterProps {
  points: PairedPoint[];
  selectedSeed: number | null;
  onSelect: (seed: number) => void;
  format: (value: number) => string;
  baselineLabel: string;
  candidateLabel: string;
  label: string;
}

const MARGIN = { top: 10, right: 14, bottom: 34, left: 72 };

/**
 * One point per seed: the baseline's value against the candidate's on that same seed.
 * Both axes share a scale, so the diagonal is where the two are equal; a point above it
 * is a seed where the candidate's value was higher.
 */
export function PairedScatter({
  points,
  selectedSeed,
  onSelect,
  format,
  baselineLabel,
  candidateLabel,
  label,
}: PairedScatterProps) {
  const [ref, size] = useElementSize<HTMLDivElement>();
  const [minimum = 0, maximum = 1] = extent(
    points.flatMap((point) => [point.baseline, point.candidate]),
  );
  const span = maximum - minimum || 1;
  const width = Math.max(40, size.width - MARGIN.left - MARGIN.right);
  const height = Math.max(40, size.height - MARGIN.top - MARGIN.bottom);
  const domain: [number, number] = [minimum - span * 0.05, maximum + span * 0.05];
  const x = scaleLinear().domain(domain).range([0, width]);
  const y = scaleLinear().domain(domain).range([height, 0]);
  const ticks = x.ticks(Math.max(2, Math.floor(Math.min(width, height) / 70)));

  return (
    <div ref={ref} className="absolute inset-0">
      {size.width > 0 && points.length > 0 ? (
        <svg
          width={size.width}
          height={size.height}
          role="img"
          aria-label={label}
          className="block"
        >
          <title>{label}</title>
          <g transform={`translate(${MARGIN.left}, ${MARGIN.top})`}>
            {ticks.map((tick) => (
              <g key={tick}>
                <line x1={x(tick)} x2={x(tick)} y2={height} stroke="var(--chart-grid)" />
                <line y1={y(tick)} y2={y(tick)} x2={width} stroke="var(--chart-grid)" />
                <text
                  x={x(tick)}
                  y={height + 14}
                  textAnchor="middle"
                  className="num"
                  fontSize={10.5}
                  fill="var(--chart-axis)"
                >
                  {format(tick)}
                </text>
                <text
                  x={-6}
                  y={y(tick)}
                  dy="0.32em"
                  textAnchor="end"
                  className="num"
                  fontSize={10.5}
                  fill="var(--chart-axis)"
                >
                  {format(tick)}
                </text>
              </g>
            ))}
            <line
              x1={x(domain[0])}
              y1={y(domain[0])}
              x2={x(domain[1])}
              y2={y(domain[1])}
              stroke="var(--border-strong)"
              strokeDasharray="4 3"
            />
            <text
              x={x(domain[1]) - 4}
              y={y(domain[1]) + 12}
              textAnchor="end"
              fontSize={9.5}
              fill="var(--text-tertiary)"
              style={{ letterSpacing: "0.06em" }}
            >
              EQUAL
            </text>
            {points.map((point) => {
              const selected = point.seed === selectedSeed;
              return (
                <circle
                  key={point.seed}
                  cx={x(point.baseline)}
                  cy={y(point.candidate)}
                  r={selected ? 4.5 : 2.5}
                  fill={selected ? "var(--accent-primary)" : "var(--chart-baseline)"}
                  fillOpacity={selected ? 1 : 0.7}
                  stroke={selected ? "var(--text-primary)" : "none"}
                  onClick={() => onSelect(point.seed)}
                  style={{ cursor: "pointer" }}
                >
                  <title>{`seed ${point.seed}: ${baselineLabel} ${format(point.baseline)}, ${candidateLabel} ${format(point.candidate)}`}</title>
                </circle>
              );
            })}
            <rect width={width} height={height} fill="none" stroke="var(--border-default)" />
          </g>
          <text
            x={MARGIN.left + width / 2}
            y={size.height - 4}
            textAnchor="middle"
            fontSize={10}
            fill="var(--text-tertiary)"
            style={{ letterSpacing: "0.06em" }}
          >
            {baselineLabel.toUpperCase()}
          </text>
          <text
            transform={`translate(10, ${MARGIN.top + height / 2}) rotate(-90)`}
            textAnchor="middle"
            fontSize={10}
            fill="var(--text-tertiary)"
            style={{ letterSpacing: "0.06em" }}
          >
            {candidateLabel.toUpperCase()}
          </text>
        </svg>
      ) : null}
    </div>
  );
}
