import { useMutation, useQuery, useQueryClient } from "@tanstack/react-query";
import { ChevronDown, ChevronUp, Plus, Trash2 } from "lucide-react";
import { useMemo, useState } from "react";
import { useNavigate, useSearchParams } from "react-router";

import { ContextBar } from "@/components/shell/ContextBar";
import { Page } from "@/components/shell/TerminalShell";
import { Button } from "@/components/ui/Button";
import { DurationField } from "@/components/ui/DurationField";
import { Checkbox, IntegerField, SelectField, TextField } from "@/components/ui/Field";
import { InstrumentPanel } from "@/components/ui/InstrumentPanel";
import { SegmentedControl } from "@/components/ui/SegmentedControl";
import { ErrorState, LoadingState } from "@/components/ui/States";
import { StatusIndicator, type StatusSpec } from "@/components/ui/StatusIndicator";
import { POLICY_LABEL } from "@/features/simulations/policySummary";
import { FormSection, SchemaFields } from "@/features/simulations/SchemaFields";
import { useDebounced } from "@/hooks/useDebounced";
import { ApiError, post } from "@/lib/api/client";
import { fieldCatalogueQuery, scenarioQuery, scenariosQuery } from "@/lib/api/queries";
import type {
  AgentConfig,
  ExperimentConfig,
  ExperimentSummary,
  ExperimentValidation,
  FieldCatalogue,
  PolicyConfig,
  RunConfig,
} from "@/lib/api/types";
import { isValidName, setIn, type Path } from "@/lib/config/draft";
import { formatInteger, formatSpan } from "@/lib/formatting";

const VALID: StatusSpec = { label: "Design valid", tone: "positive", glyph: "dot" };
const CHECKING: StatusSpec = { label: "Validating", tone: "muted", glyph: "ring", pulse: true };
const INVALID: StatusSpec = { label: "Design invalid", tone: "negative", glyph: "cross" };

const QUOTING = ["half_spread_ticks", "quote_size", "levels", "level_spacing_ticks"] as const;

/**
 * The shipped maker comparison, built for whichever scenario is chosen: one participant
 * is replaced, and both variants give it the same quoting parameters.
 */
function defaultExperiment(
  base: RunConfig,
  key: string,
  catalogue: FieldCatalogue,
): ExperimentConfig {
  const agents = base.scenario?.agents ?? [];
  const subject =
    agents.find((agent) => agent.policy.kind === "InventoryMarketMaker") ??
    agents.find((agent) => agent.policy.kind === "MarketMaker") ??
    agents[0];
  const template = (kind: string) =>
    (
      catalogue.policies.find((policy) => policy.kind === kind)?.template as
        AgentConfig | null | undefined
    )?.policy;
  const quoting: Record<string, number | string> = {};
  if (subject)
    for (const name of QUOTING)
      if (name in subject.policy) quoting[name] = subject.policy[name] as number;
  const plain: PolicyConfig = {
    ...(template("MarketMaker") ?? { kind: "MarketMaker" }),
    ...quoting,
    kind: "MarketMaker",
  };
  const inventory: PolicyConfig =
    subject?.policy.kind === "InventoryMarketMaker"
      ? subject.policy
      : {
          ...(template("InventoryMarketMaker") ?? { kind: "InventoryMarketMaker" }),
          ...quoting,
          kind: "InventoryMarketMaker",
        };
  const id = subject?.id ?? 1;
  return {
    schema_version: 1,
    experiment_id: `${key.replace(/[^A-Za-z0-9_-]/g, "_")}_makers`.slice(0, 48),
    base_config: base,
    horizon_ns: null,
    seeds: { first: 1, count: 20 },
    variants: [
      { name: "baseline_maker", policy_overrides: [{ agent_id: id, policy: plain }] },
      { name: "inventory_maker", policy_overrides: [{ agent_id: id, policy: inventory }] },
    ],
    comparison: { baseline: "baseline_maker", candidate: "inventory_maker", subject_agent_id: id },
    bootstrap: { resamples: 10_000, seed: 20261010 },
    artifacts: "None",
    limits: { max_runs: 40, max_output_bytes: 0 },
  };
}

