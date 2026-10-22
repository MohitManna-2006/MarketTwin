import { useMutation, useQuery, useQueryClient } from "@tanstack/react-query";
import { useState } from "react";
import { useNavigate } from "react-router";

import { Button } from "@/components/ui/Button";
import { DurationField } from "@/components/ui/DurationField";
import { Checkbox, IntegerField, SelectField, TextField } from "@/components/ui/Field";
import { SegmentedControl } from "@/components/ui/SegmentedControl";
import { ErrorState } from "@/components/ui/States";
import { shockLabel } from "@/features/runs/shocks";
import { post } from "@/lib/api/client";
import { fieldCatalogueQuery } from "@/lib/api/queries";
import type { ForkStageRequest, RunDetail, RunSummary } from "@/lib/api/types";
import { formatElapsed } from "@/lib/formatting";

// One change to a finished run from a chosen moment on (ADR 0012). The core builds the
// fork's config from the parent's and refuses any change that could reach the past.

type Change = "shock" | "remove" | "policy" | "none";
type ShockKind =
  "QuoteWithdrawalStress" | "FundamentalJump" | "LatencyDegradation" | "LiquidityWithdrawal";

const SECOND = 1_000_000_000;

interface AgentConfig {
  id: number;
  policy: { kind: string };
}
interface ShockConfig {
  kind: string;
  at_ns?: number;
  start_ns?: number;
  end_ns?: number;
}

function scenarioOf(run: RunDetail): { agents: AgentConfig[]; shocks: ShockConfig[] } {
  const scenario = (run.config as { scenario?: { agents: AgentConfig[]; shocks: ShockConfig[] } })
    .scenario;
  return { agents: scenario?.agents ?? [], shocks: scenario?.shocks ?? [] };
}

const MAKERS = new Set(["MarketMaker", "InventoryMarketMaker", "ModelDirected"]);

export interface ForkFormProps {
  run: RunDetail;
  /** Where the form starts, such as the time under the chart cursor. */
  at: number;
}

