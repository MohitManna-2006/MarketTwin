import type { ReactNode } from "react";

import { Digest } from "@/components/ui/Digest";
import { policySummary, POLICY_LABEL } from "@/features/simulations/policySummary";
import type {
  ExperimentConfig,
  ExperimentDetail,
  ExperimentResultsDocument,
} from "@/lib/api/types";
import { formatInteger, formatSpan } from "@/lib/formatting";

function Block({ title, children }: { title: string; children: ReactNode }) {
  return (
    <section className="border-b border-line-subtle px-3 py-2 last:border-b-0">
      <h3 className="label mb-1 text-fg-2">{title}</h3>
      <div className="flex flex-col gap-1 text-sm leading-snug text-fg-2">{children}</div>
    </section>
  );
}

function Fact({ label, children }: { label: string; children: ReactNode }) {
  return (
    <div className="flex gap-3">
      <span className="label w-[128px] shrink-0 pt-0.5 text-fg-3">{label}</span>
      <span className="min-w-0 flex-1">{children}</span>
    </div>
  );
}

/**
 * How the experiment was designed and how its numbers were computed, from the canonical
 * experiment config and the results document. Results that are negative or inconclusive
 * are reported the same way as any other.
 */
export function Methodology({
  experiment,
  results,
}: {
  experiment: ExperimentDetail;
  results: ExperimentResultsDocument | null;
}) {
  const config = experiment.config as unknown as ExperimentConfig;
  const base = config.base_config;
  const seeds = Array.isArray(config.seeds) ? config.seeds : [];
  const horizon = config.horizon_ns ?? base.horizon_ns;
  const comparison = config.comparison;
  const statistics = results?.results.statistics;
  const failed = results?.results.runs.filter((run) => run.status === "Failed") ?? [];
  const firstSeed = seeds[0];
  const lastSeed = seeds[seeds.length - 1];
  const consecutive =
    seeds.length > 1 &&
    firstSeed !== undefined &&
    lastSeed !== undefined &&
    lastSeed - firstSeed === seeds.length - 1;

  return (
    <div className="grid grid-cols-1 2xl:grid-cols-2">
      <div className="2xl:border-r 2xl:border-line-subtle">
        <Block title="Design">
          <Fact label="Seeds">
            <span className="num">{formatInteger(seeds.length)}</span>{" "}
            {consecutive ? (
              <span className="text-fg-3">
                consecutive, <span className="num">{firstSeed}</span> to{" "}
                <span className="num">{lastSeed}</span>
              </span>
            ) : (
              <span className="num text-fg-3">
                {seeds.slice(0, 12).join(", ")}
                {seeds.length > 12 ? ", …" : ""}
              </span>
            )}
          </Fact>
          <Fact label="Runs">
            <span className="num">{formatInteger(seeds.length * config.variants.length)}</span>{" "}
            <span className="text-fg-3">
              every variant on every seed, one thread, seed-major order
            </span>
          </Fact>
          <Fact label="Shared scenario">
            <span className="num">{base.run_id}</span>{" "}
            <span className="text-fg-3">
              · {formatSpan(horizon)} simulated per run
              {config.horizon_ns !== null ? " (overrides the base horizon)" : ""} ·{" "}
              {base.scenario?.agents.length ?? 0} agents · tick size{" "}
              <span className="num">{base.tick_size_minor}</span> minor
            </span>
          </Fact>
          <Fact label="Run artifacts">
            <span className="text-fg-3">
              {config.artifacts === "All"
                ? "every run directory was written"
                : "none kept: each run's digests are recorded, and a cell can be regenerated and checked against them"}
            </span>
          </Fact>
        </Block>
        <Block title="Strategy definitions">
          {config.variants.map((variant) => (
            <Fact key={variant.name} label={variant.name}>
              {variant.policy_overrides.length === 0 ? (
                <span className="text-fg-3">the base scenario, unchanged</span>
              ) : (
                variant.policy_overrides.map((override) => (
                  <span key={override.agent_id} className="block">
                    agent <span className="num">{override.agent_id}</span> runs{" "}
                    {POLICY_LABEL[override.policy.kind] ?? override.policy.kind}
                    <span className="num block text-xs text-fg-3">
                      {policySummary(override.policy)}
                    </span>
                  </span>
                ))
              )}
            </Fact>
          ))}
          <p className="text-xs text-fg-3">
            A variant replaces the policy of the named agents and nothing else: the agent keeps its
            ID, random streams, wakeup schedule, latency, and limits.
          </p>
        </Block>
        <Block title="Paired comparison">
          {comparison ? (
            <>
              <p>
                Differences are <span className="num">{comparison.candidate}</span> minus{" "}
                <span className="num">{comparison.baseline}</span> for agent{" "}
                <span className="num">{comparison.subject_agent_id}</span>, on the same seed, over
                the seeds both variants completed and for which the metric exists in both.
              </p>
              <p className="text-xs text-fg-3">
                Within a seed the two runs share everything exogenous: the fundamental path, every
                agent&apos;s wakeup times, the value traders&apos; signal noise, and the noise
                traders&apos; draws. They do not share a market history: the subject&apos;s quotes
                change who trades with whom and everything after. Pairing removes exogenous noise;
                it does not hold the rest of the market still.
              </p>
            </>
          ) : (
            <p className="text-fg-3">No comparison was defined, so no paired statistics exist.</p>
          )}
        </Block>
      </div>
      <div>
        <Block title="Confidence intervals">
          <Fact label="Student t">
            <span className="text-fg-3">
              two-sided 95% for the mean: mean ± t(0.975, n − 1) · s / √n, with the sample standard
              deviation (n − 1). Exact coverage assumes independent, normal differences; P&amp;L
              differences are not normal, so it is approximate.
            </span>
          </Fact>
          <Fact label="Bootstrap">
            <span className="text-fg-3">
              percentile, two-sided 95%,{" "}
              <span className="num">{formatInteger(config.bootstrap.resamples)}</span> resamples of
              the paired differences, stream seed{" "}
              <span className="num">{config.bootstrap.seed}</span>. Assumes independent seeds only.
            </span>
          </Fact>
          <Fact label="Arithmetic">
            <span className="text-fg-3">
              integer, reported in thousandths of each unit
              {statistics ? `, rounding ${statistics.rounding.replaceAll("_", " ")}` : ""}; computed
              by the engine, not by this terminal.
            </span>
          </Fact>
        </Block>
        <Block title="Runs excluded">
          {results === null ? (
            <p className="text-fg-3">Known when the experiment finishes.</p>
          ) : failed.length === 0 ? (
            <p className="text-fg-3">None. Every run completed and every seed is paired.</p>
          ) : (
            <>
              <p>
                <span className="num">{formatInteger(failed.length)}</span> runs failed and their
                seeds are not paired. Nothing was imputed.
              </p>
              <ul className="num text-xs text-fg-3">
                {failed.slice(0, 6).map((run) => (
                  <li key={run.run_id}>
                    {run.run_id}: {run.error_code}
                  </li>
                ))}
                {failed.length > 6 ? (
                  <li>and {formatInteger(failed.length - 6)} more in experiment.json</li>
                ) : null}
              </ul>
            </>
          )}
        </Block>
        <Block title="Provenance">
          <Fact label="Engine">
            <span className="num">{results?.engine_version ?? experiment.engine_version}</span>
            {results ? (
              <span className="text-fg-3">
                {" "}
                · {results.provenance.build_type} · {results.provenance.compiler} ·{" "}
                <span className="num">{results.provenance.git_revision}</span>
              </span>
            ) : null}
          </Fact>
          <Fact label="Experiment config">
            <Digest
              value={results?.results.config_digest ?? experiment.config_digest}
              label="experiment config digest"
            />
          </Fact>
          <Fact label="Results">
            <Digest
              value={results?.results_digest ?? experiment.results_digest}
              label="results digest"
            />
          </Fact>
          <Fact label="runs.jsonl">
            <Digest value={results?.artifacts["runs.jsonl"]} label="runs.jsonl checksum" />
          </Fact>
        </Block>
        <Block title="Limitations">
          <ul className="list-disc pl-4 text-xs text-fg-3">
            <li>
              The intervals describe variation across seeds of one stylized model at one parameter
              setting. They are not evidence about real markets or about profitability.
            </li>
            <li>
              {results?.results.comparison?.metrics.length ?? 19} metrics are reported with no
              multiplicity correction; some intervals will exclude zero by chance.
            </li>
            <li>
              No Sharpe ratio is reported: a run yields one terminal P&amp;L, not a return series.
            </li>
            <li>
              Max drawdown has the resolution of the sampling schedule and can miss an excursion
              between two samples.
            </li>
          </ul>
        </Block>
      </div>
    </div>
  );
}