function seedCount(seeds: ExperimentConfig["seeds"]): number {
  return Array.isArray(seeds) ? seeds.length : seeds.count;
}

/** Keeps the run bound equal to seeds times variants, which is what the design needs. */
function withRunLimit(config: ExperimentConfig): ExperimentConfig {
  const total = Math.max(1, seedCount(config.seeds) * config.variants.length);
  return config.limits.max_runs === total
    ? config
    : { ...config, limits: { ...config.limits, max_runs: total } };
}

export function ExperimentNewPage() {
  const navigate = useNavigate();
  const client = useQueryClient();
  const [params, setParams] = useSearchParams();
  const scenarios = useQuery(scenariosQuery());
  const catalogue = useQuery(fieldCatalogueQuery());
  const usable = useMemo(
    () => (scenarios.data ?? []).filter((item) => item.kind === "scenario"),
    [scenarios.data],
  );
  const scenarioKey = params.get("scenario") || usable[0]?.key || "baseline";
  const scenario = useQuery(scenarioQuery(scenarioKey));

  const [draft, setDraft] = useState<ExperimentConfig | null>(null);
  const [loadedKey, setLoadedKey] = useState<string | null>(null);
  const [seedText, setSeedText] = useState("1, 2, 3, 4");
  const [drawerChoice, setDrawerChoice] = useState<boolean | null>(null);

  // Choosing a scenario starts a new design from it.
  if (scenario.data && catalogue.data && loadedKey !== scenario.data.key) {
    setDraft(
      defaultExperiment(
        scenario.data.config as unknown as RunConfig,
        scenario.data.key,
        catalogue.data,
      ),
    );
    setLoadedKey(scenario.data.key);
  }

  const settled = useDebounced(draft, 300);
  const validation = useQuery({
    queryKey: ["validate", "experiment", settled],
    queryFn: () => post<ExperimentValidation>("/experiments/validate", { config: settled }),
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
    mutationFn: (config: ExperimentConfig) =>
      post<ExperimentSummary>("/experiments", { config, scenario_key: scenarioKey }),
    onSuccess: (experiment) => {
      void client.invalidateQueries({ queryKey: ["experiments"] });
      void client.invalidateQueries({ queryKey: ["health"] });
      void navigate(`/experiments/${experiment.id}`);
    },
  });

  function edit(path: Path, value: unknown) {
    setDraft((current) => (current ? withRunLimit(setIn(current, path, value)) : current));
  }

  const context = (
    <ContextBar
      crumbs={[
        { label: "Experiments", to: "/experiments" },
        { label: "New experiment" },
        ...(draft ? [{ label: draft.experiment_id, verbatim: true }] : []),
      ]}
      status={
        draft ? (
          <StatusIndicator spec={checking ? CHECKING : result?.valid ? VALID : INVALID} />
        ) : undefined
      }
    />
  );

  const failure = scenarios.error ?? catalogue.error ?? scenario.error;
  if (failure) {
    return (
      <Page context={context}>
        <ErrorState error={failure} onRetry={() => void scenario.refetch()} />
      </Page>
    );
  }
  if (!draft || !catalogue.data) {
    return (
      <Page context={context}>
        <LoadingState label="Loading scenario" />
      </Page>
    );
  }

  const fields = catalogue.data;
  const agents = draft.base_config.scenario?.agents ?? [];
  const variantNames = draft.variants.map((variant) => variant.name);
  const total = seedCount(draft.seeds) * draft.variants.length;
  const range = !Array.isArray(draft.seeds);
  const design = result?.design;
  const canRun = !checking && result?.valid === true && !launch.isPending;

  return (
    <Page context={context}>
      <div className="flex flex-col lg:h-full">
        <div className="hairline-grid grid min-h-0 flex-1 grid-cols-1 lg:grid-cols-[380px_minmax(0,1fr)_340px]">
          <InstrumentPanel index="01" title="Design" bodyClassName="overflow-auto">
            <FormSection title="Experiment">
              <div className="flex flex-col gap-2">
                <TextField
                  label="Experiment name"
                  mono
                  maxLength={48}
                  value={draft.experiment_id}
                  error={
                    isValidName(draft.experiment_id, 48)
                      ? (errorMap.get("experiment_id") ?? null)
                      : "1 to 48 letters, digits, _ or -"
                  }
                  onChange={(value) => edit(["experiment_id"], value)}
                />
                <SelectField
                  label="Scenario"
                  hint="The base config every run starts from. Choosing another resets the design."
                  value={scenarioKey}
                  options={usable.map((item) => ({ value: item.key, label: item.title }))}
                  onChange={(key) => setParams({ scenario: key })}
                />
                <p className="text-xs leading-snug text-fg-3">{scenario.data?.description}</p>
              </div>
            </FormSection>
            <FormSection
              title="Horizon"
              note={
                draft.horizon_ns === null
                  ? `base: ${formatSpan(draft.base_config.horizon_ns)}`
                  : "overrides the base"
              }
              actions={
                <Checkbox
                  label="Override"
                  checked={draft.horizon_ns !== null}
                  onChange={(checked) =>
                    edit(["horizon_ns"], checked ? draft.base_config.horizon_ns : null)
                  }
                />
              }
            >
              {draft.horizon_ns !== null ? (
                <DurationField
                  label="Simulated horizon per run"
                  value={draft.horizon_ns}
                  error={errorMap.get("horizon_ns")}
                  onChange={(value) => edit(["horizon_ns"], value)}
                />
              ) : null}
            </FormSection>
            <FormSection
              title="Seeds"
              note={`${formatInteger(seedCount(draft.seeds))} seeds`}
              actions={
                <SegmentedControl
                  label="Seed entry"
                  value={range ? "range" : "list"}
                  onChange={(mode) => {
                    if (mode === "range") edit(["seeds"], { first: 1, count: 20 });
                    else {
                      const list = seedText
                        .split(/[\s,]+/)
                        .filter((item) => /^\d+$/.test(item))
                        .map(Number);
                      edit(["seeds"], list.length > 0 ? list : [1]);
                    }
                  }}
                  options={[
                    { value: "range", label: "Range" },
                    { value: "list", label: "List" },
                  ]}
                />
              }
            >
              {!Array.isArray(draft.seeds) ? (
                <div className="grid grid-cols-2 gap-x-3">
                  <IntegerField
                    label="First seed"
                    value={draft.seeds.first}
                    min={0}
                    error={errorMap.get("seeds.first")}
                    onChange={(value) => value !== null && edit(["seeds", "first"], value)}
                  />
                  <IntegerField
                    label="Count"
                    hint="Consecutive seeds from the first"
                    value={draft.seeds.count}
                    min={1}
                    error={errorMap.get("seeds.count") ?? errorMap.get("seeds")}
                    onChange={(value) => value !== null && edit(["seeds", "count"], value)}
                  />
                </div>
              ) : (
                <TextField
                  label="Seed list"
                  mono
                  value={seedText}
                  placeholder="1, 2, 3, 4"
                  error={errorMap.get("seeds") ?? null}
                  onChange={(text) => {
                    setSeedText(text);
                    const list = text
                      .split(/[\s,]+/)
                      .filter((item) => /^\d+$/.test(item))
                      .map(Number);
                    edit(["seeds"], list);
                  }}
                />
              )}
              <p className="mt-2 text-xs leading-snug text-fg-3">
                Every variant runs every seed:{" "}
                <span className="num text-fg-2">
                  {formatInteger(seedCount(draft.seeds))} × {draft.variants.length} ={" "}
                  {formatInteger(total)} runs
                </span>
                , one after another.
              </p>
            </FormSection>
            <FormSection title="Bootstrap" note="percentile interval of the mean difference">
              <SchemaFields
                fields={[
                  {
                    name: "resamples",
                    unit: "resamples",
                    description: "0 turns the bootstrap interval off; otherwise 100 to 1,000,000",
                    minimum: 0,
                    maximum: 1_000_000,
                    options: null,
                    nullable: false,
                  },
                  {
                    name: "seed",
                    unit: "",
                    description: "Seed of the bootstrap stream, which no run draws from",
                    minimum: 0,
                    maximum: null,
                    options: null,
                    nullable: false,
                  },
                ]}
                value={draft.bootstrap}
                path="bootstrap"
                errors={errorMap}
                onChange={(name, value) => edit(["bootstrap", name], value)}
              />
            </FormSection>
            <FormSection
              title="Run artifacts"
              note={draft.artifacts === "None" ? "digests only" : "every run directory"}
            >
              <div className="flex flex-col gap-2">
                <SelectField
                  label="Keep"
                  value={draft.artifacts}
                  options={[
                    { value: "None", label: "None: record each run's digests" },
                    { value: "All", label: "All: write every run directory" },
                  ]}
                  onChange={(value) => {
                    const next = setIn(draft, ["artifacts"], value);
                    setDraft(
                      withRunLimit(
                        setIn(
                          next,
                          ["limits", "max_output_bytes"],
                          value === "All" ? 4 * 1024 * 1024 * 1024 : 0,
                        ),
                      ),
                    );
                  }}
                />
                {draft.artifacts === "All" ? (
                  <IntegerField
                    label="Output budget"
                    unit="bytes"
                    hint="Once spent, remaining cells fail with OutputLimit instead of filling the disk"
                    value={draft.limits.max_output_bytes}
                    min={1}
                    error={errorMap.get("limits.max_output_bytes")}
                    onChange={(value) =>
                      value !== null && edit(["limits", "max_output_bytes"], value)
                    }
                  />
                ) : (
                  <p className="text-xs leading-snug text-fg-3">
                    A one-hour run writes about 55 MB. With digests only, any seed can still be
                    regenerated and inspected later; the terminal checks it against these digests.
                  </p>
                )}
              </div>
            </FormSection>
          </InstrumentPanel>

          <InstrumentPanel
            index="02"
            title="Strategy variants"
            subtitle="each replaces named agents' policies and nothing else"
            bodyClassName="overflow-auto"
            actions={
              <Button
                size="sm"
                variant="ghost"
                onClick={() =>
                  edit(
                    ["variants"],
                    [
                      ...draft.variants,
                      { name: `variant_${draft.variants.length + 1}`, policy_overrides: [] },
                    ],
                  )
                }
              >
                <Plus size={11} aria-hidden />
                Add variant
              </Button>
            }
          >
            <div className="grid grid-cols-1 2xl:grid-cols-2">
              {draft.variants.map((variant, index) => (
                <section key={index} className="border-r border-b border-line-subtle">
                  <header className="flex h-7 items-center gap-2 border-b border-line-subtle bg-surface-1 pr-1 pl-3">
                    <span className="num text-xs text-accent-label">V{index + 1}</span>
                    <span className="label text-fg-2">
                      {variant.name === draft.comparison?.baseline
                        ? "Baseline"
                        : variant.name === draft.comparison?.candidate
                          ? "Candidate"
                          : "Variant"}
                    </span>
                    <span className="flex-1" />
                    <button
                      type="button"
                      aria-label={`Remove variant ${variant.name}`}
                      disabled={draft.variants.length <= 1}
                      onClick={() =>
                        edit(
                          ["variants"],
                          draft.variants.filter((_, position) => position !== index),
                        )
                      }
                      className="transition-hover inline-flex size-5 items-center justify-center rounded-sm text-fg-3 hover:text-neg disabled:text-fg-off"
                    >
                      <Trash2 size={11} aria-hidden />
                    </button>
                  </header>
                  <div className="flex flex-col gap-2 p-3">
                    <TextField
                      label="Variant name"
                      mono
                      maxLength={48}
                      value={variant.name}
                      error={
                        isValidName(variant.name, 48)
                          ? (errorMap.get(`variants.${index}.name`) ?? null)
                          : "1 to 48 letters, digits, _ or -"
                      }
                      onChange={(name) => {
                        let next = setIn(draft, ["variants", index, "name"], name);
                        if (draft.comparison?.baseline === variant.name)
                          next = setIn(next, ["comparison", "baseline"], name);
                        if (draft.comparison?.candidate === variant.name)
                          next = setIn(next, ["comparison", "candidate"], name);
                        setDraft(withRunLimit(next));
                      }}
                    />
                    {variant.policy_overrides.length === 0 ? (
                      <p className="text-xs text-fg-3">
                        No override: this variant is the base scenario unchanged.
                      </p>
                    ) : null}
                    {variant.policy_overrides.map((override, position) => {
                      const base: Path = ["variants", index, "policy_overrides", position];
                      const spec = fields.policies.find(
                        (policy) => policy.kind === override.policy.kind,
                      );
                      return (
                        <div
                          key={position}
                          className="flex flex-col gap-2 border border-line-subtle p-2"
                        >
                          <div className="grid grid-cols-[1fr_1.4fr_auto] items-end gap-2">
                            <SelectField
                              label="Agent"
                              value={String(override.agent_id)}
                              options={agents.map((agent) => ({
                                value: String(agent.id),
                                label: `${agent.id} · ${POLICY_LABEL[agent.policy.kind] ?? agent.policy.kind}`,
                              }))}
                              onChange={(value) => edit([...base, "agent_id"], Number(value))}
                            />
                            <SelectField
                              label="Runs policy"
                              value={override.policy.kind}
                              options={fields.policies.map((policy) => ({
                                value: policy.kind,
                                label: POLICY_LABEL[policy.kind] ?? policy.kind,
                              }))}
                              onChange={(kind) => {
                                const template = fields.policies.find(
                                  (policy) => policy.kind === kind,
                                )?.template as AgentConfig | null | undefined;
                                if (!template) return;
                                // Carry over the parameters the two policies share.
                                const shared = Object.fromEntries(
                                  Object.entries(override.policy).filter(
                                    ([name]) => name !== "kind" && name in template.policy,
                                  ),
                                );
                                edit([...base, "policy"], { ...template.policy, ...shared });
                              }}
                            />
                            <button
                              type="button"
                              aria-label="Remove override"
                              onClick={() =>
                                edit(
                                  ["variants", index, "policy_overrides"],
                                  variant.policy_overrides.filter((_, item) => item !== position),
                                )
                              }
                              className="transition-hover mb-0.5 inline-flex size-5 items-center justify-center rounded-sm text-fg-3 hover:text-neg"
                            >
                              <Trash2 size={11} aria-hidden />
                            </button>
                          </div>
                          <SchemaFields
                            fields={spec?.fields ?? []}
                            value={override.policy}
                            path={`variants.${index}.policy_overrides.${position}.policy`}
                            errors={errorMap}
                            onChange={(name, value) => edit([...base, "policy", name], value)}
                          />
                        </div>
                      );
                    })}
                    <div>
                      <Button
                        size="sm"
                        variant="ghost"
                        disabled={agents.length === 0}
                        onClick={() => {
                          const agent =
                            agents.find((item) => item.id === draft.comparison?.subject_agent_id) ??
                            agents[0];
                          if (!agent) return;
                          edit(
                            ["variants", index, "policy_overrides"],
                            [
                              ...variant.policy_overrides,
                              { agent_id: agent.id, policy: structuredClone(agent.policy) },
                            ],
                          );
                        }}
                      >
                        <Plus size={11} aria-hidden />
                        Add policy override
                      </Button>
                    </div>
                  </div>
                </section>
              ))}
            </div>
          </InstrumentPanel>

          <InstrumentPanel index="03" title="Comparison" bodyClassName="overflow-auto">
            <FormSection
              title="Paired comparison"
              note={draft.comparison ? "candidate minus baseline" : "off"}
              actions={
                <Checkbox
                  label="Compare"
                  checked={draft.comparison !== null}
                  onChange={(checked) =>
                    edit(
                      ["comparison"],
                      checked
                        ? {
                            baseline: variantNames[0] ?? "",
                            candidate: variantNames[1] ?? variantNames[0] ?? "",
                            subject_agent_id: agents[0]?.id ?? 1,
                          }
                        : null,
                    )
                  }
                />
              }
            >
              {draft.comparison ? (
                <div className="flex flex-col gap-2">
                  <SelectField
                    label="Baseline"
                    value={draft.comparison.baseline}
                    options={variantNames.map((name) => ({ value: name, label: name }))}
                    onChange={(value) => edit(["comparison", "baseline"], value)}
                  />
                  <SelectField
                    label="Candidate"
                    value={draft.comparison.candidate}
                    options={variantNames.map((name) => ({ value: name, label: name }))}
                    onChange={(value) => edit(["comparison", "candidate"], value)}
                  />
                  <SelectField
                    label="Subject agent"
                    hint="The agent whose P&L, drawdown, and inventory are compared"
                    value={String(draft.comparison.subject_agent_id)}
                    options={draft.base_config.exchange.agents.map((id) => ({
                      value: String(id),
                      label: `Agent ${id}`,
                    }))}
                    onChange={(value) => edit(["comparison", "subject_agent_id"], Number(value))}
                  />
                  <p className="text-xs leading-snug text-fg-3">
                    Nineteen metrics, fixed in the engine before any run, are compared seed by seed.
                    Each difference comes with a 95% Student t interval and a 95% percentile
                    bootstrap interval. No multiplicity correction is applied.
                  </p>
                </div>
              ) : (
                <p className="text-xs leading-snug text-fg-3">
                  Without a comparison the experiment still runs every cell and records its digests,
                  but reports no paired statistics.
                </p>
              )}
            </FormSection>
            <FormSection title="What will run">
              {design ? (
                <table className="w-full text-sm">
                  <caption className="sr-only">Design counts</caption>
                  <tbody>
                    {(
                      [
                        ["Seeds", formatInteger(design.seeds)],
                        ["Variants", formatInteger(design.variants)],
                        ["Runs", formatInteger(design.total_runs)],
                        ["Simulated per run", formatSpan(design.per_run.horizon_ns)],
                        ["Samples per run", formatInteger(design.per_run.samples)],
                        ["Agents per run", formatInteger(design.per_run.agents)],
                      ] as const
                    ).map(([label, value]) => (
                      <tr key={label} className="border-b border-line-subtle">
                        <th scope="row" className="label py-1 text-left font-semibold text-fg-3">
                          {label}
                        </th>
                        <td className="num py-1 text-right text-fg">{value}</td>
                      </tr>
                    ))}
                  </tbody>
                </table>
              ) : (
                <p className="text-xs text-fg-3">Shown once the design validates.</p>
              )}
              <p className="mt-2 text-xs leading-snug text-fg-3">
                Processing time is not estimated. As a reference, the shipped 200-seed comparison
                (400 one-hour runs) takes about two and a half minutes on one core.
              </p>
            </FormSection>
          </InstrumentPanel>
        </div>

        {drawer ? (
          <div className="hairline-grid grid h-[240px] shrink-0 grid-cols-1 border-t border-line lg:grid-cols-[380px_minmax(0,1fr)]">
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
                        <span className="label text-neg">
                          {error.source === "engine"
                            ? "Engine"
                            : error.source === "api"
                              ? "Service limit"
                              : "Schema"}
                        </span>
                        <span className="num truncate text-xs text-fg-2">
                          {error.path || "the engine does not say which field"}
                        </span>
                      </div>
                      <p className="mt-0.5 text-sm text-fg">{error.message}</p>
                    </li>
                  ))}
                </ul>
              ) : (
                <p className="p-3 text-sm text-fg-3">
                  The design passes the schema, this service&apos;s limits, and the engine&apos;s
                  own validation, which checks one cell of every variant.
                </p>
              )}
            </InstrumentPanel>
            <InstrumentPanel
              index="05"
              title="Experiment JSON"
              subtitle="exactly what will be submitted, base config embedded"
            >
              <pre
                tabIndex={0}
                className="num h-full overflow-auto px-3 py-2 text-xs leading-relaxed whitespace-pre text-fg-2"
              >
                {JSON.stringify(draft, null, 2)}
              </pre>
            </InstrumentPanel>
          </div>
        ) : null}

        <div className="flex h-11 shrink-0 items-center gap-4 border-t border-line bg-surface-1 pr-3 pl-4 max-lg:sticky max-lg:bottom-0">
          <StatusIndicator spec={checking ? CHECKING : result?.valid ? VALID : INVALID} />
          <span className="num text-sm text-fg-2">
            {formatInteger(seedCount(draft.seeds))} seeds × {draft.variants.length} variants ={" "}
            <span className="text-fg">{formatInteger(total)} runs</span>
          </span>
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
          <Button variant="solid" disabled={!canRun} onClick={() => launch.mutate(draft)}>
            {launch.isPending ? "Submitting" : "Run experiment"}
          </Button>
        </div>
      </div>
    </Page>
  );
}
