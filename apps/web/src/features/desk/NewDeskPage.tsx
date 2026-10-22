import { useMutation, useQuery } from "@tanstack/react-query";
import { useState } from "react";
import { useNavigate } from "react-router";

import { ContextBar } from "@/components/shell/ContextBar";
import { Page } from "@/components/shell/TerminalShell";
import { Button } from "@/components/ui/Button";
import { DurationField } from "@/components/ui/DurationField";
import { Checkbox, IntegerField, SelectField } from "@/components/ui/Field";
import { InstrumentPanel } from "@/components/ui/InstrumentPanel";
import { KeyValue } from "@/components/ui/KeyValue";
import { SegmentedControl } from "@/components/ui/SegmentedControl";
import { ErrorState, LoadingState } from "@/components/ui/States";
import { post } from "@/lib/api/client";
import { scenarioQuery, scenariosQuery } from "@/lib/api/queries";
import type { DeskEstimate, DeskRequest, RunSummary } from "@/lib/api/types";
import { formatInteger, formatSpan } from "@/lib/formatting";

// Hands one agent of a catalogued scenario to the AI desk (ADR 0012). A model desk shows
// how many times it will ask the model and starts only once exactly that many are approved.

const MINUTE = 60_000_000_000;
const MAKERS = new Set(["MarketMaker", "InventoryMarketMaker"]);
const DEFAULT_MANDATE =
  "Make markets in this name. Keep inventory within your position cap, and protect the desk " +
  "when the market becomes one-sided or unstable.";

interface ScenarioAgent {
  id: number;
  policy: { kind: string };
}

