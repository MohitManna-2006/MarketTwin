import { useMutation, useQuery, useQueryClient } from "@tanstack/react-query";
import { ChevronDown, ChevronUp, Copy, Plus, Trash2 } from "lucide-react";
import { DropdownMenu } from "radix-ui";
import { useMemo, useState } from "react";
import { useNavigate, useSearchParams } from "react-router";

import { ContextBar } from "@/components/shell/ContextBar";
import { Page } from "@/components/shell/TerminalShell";
import { DataTable, useArrayRows, type DataColumn } from "@/components/tables/DataTable";
import { Button } from "@/components/ui/Button";
import { Digest } from "@/components/ui/Digest";
import { DurationField } from "@/components/ui/DurationField";
import { Checkbox, IntegerField, SelectField, TextField } from "@/components/ui/Field";
import { InstrumentPanel } from "@/components/ui/InstrumentPanel";
import { EmptyState, ErrorState, LoadingState } from "@/components/ui/States";
import { StatusIndicator, type StatusSpec } from "@/components/ui/StatusIndicator";
import { useDebounced } from "@/hooks/useDebounced";
import { ApiError, post } from "@/lib/api/client";
import { fieldCatalogueQuery, scenarioQuery, scenariosQuery } from "@/lib/api/queries";
import type {
  AgentConfig,
  ConfigError,
  FieldCatalogue,
  RunConfig,
  RunSummary,
  RunValidation,
  ShockConfig,
} from "@/lib/api/types";
import {
  addAgent,
  defaultShock,
  isValidName,
  removeAgent,
  setAccounting,
  shockAgentChoice,
  setIn,
  type Path,
} from "@/lib/config/draft";
import { cx } from "@/lib/cx";
import { formatDuration, formatInteger, formatSpan } from "@/lib/formatting";

import { POLICY_LABEL, policySummary, wakeupSummary } from "./policySummary";
import { FormSection, SchemaFields } from "./SchemaFields";

const VALID: StatusSpec = { label: "Config valid", tone: "positive", glyph: "dot" };
const CHECKING: StatusSpec = { label: "Validating", tone: "muted", glyph: "ring", pulse: true };
const INVALID: StatusSpec = { label: "Config invalid", tone: "negative", glyph: "cross" };

const SOURCE_LABEL: Record<ConfigError["source"], string> = {
  schema: "Schema",
  api: "Service limit",
  engine: "Engine",
};