export function ForkForm({ run, at: initialAt }: ForkFormProps) {
  const navigate = useNavigate();
  const client = useQueryClient();
  const catalogue = useQuery(fieldCatalogueQuery());
  const { agents, shocks } = scenarioOf(run);
  const horizon = run.horizon_ns;
  const [at, setAt] = useState(Math.min(initialAt, horizon - SECOND));
  const [change, setChange] = useState<Change>("shock");
  const [title, setTitle] = useState("");
  const [kind, setKind] = useState<ShockKind>("QuoteWithdrawalStress");
  const [chosen, setChosen] = useState<number[]>(
    agents.filter((agent) => MAKERS.has(agent.policy.kind)).map((agent) => agent.id),
  );
  const [duration, setDuration] = useState(60 * SECOND);
  const [delta, setDelta] = useState<number | null>(50_000);
  const [delay, setDelay] = useState(50_000_000);
  const [removed, setRemoved] = useState<number[]>([]);
  const [agent, setAgent] = useState(String(agents[0]?.id ?? 1));
  const [policy, setPolicy] = useState("InventoryMarketMaker");
  const [decisions, setDecisions] = useState<"hold" | "parent" | "live">("live");
  const desk = run.desk !== null && run.desk !== undefined;

  const pending = shocks
    .map((shock, index) => ({ shock, index, first: shock.at_ns ?? shock.start_ns ?? 0 }))
    .filter((item) => item.first >= at);

  function stage(): ForkStageRequest {
    const request: ForkStageRequest = { at_ns: at };
    if (desk) request.decisions = decisions;
    if (change === "remove")
      request.remove_shocks = removed.filter((index) =>
        pending.some((item) => item.index === index),
      );
    if (change === "policy") {
      const template = catalogue.data?.policies.find((item) => item.kind === policy)?.template as
        { policy: Record<string, unknown> } | null | undefined;
      if (template)
        request.policy_overrides = [{ agent_id: Number(agent), policy: template.policy }];
    }
    if (change === "shock") {
      const end = Math.min(at + duration, horizon);
      if (kind === "QuoteWithdrawalStress")
        request.add_shocks = [{ kind, at_ns: at, agents: chosen }];
      if (kind === "FundamentalJump")
        request.add_shocks = [{ kind, at_ns: at, delta_milliticks: delta ?? 0 }];
      if (kind === "LatencyDegradation")
        request.add_shocks = [
          {
            kind,
            start_ns: at,
            end_ns: end,
            agents: chosen,
            added_order_entry_ns: delay,
            added_report_ns: 0,
            added_market_data_ns: delay,
          },
        ];
      if (kind === "LiquidityWithdrawal")
        request.add_shocks = [
          {
            kind,
            start_ns: at,
            end_ns: end,
            agents: chosen.filter((id) =>
              MAKERS.has(agents.find((item) => item.id === id)?.policy.kind ?? ""),
            ),
            quote_size_ppm: 0,
          },
        ];
    }
    return request;
  }

  const create = useMutation({
    mutationFn: () =>
      post<RunSummary>(`/runs/${run.id}/forks`, { title: title || null, ...stage() }),
    onSuccess: (created) => {
      void client.invalidateQueries({ queryKey: ["runs", run.id, "timeline"] });
      void navigate(`/runs/${created.id}/timeline`);
    },
  });

  const policies = (catalogue.data?.policies ?? []).filter((item) => item.template);

  return (
    <form
      className="flex flex-col gap-3 p-3"
      onSubmit={(event) => {
        event.preventDefault();
        create.mutate();
      }}
    >
      <p className="text-xs text-fg-3">
        The engine replays this run exactly up to the fork, then applies one change. Everything
        before {formatElapsed(at, "s")} stays identical, and the fork is checked against it.
      </p>
      <DurationField
        label="Fork at"
        value={at}
        onChange={setAt}
        min={0}
        max={horizon - 1}
        hint="Elapsed simulation time of the fork"
      />
      <SegmentedControl
        label="Change"
        value={change}
        onChange={setChange}
        options={[
          { value: "shock", label: "Add shock" },
          { value: "remove", label: "Remove shock", disabled: pending.length === 0 },
          { value: "policy", label: "Switch policy" },
          { value: "none", label: desk ? "Decisions only" : "Nothing" },
        ]}
      />
      {change === "shock" ? (
        <div className="flex flex-col gap-2">
          <SelectField
            label="Shock"
            value={kind}
            onChange={(value) => setKind(value as ShockKind)}
            options={(
              [
                "QuoteWithdrawalStress",
                "FundamentalJump",
                "LatencyDegradation",
                "LiquidityWithdrawal",
              ] as const
            ).map((value) => ({ value, label: shockLabel(value) }))}
          />
          {kind === "FundamentalJump" ? (
            <IntegerField
              label="Jump"
              unit="milliticks"
              value={delta}
              onChange={setDelta}
              signed
              min={-1_000_000_000}
              max={1_000_000_000}
            />
          ) : (
            <fieldset className="flex flex-col gap-1">
              <legend className="label mb-1 text-fg-3">Agents</legend>
              <div className="grid grid-cols-2 gap-1">
                {agents.map((item) => (
                  <Checkbox
                    key={item.id}
                    label={`${item.id} · ${item.policy.kind}`}
                    checked={chosen.includes(item.id)}
                    onChange={(checked) =>
                      setChosen(
                        checked ? [...chosen, item.id] : chosen.filter((id) => id !== item.id),
                      )
                    }
                  />
                ))}
              </div>
            </fieldset>
          )}
          {kind === "LatencyDegradation" || kind === "LiquidityWithdrawal" ? (
            <DurationField label="For" value={duration} onChange={setDuration} min={1} />
          ) : null}
          {kind === "LatencyDegradation" ? (
            <DurationField label="Added delay" value={delay} onChange={setDelay} min={1} />
          ) : null}
        </div>
      ) : null}
      {change === "remove" ? (
        <fieldset className="flex flex-col gap-1">
          <legend className="label mb-1 text-fg-3">Shocks that have not started</legend>
          {pending.map((item) => (
            <Checkbox
              key={item.index}
              label={`${shockLabel(item.shock.kind)} at ${formatElapsed(item.first, "s")}`}
              checked={removed.includes(item.index)}
              onChange={(checked) =>
                setRemoved(
                  checked ? [...removed, item.index] : removed.filter((i) => i !== item.index),
                )
              }
            />
          ))}
        </fieldset>
      ) : null}
      {change === "policy" ? (
        <div className="grid grid-cols-2 gap-2">
          <SelectField
            label="Agent"
            value={agent}
            onChange={setAgent}
            options={agents.map((item) => ({
              value: String(item.id),
              label: `${item.id} · ${item.policy.kind}`,
            }))}
          />
          <SelectField
            label="New policy"
            value={policy}
            onChange={setPolicy}
            options={policies.map((item) => ({ value: item.kind, label: item.kind }))}
          />
          <p className="col-span-2 text-xs text-fg-3">
            The agent keeps its position, orders, latency, and random streams. Only its decision
            logic changes, and the new policy starts with no memory.
          </p>
        </div>
      ) : null}
      {desk ? (
        <SelectField
          label="AI desk after the fork"
          value={decisions}
          onChange={(value) => setDecisions(value as "hold" | "parent" | "live")}
          options={[
            { value: "live", label: "Decides again on the new path" },
            { value: "parent", label: "Repeats the parent's decisions" },
            { value: "hold", label: "Holds its directive" },
          ]}
        />
      ) : null}
      <TextField label="Title" value={title} onChange={setTitle} maxLength={120} />
      <div className="flex items-center gap-2">
        <Button type="submit" variant="solid" disabled={create.isPending}>
          {create.isPending ? "Forking…" : "Fork this run"}
        </Button>
        <span className="text-xs text-fg-3">One sample path; repeat over seeds for an effect.</span>
      </div>
      {create.isError ? <ErrorState error={create.error} compact /> : null}
    </form>
  );
}