export function NewDeskPage() {
  const navigate = useNavigate();
  const scenarios = useQuery(scenariosQuery());
  const choices = (scenarios.data ?? []).filter((item) => item.kind === "scenario");
  const [scenario, setScenario] = useState("latency_shock");
  const detail = useQuery({ ...scenarioQuery(scenario), enabled: scenario !== "" });
  const agents = (
    (detail.data?.config as { scenario?: { agents: ScenarioAgent[] } } | undefined)?.scenario
      ?.agents ?? []
  ).filter((agent) => MAKERS.has(agent.policy.kind));
  const [agent, setAgent] = useState<number | null>(null);
  const subject = agent ?? agents.at(-1)?.id ?? 1;
  const [seed, setSeed] = useState<number | null>(42);
  const [horizon, setHorizon] = useState(60 * MINUTE);
  const [interval, setInterval] = useState(MINUTE);
  const [mandate, setMandate] = useState(DEFAULT_MANDATE);
  const [source, setSource] = useState<"fixture" | "model">("fixture");
  const [approved, setApproved] = useState(false);

  const request: DeskRequest = {
    scenario,
    agent_id: subject,
    seed: seed ?? 0,
    horizon_ns: horizon,
    mandate,
    decision_interval_ns: interval,
    max_half_spread_ticks: 20,
    max_skew_ticks: 10,
    source,
  };
  const estimate = useQuery({
    queryKey: ["desks", "estimate", request],
    queryFn: () => post<DeskEstimate>("/desks/estimate", request),
    enabled: detail.isSuccess,
  });
  const calls = estimate.data?.model_calls ?? 0;
  const create = useMutation({
    mutationFn: () =>
      post<RunSummary>("/desks", { ...request, approved_calls: source === "model" ? calls : null }),
    onSuccess: (run) => void navigate(`/runs/${run.id}/desk`),
  });
  const blocked =
    source === "model" && (!approved || !estimate.data?.model_configured || calls === 0);

  return (
    <Page scroll context={<ContextBar crumbs={[{ label: "AI Desk" }, { label: "New desk" }]} />}>
      <div className="grid min-h-full grid-cols-1 gap-px bg-line-subtle lg:grid-cols-[minmax(0,1fr)_minmax(0,24rem)]">
        <InstrumentPanel index="01" title="Hand one agent to a model">
          {scenarios.isPending ? (
            <LoadingState label="Loading scenarios" />
          ) : (
            <form
              className="grid max-w-2xl grid-cols-1 gap-3 p-4 sm:grid-cols-2"
              onSubmit={(event) => {
                event.preventDefault();
                create.mutate();
              }}
            >
              <SelectField
                label="Scenario"
                value={scenario}
                onChange={(value) => {
                  setScenario(value);
                  setAgent(null);
                }}
                options={choices.map((item) => ({ value: item.key, label: item.title }))}
              />
              <SelectField
                label="Agent handed to the desk"
                value={String(subject)}
                onChange={(value) => setAgent(Number(value))}
                options={agents.map((item) => ({
                  value: String(item.id),
                  label: `${item.id} · replaces ${item.policy.kind}`,
                }))}
              />
              <IntegerField label="Seed" value={seed} onChange={setSeed} min={0} />
              <DurationField
                label="Horizon"
                value={horizon}
                onChange={setHorizon}
                min={MINUTE}
                max={4 * 60 * MINUTE}
              />
              <DurationField
                label="Decides every"
                value={interval}
                onChange={setInterval}
                min={1_000_000_000}
              />
              <div className="flex flex-col gap-1">
                <span className="label text-fg-3">Decisions come from</span>
                <SegmentedControl
                  label="Decision source"
                  value={source}
                  onChange={(value) => {
                    setSource(value);
                    setApproved(false);
                  }}
                  options={[
                    { value: "fixture", label: "Fixture model" },
                    { value: "model", label: "Configured model" },
                  ]}
                />
              </div>
              <label className="flex flex-col gap-1 sm:col-span-2">
                <span className="label text-fg-3">Mandate</span>
                <textarea
                  value={mandate}
                  maxLength={2000}
                  rows={4}
                  onChange={(event) =>
                    setMandate(event.target.value.replace(/[^\n\x20-\x7e]/g, ""))
                  }
                  className="rounded-md border border-line bg-canvas px-1.5 py-1 text-sm text-fg outline-none focus-visible:border-focus"
                />
              </label>
              {source === "model" ? (
                <div className="sm:col-span-2">
                  <Checkbox
                    checked={approved}
                    onChange={setApproved}
                    label={`I approve ${formatInteger(calls)} calls to ${estimate.data?.model || "the configured model"}.`}
                  />
                </div>
              ) : null}
              <div className="flex items-center gap-2 sm:col-span-2">
                <Button type="submit" variant="solid" disabled={blocked || create.isPending}>
                  {create.isPending ? "Starting…" : "Start the desk"}
                </Button>
                {create.isError ? <ErrorState error={create.error} compact /> : null}
              </div>
            </form>
          )}
        </InstrumentPanel>
        <InstrumentPanel index="02" title="What the desk does">
          <KeyValue
            rows={[
              { label: "Decisions", value: formatInteger(estimate.data?.decisions) },
              { label: "Model calls", value: formatInteger(calls) },
              {
                label: "Model",
                value:
                  source === "fixture"
                    ? "deterministic fixture"
                    : estimate.data?.model_configured
                      ? estimate.data.model
                      : "not configured",
              },
              { label: "Horizon", value: formatSpan(horizon) },
            ]}
          />
          <div className="flex flex-col gap-2 p-3 text-sm text-fg-2">
            <p>
              Every interval the model sees only what this agent was delivered: delayed quotes, its
              own fills, position, and working orders. It never sees the fundamental value, other
              agents, or the shock schedule.
            </p>
            <p>
              It answers with a typed directive: quote with a spread, skew, size, and position cap;
              pause; or reduce toward flat. A native execution layer carries it out and the book
              decides every fill. Each decision is recorded, so replay never calls the model again.
            </p>
            <p className="text-xs text-fg-3">
              A failed call keeps the previous directive and is recorded as a fallback. Set
              MARKETTWIN_AI_MODEL and MARKETTWIN_AI_API_KEY on the API to use a real model.
            </p>
          </div>
        </InstrumentPanel>
      </div>
    </Page>
  );
}