export function SimulationLabPage() {
  const navigate = useNavigate();
  const client = useQueryClient();
  const [params, setParams] = useSearchParams();
  const scenarios = useQuery(scenariosQuery());
  const catalogue = useQuery(fieldCatalogueQuery());
  const scenarioKey = params.get("scenario") || scenarios.data?.[0]?.key || "baseline";
  const scenario = useQuery(scenarioQuery(scenarioKey));

  const [draft, setDraft] = useState<RunConfig | null>(null);
  const [loadedKey, setLoadedKey] = useState<string | null>(null);
  const [selectedAgent, setSelectedAgent] = useState<number | null>(null);
  // Open, closed, or (null) following the validation result.
  const [drawerChoice, setDrawerChoice] = useState<boolean | null>(null);
  const [jsonText, setJsonText] = useState<string | null>(null);
  const [jsonError, setJsonError] = useState<string | null>(null);

  // A scenario is a starting point: choosing one replaces the draft with its shipped config.
  if (scenario.data && loadedKey !== scenario.data.key) {
    const config = scenario.data.config as unknown as RunConfig;
    setDraft(config);
    setLoadedKey(scenario.data.key);
    setSelectedAgent(config.scenario?.agents[0]?.id ?? null);
    setJsonText(null);
    setJsonError(null);
  }

  const settled = useDebounced(draft, 250);
  const validation = useQuery({
    queryKey: ["validate", "run", settled],
    queryFn: () => post<RunValidation>("/configs/validate", { config: settled }),
    enabled: settled !== null,
    staleTime: Number.POSITIVE_INFINITY,
    placeholderData: (previous) => previous,
  });
  const checking = validation.isFetching || settled !== draft;
  const result = validation.data;
  const errors = useMemo(() => result?.errors ?? [], [result]);
  const errorMap = useMemo(() => {
    const map = new Map<string, string>();
    for (const error of errors) if (error.path) map.set(error.path, error.message);
    return map;
  }, [errors]);
  const drawer = drawerChoice ?? errors.length > 0;

  const launch = useMutation({
    mutationFn: (config: RunConfig) =>
      post<RunSummary>("/runs", { config, scenario_key: scenarioKey }),
    onSuccess: (run) => {
      void client.invalidateQueries({ queryKey: ["runs"] });
      void client.invalidateQueries({ queryKey: ["health"] });
      void navigate(`/runs/${run.id}`);
    },
  });

  function edit(path: Path, value: unknown) {
    setDraft((current) => (current ? setIn(current, path, value) : current));
  }

  const context = (
    <ContextBar
      crumbs={[
        { label: "Simulations", to: "/simulations" },
        { label: "New simulation" },
        ...(draft
          ? [{ label: draft.run_id, verbatim: true }, { label: `Seed ${draft.seed}` }]
          : []),
      ]}
      status={
        draft ? (
          <StatusIndicator spec={checking ? CHECKING : result?.valid ? VALID : INVALID} />
        ) : undefined
      }
    />
  );

  if (scenarios.isError || catalogue.isError || scenario.isError) {
    const error = scenarios.error ?? catalogue.error ?? scenario.error;
    return (
      <Page context={context}>
        <ErrorState
          error={error}
          onRetry={() => {
            void scenarios.refetch();
            void catalogue.refetch();
            void scenario.refetch();
          }}
        />
      </Page>
    );
  }
  if (!draft || !catalogue.data || !scenarios.data) {
    return (
      <Page context={context}>
        <LoadingState label="Loading scenario" />
      </Page>
    );
  }

  const fields = catalogue.data;
  const agents = draft.scenario?.agents ?? [];
  const agentIndex = agents.findIndex((agent) => agent.id === selectedAgent);
  const agent = agentIndex >= 0 ? agents[agentIndex] : undefined;
  const nameError = isValidName(draft.run_id)
    ? (errorMap.get("run_id") ?? null)
    : "1 to 128 letters, digits, _ or -";
  const canRun = !checking && result?.valid === true && !launch.isPending;

  return (
    <Page context={context}>
      <div className="flex flex-col lg:h-full">
        <div className="hairline-grid grid min-h-0 flex-1 grid-cols-1 lg:grid-cols-[400px_minmax(0,1fr)] 2xl:grid-cols-[440px_minmax(0,1fr)]">
          <InstrumentPanel
            index="01"
            title="Scenario"
            subtitle={scenario.data?.title}
            bodyClassName="overflow-auto"
          >
            <FormSection title="Template" note="replaces the draft with a shipped config">
              <div className="flex flex-col border border-line-subtle">
                {scenarios.data.map((item) => {
                  const active = item.key === scenarioKey;
                  return (
                    <button
                      key={item.key}
                      type="button"
                      aria-pressed={active}
                      onClick={() => setParams({ scenario: item.key })}
                      className={cx(
                        "transition-hover flex items-baseline gap-2 border-b border-line-subtle px-2 py-1 text-left last:border-b-0",
                        active
                          ? "bg-surface-selected shadow-[inset_2px_0_0_var(--accent-primary)]"
                          : "hover:bg-surface-hover",
                      )}
                    >
                      <span className={cx("text-sm", active ? "font-medium text-fg" : "text-fg-2")}>
                        {item.title}
                      </span>
                      <span className="flex-1" />
                      <span className="label text-fg-3">
                        {item.shocks.length > 0
                          ? item.shocks.join(" ").replace(/([a-z])([A-Z])/g, "$1 $2")
                          : item.kind === "scripted"
                            ? "scripted"
                            : "no shock"}
                      </span>
                    </button>
                  );
                })}
              </div>
              <p className="mt-2 text-xs leading-snug text-fg-3">{scenario.data?.description}</p>
            </FormSection>

            <FormSection title="Run">
              <div className="grid grid-cols-2 gap-x-3 gap-y-2">
                <TextField
                  label="Run name"
                  mono
                  value={draft.run_id}
                  maxLength={128}
                  error={nameError}
                  onChange={(value) => edit(["run_id"], value)}
                  className="col-span-2"
                />
                <IntegerField
                  label="Seed"
                  hint="Seed of every random stream. The same config and seed give the same run."
                  value={draft.seed}
                  min={0}
                  error={errorMap.get("seed")}
                  onChange={(value) => value !== null && edit(["seed"], value)}
                />
                <DurationField
                  label="Simulated horizon"
                  hint="Elapsed simulation time the run covers. Events exactly at the horizon execute."
                  value={draft.horizon_ns}
                  error={errorMap.get("horizon_ns")}
                  onChange={(value) => edit(["horizon_ns"], value)}
                />
                <IntegerField
                  label="Tick size"
                  unit="minor / tick"
                  hint="Minor currency units one tick is worth. Notional = price × quantity × tick size."
                  value={draft.tick_size_minor}
                  min={1}
                  error={errorMap.get("tick_size_minor")}
                  onChange={(value) => value !== null && edit(["tick_size_minor"], value)}
                />
              </div>
            </FormSection>

            {draft.scenario ? (
              <>
                <FormSection title="Market">
                  <SchemaFields
                    fields={fields.scenario}
                    value={draft.scenario as unknown as Record<string, unknown>}
                    path="scenario"
                    errors={errorMap}
                    onChange={(name, value) => edit(["scenario", name], value)}
                  />
                </FormSection>
                <FormSection
                  title="Fundamental value"
                  note="mean-reverting random walk; never a trade price"
                >
                  <SchemaFields
                    fields={fields.fundamental}
                    value={draft.scenario.fundamental}
                    path="scenario.fundamental"
                    errors={errorMap}
                    onChange={(name, value) => edit(["scenario", "fundamental", name], value)}
                  />
                </FormSection>
                <FormSection
                  title="Sampling"
                  note="depth snapshots and accounting samples"
                  actions={
                    <Checkbox
                      label="Record"
                      checked={draft.scenario.sampling !== null}
                      onChange={(checked) =>
                        edit(
                          ["scenario", "sampling"],
                          checked ? { interval_ns: 1_000_000_000, depth: 5 } : null,
                        )
                      }
                    />
                  }
                >
                  {draft.scenario.sampling ? (
                    <SchemaFields
                      fields={fields.sampling}
                      value={draft.scenario.sampling}
                      path="scenario.sampling"
                      errors={errorMap}
                      onChange={(name, value) => edit(["scenario", "sampling", name], value)}
                    />
                  ) : (
                    <p className="text-xs text-fg-3">
                      Without sampling the run records no depth snapshots or equity series: the
                      Market Microscope will have a trade tape and nothing to scrub.
                    </p>
                  )}
                </FormSection>
                <ShockEditor draft={draft} fields={fields} errors={errorMap} edit={edit} />
                <FormSection
                  title="Fees and cash"
                  note={draft.accounting ? "config schema 3" : "none: no fees, zero opening cash"}
                  actions={
                    <Checkbox
                      label="Enable"
                      checked={draft.accounting !== undefined}
                      onChange={(checked) => setDraft(setAccounting(draft, checked))}
                    />
                  }
                >
                  {draft.accounting ? (
                    <div className="flex flex-col gap-2">
                      <SchemaFields
                        fields={fields.fees}
                        value={draft.accounting.fees}
                        path="accounting.fees"
                        errors={errorMap}
                        onChange={(name, value) => edit(["accounting", "fees", name], value)}
                      />
                      <SchemaFields
                        fields={fields.accounting}
                        value={draft.accounting as unknown as Record<string, unknown>}
                        path="accounting"
                        errors={errorMap}
                        columns={1}
                        onChange={(name, value) => edit(["accounting", name], value)}
                      />
                      <p className="text-xs text-fg-3">
                        Per-agent opening cash and credit limits are set in the agent inspector.
                      </p>
                    </div>
                  ) : null}
                </FormSection>
              </>
            ) : (
              <FormSection title="Scripted run">
                <p className="text-sm leading-snug text-fg-3">
                  This config has no scenario: {formatInteger(draft.commands.length)} scripted
                  commands are run as written, against {draft.exchange.agents.length} registered
                  agents. There is no fundamental value, no sampling schedule, and no policy to
                  configure. The full config is in the JSON panel below.
                </p>
              </FormSection>
            )}

            <FormSection
              title="Resource limits"
              note="the run stops with a recorded reason at a cap"
            >
              <SchemaFields
                fields={fields.run}
                value={draft as unknown as Record<string, unknown>}
                path=""
                errors={new Map([...errorMap].map(([key, value]) => [`.${key}`, value]))}
                omit={["run_id", "seed", "horizon_ns", "tick_size_minor"]}
                columns={1}
                inline
                onChange={(name, value) => edit([name], value)}
              />
            </FormSection>
          </InstrumentPanel>

          <div className="hairline-grid grid min-h-0 grid-rows-[340px_auto] lg:grid-rows-[minmax(0,5fr)_minmax(0,6fr)]">
            <AgentPopulation
              draft={draft}
              fields={fields}
              errors={errorMap}
              selected={selectedAgent}
              onSelect={setSelectedAgent}
              onChange={setDraft}
            />
            <InstrumentPanel
              index="03"
              title="Agent parameters"
              subtitle={
                agent
                  ? `agent ${agent.id} · ${POLICY_LABEL[agent.policy.kind] ?? agent.policy.kind}`
                  : undefined
              }
              bodyClassName="overflow-auto"
            >
              {agent ? (
                <AgentInspectorForm
                  key={agent.id}
                  draft={draft}
                  agent={agent}
                  index={agentIndex}
                  fields={fields}
                  errors={errorMap}
                  edit={edit}
                />
              ) : (
                <EmptyState title="No agent selected">
                  {draft.scenario
                    ? "Select an agent in the population to edit its policy, latency, and limits."
                    : "A scripted run has no trading agents to configure."}
                </EmptyState>
              )}
            </InstrumentPanel>
          </div>
        </div>

        {drawer ? (
          <div className="hairline-grid grid h-[260px] shrink-0 grid-cols-1 border-t border-line lg:grid-cols-[400px_minmax(0,1fr)] 2xl:grid-cols-[440px_minmax(0,1fr)]">
            <InstrumentPanel
              index="04"
              title="Validation"
              subtitle={
                errors.length > 0 ? `${errors.length} to resolve` : "schema, service limits, engine"
              }
              bodyClassName="overflow-auto"
            >
              {validation.isError ? (
                <ErrorState
                  error={validation.error}
                  compact
                  onRetry={() => void validation.refetch()}
                />
              ) : errors.length > 0 ? (
                <ul>
                  {errors.map((error, index) => (
                    <li key={index} className="border-b border-line-subtle px-3 py-1.5">
                      <div className="flex items-baseline gap-2">
                        <span className="label text-neg">{SOURCE_LABEL[error.source]}</span>
                        {error.path ? (
                          <span className="num truncate text-xs text-fg-2">{error.path}</span>
                        ) : (
                          <span className="text-xs text-fg-3">
                            the engine does not say which field
                          </span>
                        )}
                      </div>
                      <p className="mt-0.5 text-sm text-fg">{error.message}</p>
                    </li>
                  ))}
                </ul>
              ) : result?.complexity ? (
                <Complexity result={result} />
              ) : (
                <LoadingState label="Validating" />
              )}
            </InstrumentPanel>
            <InstrumentPanel
              index="05"
              title="Config JSON"
              subtitle={
                jsonText === null
                  ? "exactly what will be submitted"
                  : "editing: apply to replace the draft"
              }
              actions={
                jsonText === null ? (
                  <Button
                    size="sm"
                    variant="ghost"
                    onClick={() => setJsonText(JSON.stringify(draft, null, 2))}
                  >
                    Edit JSON
                  </Button>
                ) : (
                  <>
                    <Button
                      size="sm"
                      variant="ghost"
                      onClick={() => {
                        setJsonText(null);
                        setJsonError(null);
                      }}
                    >
                      Discard
                    </Button>
                    <Button
                      size="sm"
                      variant="primary"
                      onClick={() => {
                        try {
                          const parsed = JSON.parse(jsonText) as RunConfig;
                          if (
                            typeof parsed !== "object" ||
                            parsed === null ||
                            !("exchange" in parsed)
                          ) {
                            throw new Error("not a run config object");
                          }
                          setDraft(parsed);
                          setSelectedAgent(parsed.scenario?.agents[0]?.id ?? null);
                          setJsonText(null);
                          setJsonError(null);
                        } catch (error) {
                          setJsonError(error instanceof Error ? error.message : String(error));
                        }
                      }}
                    >
                      Apply
                    </Button>
                  </>
                )
              }
              footer={
                jsonError ? (
                  <span role="alert" className="text-neg">
                    Not applied: {jsonError}
                  </span>
                ) : undefined
              }
            >
              {jsonText === null ? (
                <pre
                  tabIndex={0}
                  className="num h-full overflow-auto px-3 py-2 text-xs leading-relaxed whitespace-pre text-fg-2"
                >
                  {JSON.stringify(draft, null, 2)}
                </pre>
              ) : (
                <textarea
                  aria-label="Run config JSON"
                  spellCheck={false}
                  value={jsonText}
                  onChange={(event) => setJsonText(event.target.value)}
                  className="num h-full w-full resize-none bg-canvas px-3 py-2 text-xs leading-relaxed text-fg outline-none"
                />
              )}
            </InstrumentPanel>
          </div>
        ) : null}

        <div className="flex h-11 shrink-0 items-center gap-4 border-t border-line bg-surface-1 pr-3 pl-4 max-lg:sticky max-lg:bottom-0">
          <StatusIndicator spec={checking ? CHECKING : result?.valid ? VALID : INVALID} />
          {result?.valid && !checking ? (
            <span className="label hidden items-baseline gap-1.5 text-fg-3 md:flex">
              Config digest <Digest value={result.config_digest} label="config digest" />
            </span>
          ) : null}
          {!checking && errors.length > 0 ? (
            <span className="truncate text-sm text-neg">
              {errors[0]?.path ? `${errors[0].path}: ` : ""}
              {errors[0]?.message}
              {errors.length > 1 ? ` (+${errors.length - 1} more)` : ""}
            </span>
          ) : null}
          {launch.isError ? (
            <span role="alert" className="truncate text-sm text-neg">
              {launch.error instanceof ApiError
                ? `${launch.error.code}: ${launch.error.message}`
                : "The research API did not respond."}
            </span>
          ) : null}
          <span className="flex-1" />
          <Button variant="ghost" onClick={() => setDrawerChoice(!drawer)} aria-expanded={drawer}>
            Validation and JSON
            {drawer ? <ChevronDown size={12} aria-hidden /> : <ChevronUp size={12} aria-hidden />}
          </Button>
          <Button
            variant="solid"
            disabled={!canRun}
            onClick={() => launch.mutate(draft)}
            title={canRun ? "Submit this config to the engine" : "The config must validate first"}
          >
            {launch.isPending ? "Submitting" : "Run simulation"}
          </Button>
        </div>
      </div>
    </Page>
  );
}

