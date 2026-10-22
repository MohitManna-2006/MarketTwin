import { useNavigate } from "react-router";

import type { Timeline, TimelineNode } from "@/lib/api/types";
import { formatElapsed } from "@/lib/formatting";

// The tree of timelines a run belongs to, drawn on one axis of elapsed simulation time.
// Every row is one run; a fork leaves its parent's row at the moment it was forked, and a
// diamond marks where its future first differs from the parent's.

const ROW = 26;
const LEFT = 220;
const RIGHT = 16;
const WIDTH = 1000;

function order(timeline: Timeline): { node: TimelineNode; depth: number }[] {
  const children = new Map<string, TimelineNode[]>();
  for (const node of timeline.nodes) {
    if (node.parent_run_id) {
      const list = children.get(node.parent_run_id) ?? [];
      list.push(node);
      children.set(node.parent_run_id, list);
    }
  }
  const result: { node: TimelineNode; depth: number }[] = [];
  const visit = (node: TimelineNode, depth: number) => {
    result.push({ node, depth });
    for (const child of children.get(node.id) ?? []) visit(child, depth + 1);
  };
  const root = timeline.nodes.find((node) => node.id === timeline.root_id);
  if (root) visit(root, 0);
  return result;
}

export function changeSummary(node: TimelineNode): string {
  if (!node.parent_run_id) return node.desk ? "AI desk root" : "root run";
  if (node.title) return node.title;
  const stage = node.stage;
  if (!stage) return "fork";
  const parts: string[] = [];
  for (const shock of stage.add_shocks ?? []) parts.push(`+ ${String(shock.kind)}`);
  for (const index of stage.remove_shocks ?? []) parts.push(`− shock ${index}`);
  for (const item of stage.policy_overrides ?? [])
    parts.push(`agent ${item.agent_id} → ${String(item.policy.kind)}`);
  for (const item of stage.directive_overrides ?? [])
    parts.push(`agent ${item.agent_id} directive ${item.directive.mode}`);
  if (stage.decisions && stage.decisions !== "hold") parts.push(`decisions ${stage.decisions}`);
  return parts.join(", ") || "no change";
}

export function TimelineTree({ timeline }: { timeline: Timeline }) {
  const navigate = useNavigate();
  const rows = order(timeline);
  const span = Math.max(1, ...timeline.nodes.map((node) => node.horizon_ns));
  const x = (ns: number) => LEFT + ((WIDTH - LEFT - RIGHT) * ns) / span;
  const y = (row: number) => ROW * row + ROW / 2 + 4;
  const rowOf = new Map(rows.map((item, index) => [item.node.id, index]));
  const height = ROW * rows.length + 22;
  return (
    <svg
      role="group"
      aria-label={`Tree of ${rows.length} timelines over elapsed simulation time`}
      viewBox={`0 0 ${WIDTH} ${height}`}
      className="w-full"
      style={{ height }}
    >
      {[0, 0.25, 0.5, 0.75, 1].map((fraction) => (
        <g key={fraction}>
          <line
            x1={x(span * fraction)}
            x2={x(span * fraction)}
            y1={0}
            y2={height - 18}
            stroke="var(--chart-grid)"
          />
          <text
            x={x(span * fraction)}
            y={height - 4}
            textAnchor="middle"
            className="num"
            fontSize={10}
            fill="var(--chart-axis)"
          >
            {formatElapsed(span * fraction, "s")}
          </text>
        </g>
      ))}
      {rows.map(({ node, depth }, index) => {
        const start = node.fork_at_ns ?? 0;
        const focus = node.id === timeline.focus_id;
        const parentRow = node.parent_run_id ? rowOf.get(node.parent_run_id) : undefined;
        const stroke = focus ? "var(--accent-primary)" : "var(--chart-baseline)";
        const status =
          node.status !== "completed"
            ? "var(--data-warning)"
            : node.fork_status === "failed"
              ? "var(--data-negative)"
              : "var(--data-positive)";
        return (
          <g
            key={node.id}
            role="link"
            tabIndex={0}
            aria-label={`${node.name}: ${changeSummary(node)}${focus ? ", shown" : ""}`}
            className="cursor-pointer"
            onClick={() => void navigate(`/runs/${node.id}/timeline`)}
            onKeyDown={(event) => {
              if (event.key === "Enter") void navigate(`/runs/${node.id}/timeline`);
            }}
          >
            <rect x={0} y={y(index) - ROW / 2} width={WIDTH} height={ROW} fill="transparent" />
            <text
              x={8 + depth * 10}
              y={y(index) + 4}
              fontSize={11}
              fill={focus ? "var(--accent-primary)" : "var(--text-secondary)"}
            >
              {changeSummary(node).slice(0, 30)}
            </text>
            {parentRow !== undefined ? (
              <path
                d={`M ${x(start)} ${y(parentRow)} V ${y(index)}`}
                stroke="var(--chart-axis)"
                strokeDasharray="2 2"
                fill="none"
              />
            ) : null}
            <line
              x1={x(start)}
              x2={x(node.horizon_ns)}
              y1={y(index)}
              y2={y(index)}
              stroke={stroke}
              strokeWidth={focus ? 2.5 : 1.5}
            />
            <circle cx={x(start)} cy={y(index)} r={3.5} fill={status} />
            {node.divergence_ns !== null ? (
              <path
                d={`M ${x(node.divergence_ns)} ${y(index) - 5} l 5 5 l -5 5 l -5 -5 z`}
                fill="var(--data-warning)"
              >
                <title>{`First divergence at ${formatElapsed(node.divergence_ns, "ms")}`}</title>
              </path>
            ) : null}
          </g>
        );
      })}
    </svg>
  );
}
