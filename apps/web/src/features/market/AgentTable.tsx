import { useMemo } from "react";
import { useNavigate } from "react-router";

import { DataTable, useArrayRows, type DataColumn } from "@/components/tables/DataTable";
import { EmptyState } from "@/components/ui/States";
import type { CursorSample } from "@/features/runs/cursor";
import { POLICY_SHORT } from "@/features/simulations/policySummary";
import type { AgentView, RunDetail } from "@/lib/api/types";
import { cx } from "@/lib/cx";
import { formatInteger, toneOf, type Tone } from "@/lib/formatting";

const TONE: Record<Tone, string> = {
  positive: "text-pos",
  negative: "text-neg",
  neutral: "text-fg-2",
};

function Signed({ value }: { value: number | null | undefined }) {
  return (
    <span className={cx("num", TONE[toneOf(value)])}>{formatInteger(value, { signed: true })}</span>
  );
}

/**
 * Every agent's account at the sample under the cursor, from the accounting stream.
 * Selecting an agent links the rest of the workspace to it.
 */
export function AgentTable({
  run,
  sample,
  selected,
  onSelect,
}: {
  run: RunDetail;
  sample: CursorSample;
  selected: number | null;
  onSelect: (agentId: number | null) => void;
}) {
  const navigate = useNavigate();
  const rows = useArrayRows(run.agents);
  const columns = useMemo<DataColumn<AgentView>[]>(
    () => [
      {
        id: "id",
        header: "ID",
        align: "right",
        width: "38px",
        cell: (agent) => <span className="num text-fg">{agent.id}</span>,
      },
      {
        id: "policy",
        header: "Policy",
        width: "minmax(84px, 1.1fr)",
        cell: (agent) => (
          <span className="text-fg-2">
            {agent.policy ? (POLICY_SHORT[agent.policy] ?? agent.policy) : "scripted"}
          </span>
        ),
      },
      {
        id: "position",
        header: "Position",
        unit: "sh",
        align: "right",
        width: "minmax(72px, 1fr)",
        cell: (agent) => <Signed value={sample.agents[String(agent.id)]?.position} />,
      },
      {
        id: "equity",
        header: "Equity",
        unit: "minor",
        title: "Cash plus position marked at the sample's mark",
        align: "right",
        width: "minmax(88px, 1fr)",
        cell: (agent) => <Signed value={sample.agents[String(agent.id)]?.equity_minor} />,
      },
      {
        id: "realized",
        header: "Realized",
        unit: "minor",
        title: "Realized P&L, gross of fees",
        align: "right",
        width: "minmax(84px, 1fr)",
        cell: (agent) => <Signed value={sample.agents[String(agent.id)]?.realized_pnl_minor} />,
      },
    ],
    [sample],
  );

  return (
    <DataTable
      label="Agent accounts at the cursor"
      columns={columns}
      {...rows}
      getRowId={(agent) => String(agent.id)}
      selectedId={selected === null ? null : String(selected)}
      onSelect={(agent) => onSelect(agent.id === selected ? null : agent.id)}
      onActivate={(agent) => void navigate(`/runs/${run.id}/agents/${agent.id}`)}
      empty={
        <EmptyState title="No accounts">
          This run registered no agents with the exchange.
        </EmptyState>
      }
    />
  );
}