function Complexity({ result }: { result: RunValidation }) {
  const complexity = result.complexity;
  if (!complexity) return null;
  const rows: [string, string, string][] = [
    ["Simulated horizon", formatSpan(complexity.horizon_ns), "elapsed simulation time"],
    ["Agents", formatInteger(complexity.agents), "with a trading policy"],
    [
      "Scheduled wakeups",
      formatInteger(complexity.scheduled_wakeups),
      complexity.wakeups_exact === false
        ? "expected: exponential schedules are random"
        : "exact: every schedule is fixed",
    ],
    ["Fundamental steps", formatInteger(complexity.fundamental_updates), "one trace record each"],
    [
      "Samples",
      formatInteger(complexity.samples),
      complexity.sampling_depth
        ? `${complexity.sampling_depth} price levels per side`
        : "sampling is off",
    ],
    ["Shocks", formatInteger(complexity.shocks), "scheduled transitions"],
    ["Event cap", formatInteger(complexity.max_events), "the run stops here if reached"],
  ];
  return (
    <div className="flex h-full flex-col">
      <table className="w-full text-sm">
        <caption className="sr-only">What the config schedules</caption>
        <tbody>
          {rows.map(([label, value, note]) => (
            <tr key={label} className="border-b border-line-subtle">
              <th scope="row" className="label py-1 pl-3 text-left font-semibold text-fg-3">
                {label}
              </th>
              <td className="num py-1 pr-3 text-right text-fg">{value}</td>
              <td className="py-1 pr-3 text-xs text-fg-3">{note}</td>
            </tr>
          ))}
        </tbody>
      </table>
      <p className="px-3 py-2 text-xs leading-snug text-fg-3">
        These counts follow from the config alone. Orders, trades, and processing time depend on
        what the agents decide and are not estimated.
      </p>
    </div>
  );
}

function ShockEditor({
  draft,
  fields,
  errors,
  edit,
}: {
  draft: RunConfig;
  fields: FieldCatalogue;
  errors: Map<string, string>;
  edit: (path: Path, value: unknown) => void;
}) {
  const scenario = draft.scenario;
  if (!scenario) return null;
  return (
    <FormSection
      title="Shocks"
      note={scenario.shocks.length === 0 ? "none scheduled" : `${scenario.shocks.length} scheduled`}
      actions={
        <DropdownMenu.Root>
          <DropdownMenu.Trigger asChild>
            <Button size="sm" variant="ghost">
              <Plus size={11} aria-hidden />
              Add shock
            </Button>
          </DropdownMenu.Trigger>
          <DropdownMenu.Portal>
            <DropdownMenu.Content
              align="end"
              sideOffset={4}
              className="z-50 w-[300px] rounded-md border border-line-strong bg-surface-1 py-1"
            >
              {fields.shocks.map((shock) => (
                <DropdownMenu.Item
                  key={shock.kind}
                  onSelect={() =>
                    edit(
                      ["scenario", "shocks"],
                      [...scenario.shocks, defaultShock(shock.kind as ShockConfig["kind"], draft)],
                    )
                  }
                  className="flex cursor-pointer flex-col gap-0.5 px-3 py-1.5 outline-none data-[highlighted]:bg-surface-hover"
                >
                  <span className="text-sm text-fg">
                    {shock.kind.replace(/([a-z])([A-Z])/g, "$1 $2")}
                  </span>
                  <span className="text-xs leading-snug text-fg-3">{shock.description}</span>
                </DropdownMenu.Item>
              ))}
            </DropdownMenu.Content>
          </DropdownMenu.Portal>
        </DropdownMenu.Root>
      }
    >
      {scenario.shocks.length === 0 ? (
        <p className="text-xs text-fg-3">
          The market runs with no exogenous shock. Shock intervals are half-open: [start, end).
        </p>
      ) : (
        <ul className="flex flex-col gap-2">
          {scenario.shocks.map((shock, index) => {
            const spec = fields.shocks.find((item) => item.kind === shock.kind);
            const choice = shockAgentChoice(shock.kind);
            const eligible = choice
              ? scenario.agents.filter((agent) => choice.eligible(agent.policy.kind))
              : [];
            return (
              <li key={index} className="border border-line-subtle">
                <div className="flex h-6 items-center gap-2 border-b border-line-subtle bg-surface-1 pr-1 pl-2">
                  <span className="num text-xs text-accent-label">S{index + 1}</span>
                  <span className="label text-warn">
                    {shock.kind.replace(/([a-z])([A-Z])/g, "$1 $2")}
                  </span>
                  <span className="flex-1" />
                  <button
                    type="button"
                    aria-label={`Remove shock ${index + 1}`}
                    onClick={() =>
                      edit(
                        ["scenario", "shocks"],
                        scenario.shocks.filter((_, position) => position !== index),
                      )
                    }
                    className="transition-hover inline-flex size-5 items-center justify-center rounded-sm text-fg-3 hover:text-neg"
                  >
                    <Trash2 size={11} aria-hidden />
                  </button>
                </div>
                <div className="flex flex-col gap-2 p-2">
                  <SchemaFields
                    fields={spec?.fields ?? []}
                    value={shock as unknown as Record<string, unknown>}
                    path={`scenario.shocks.${index}`}
                    errors={errors}
                    onChange={(name, value) => edit(["scenario", "shocks", index, name], value)}
                  />
                  {choice && shock.agents ? (
                    <fieldset>
                      <legend className="label mb-1 text-fg-3">{choice.legend}</legend>
                      <div className="flex flex-wrap gap-x-3 gap-y-1">
                        {eligible.map((agent) => (
                          <Checkbox
                            key={agent.id}
                            label={`Agent ${agent.id}`}
                            checked={shock.agents?.includes(agent.id) ?? false}
                            onChange={(checked) =>
                              edit(
                                ["scenario", "shocks", index, "agents"],
                                checked
                                  ? [...(shock.agents ?? []), agent.id].sort((a, b) => a - b)
                                  : (shock.agents ?? []).filter((id) => id !== agent.id),
                              )
                            }
                          />
                        ))}
                      </div>
                      {errors.get(`scenario.shocks.${index}.agents`) ? (
                        <p className="mt-0.5 text-2xs text-neg">
                          {errors.get(`scenario.shocks.${index}.agents`)}
                        </p>
                      ) : null}
                    </fieldset>
                  ) : null}
                </div>
              </li>
            );
          })}
        </ul>
      )}
    </FormSection>
  );
}

function AgentPopulation({
  draft,
  fields,
  errors,
  selected,
  onSelect,
  onChange,
}: {
  draft: RunConfig;
  fields: FieldCatalogue;
  errors: Map<string, string>;
  selected: number | null;
  onSelect: (id: number | null) => void;
  onChange: (config: RunConfig) => void;
}) {
  const agents = useMemo(() => draft.scenario?.agents ?? [], [draft.scenario]);
  const rows = useArrayRows(agents);
  const invalid = useMemo(() => {
    const set = new Set<number>();
    for (const path of errors.keys()) {
      const match = /^scenario\.agents\.(\d+)\./.exec(path);
      if (match) set.add(Number(match[1]));
    }
    return set;
  }, [errors]);

  const columns = useMemo<DataColumn<AgentConfig>[]>(
    () => [
      {
        id: "id",
        header: "ID",
        align: "right",
        width: "44px",
        cell: (agent) => <span className="num text-fg">{agent.id}</span>,
      },
      {
        id: "policy",
        header: "Policy",
        width: "minmax(150px, 0.9fr)",
        cell: (agent, index) => (
          <span className="flex items-baseline gap-1.5">
            <span className="text-fg">{POLICY_LABEL[agent.policy.kind] ?? agent.policy.kind}</span>
            {invalid.has(index) ? <span className="label text-neg">invalid</span> : null}
          </span>
        ),
      },
      {
        id: "parameters",
        header: "Trading parameters",
        width: "minmax(260px, 2fr)",
        cell: (agent) => <span className="num text-fg-2">{policySummary(agent.policy)}</span>,
      },
      {
        id: "wakeup",
        header: "Wakeup",
        width: "minmax(130px, 0.8fr)",
        cell: (agent) => <span className="num text-fg-2">{wakeupSummary(agent)}</span>,
      },
      {
        id: "latency",
        header: "Order entry",
        title: "Order-entry latency plus uniform jitter",
        align: "right",
        width: "128px",
        cell: (agent) => (
          <span className="num text-fg-2">
            {formatDuration(agent.latency.order_entry_ns)}
            <span className="text-fg-3">
              {" "}
              +{formatDuration(agent.latency.order_entry_jitter_ns)}
            </span>
          </span>
        ),
      },
      {
        id: "position",
        header: "Max position",
        unit: "shares",
        align: "right",
        width: "148px",
        cell: (agent) => (
          <span className="num text-fg-2">±{formatInteger(agent.limits.max_position)}</span>
        ),
      },
    ],
    [invalid],
  );

  const current = agents.find((agent) => agent.id === selected);
  return (
    <InstrumentPanel
      index="02"
      title="Agent population"
      subtitle={
        draft.scenario
          ? `${agents.length} agents · ${draft.exchange.agents.length} registered with the exchange`
          : undefined
      }
      actions={
        draft.scenario ? (
          <>
            <DropdownMenu.Root>
              <DropdownMenu.Trigger asChild>
                <Button size="sm" variant="ghost">
                  <Plus size={11} aria-hidden />
                  Add agent
                </Button>
              </DropdownMenu.Trigger>
              <DropdownMenu.Portal>
                <DropdownMenu.Content
                  align="end"
                  sideOffset={4}
                  className="z-50 w-[340px] rounded-md border border-line-strong bg-surface-1 py-1"
                >
                  {fields.policies
                    .filter((policy) => policy.template)
                    .map((policy) => (
                      <DropdownMenu.Item
                        key={policy.kind}
                        onSelect={() => {
                          const next = addAgent(draft, policy.template as unknown as AgentConfig);
                          onChange(next);
                          onSelect(Math.max(...next.exchange.agents));
                        }}
                        className="flex cursor-pointer flex-col gap-0.5 px-3 py-1.5 outline-none data-[highlighted]:bg-surface-hover"
                      >
                        <span className="text-sm text-fg">
                          {POLICY_LABEL[policy.kind] ?? policy.kind}
                        </span>
                        <span className="text-xs leading-snug text-fg-3">{policy.description}</span>
                      </DropdownMenu.Item>
                    ))}
                </DropdownMenu.Content>
              </DropdownMenu.Portal>
            </DropdownMenu.Root>
            <Button
              size="sm"
              variant="ghost"
              disabled={!current}
              onClick={() => {
                if (!current) return;
                const next = addAgent(draft, current);
                onChange(next);
                onSelect(Math.max(...next.exchange.agents));
              }}
            >
              <Copy size={11} aria-hidden />
              Duplicate
            </Button>
            <Button
              size="sm"
              variant="ghost"
              disabled={!current}
              onClick={() => {
                if (!current) return;
                const next = removeAgent(draft, current.id);
                onChange(next);
                onSelect(next.scenario?.agents[0]?.id ?? null);
              }}
            >
              <Trash2 size={11} aria-hidden />
              Remove
            </Button>
          </>
        ) : undefined
      }
    >
      <DataTable
        label="Agent population"
        columns={columns}
        {...rows}
        getRowId={(agent) => String(agent.id)}
        selectedId={selected === null ? null : String(selected)}
        onSelect={(agent) => onSelect(agent.id)}
        empty={
          <EmptyState title={draft.scenario ? "No agents" : "Scripted run"}>
            {draft.scenario
              ? "A market with no agents records a fundamental value and an empty book. Add an agent to begin."
              : "Orders come from the scripted commands in the config, not from trading policies."}
          </EmptyState>
        }
      />
    </InstrumentPanel>
  );
}

function AgentInspectorForm({
  draft,
  agent,
  index,
  fields,
  errors,
  edit,
}: {
  draft: RunConfig;
  agent: AgentConfig;
  index: number;
  fields: FieldCatalogue;
  errors: Map<string, string>;
  edit: (path: Path, value: unknown) => void;
}) {
  const base: Path = ["scenario", "agents", index];
  const path = `scenario.agents.${index}`;
  const policy = fields.policies.find((item) => item.kind === agent.policy.kind);
  const accounting = draft.accounting;
  const accountIndex =
    accounting?.accounts.findIndex((account) => account.agent_id === agent.id) ?? -1;
  const account = accountIndex >= 0 ? accounting?.accounts[accountIndex] : undefined;

  return (
    <div className="grid grid-cols-1 xl:grid-cols-2">
      <div className="xl:border-r xl:border-line-subtle">
        <FormSection title="Policy" note={policy?.description}>
          <div className="flex flex-col gap-2">
            <SelectField
              label="Policy type"
              value={agent.policy.kind}
              options={fields.policies.map((item) => ({
                value: item.kind,
                label: POLICY_LABEL[item.kind] ?? item.kind,
              }))}
              onChange={(kind) => {
                const template = fields.policies.find((item) => item.kind === kind)?.template as
                  AgentConfig | null | undefined;
                if (template) edit([...base, "policy"], structuredClone(template.policy));
              }}
            />
            <SchemaFields
              fields={policy?.fields ?? []}
              value={agent.policy}
              path={`${path}.policy`}
              errors={errors}
              onChange={(name, value) => edit([...base, "policy", name], value)}
            />
          </div>
        </FormSection>
        {accounting ? (
          <FormSection
            title="Account"
            note={account ? "exception to the default" : "uses the default opening cash"}
            actions={
              <Checkbox
                label="Own balance"
                checked={account !== undefined}
                onChange={(checked) =>
                  edit(
                    ["accounting", "accounts"],
                    checked
                      ? [
                          ...accounting.accounts,
                          {
                            agent_id: agent.id,
                            initial_cash_minor: accounting.default_initial_cash_minor,
                            credit_limit_minor: null,
                          },
                        ]
                      : accounting.accounts.filter((item) => item.agent_id !== agent.id),
                  )
                }
              />
            }
          >
            {account ? (
              <SchemaFields
                fields={fields.account}
                value={account}
                path={`accounting.accounts.${accountIndex}`}
                errors={errors}
                omit={["agent_id"]}
                onChange={(name, value) =>
                  edit(["accounting", "accounts", accountIndex, name], value)
                }
              />
            ) : null}
          </FormSection>
        ) : null}
      </div>
      <div>
        <FormSection title="Wakeup schedule">
          <SchemaFields
            fields={fields.wakeup}
            value={agent.wakeup}
            path={`${path}.wakeup`}
            errors={errors}
            columns={3}
            onChange={(name, value) => edit([...base, "wakeup", name], value)}
          />
        </FormSection>
        <FormSection title="Latency" note="each path is first-in first-out per agent">
          <SchemaFields
            fields={fields.latency}
            value={agent.latency}
            path={`${path}.latency`}
            errors={errors}
            onChange={(name, value) => edit([...base, "latency", name], value)}
          />
        </FormSection>
        <FormSection title="Limits" note="intents beyond a limit are dropped and counted">
          <SchemaFields
            fields={fields.limits}
            value={agent.limits}
            path={`${path}.limits`}
            errors={errors}
            onChange={(name, value) => edit([...base, "limits", name], value)}
          />
        </FormSection>
      </div>
    </div>
  );
}
