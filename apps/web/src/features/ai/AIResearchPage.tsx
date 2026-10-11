import { useMutation, useQuery, useQueryClient } from "@tanstack/react-query";
import { useState } from "react";
import { Link, useNavigate, useParams } from "react-router";

import { ContextBar } from "@/components/shell/ContextBar";
import { Page } from "@/components/shell/TerminalShell";
import { Button } from "@/components/ui/Button";
import { get, post, describeError } from "@/lib/api/client";

type Claim = {
  metric: string;
  scope: string;
  unit: string;
  pairs: number;
  baseline_mean_milli: number | null;
  candidate_mean_milli: number | null;
  difference_mean_milli: number | null;
  difference_t_interval_milli: [number, number] | null;
  difference_bootstrap_interval_milli: [number, number] | null;
  evidence_ref: string;
};
type Plan = {
  question: string;
  hypothesis: string;
  scenario_key: string;
  scenario_digest: string;
  seed_count: number;
  total_runs: number;
  subject_agent_id: number;
  baseline: string;
  candidate: string;
  primary_metric: string;
  secondary_metrics: string[];
  analysis_topics: string[];
  methodology: string;
  limitations: string[];
  config_digest: string;
};
type Report = {
  question: string;
  hypothesis: string;
  experiment_id: string;
  engine_version: string;
  config_digest: string;
  results_digest: string;
  pairs_completed: number;
  claims: Claim[];
  interpretation: string;
  limitations: string[];
  follow_up: string[];
  replay_status: string;
};
type Event = {
  id: number;
  created_at: string;
  stage: string;
  operation: string;
  status: string;
  detail: Record<string, unknown>;
};
type Message = { id: number; role: string; content: string; evidence: string[] };
type Investigation = {
  id: string;
  question: string;
  status: string;
  created_at: string;
  provider: string | null;
  model: string | null;
  plan: Plan | null;
  experiment_id: string | null;
  experiment_status: string | null;
  progress: {
    total: number;
    completed: number;
    failed: number;
    current_run_id: string | null;
  } | null;
  report: Report | null;
  error_code: string | null;
  error_message: string | null;
  events: Event[];
  messages: Message[];
};
type List = { total: number; investigations: Investigation[] };
const active = new Set(["CREATED", "PLANNING", "APPROVED", "EXECUTING", "ANALYZING"]);

function value(milli: number | null, unit: string) {
  if (milli === null) return "undefined";
  return `${(milli / 1000).toLocaleString("en-US", { maximumFractionDigits: 3 })} ${unit}`;
}
function label(name: string) {
  return name.replaceAll("_", " ");
}

export function AIResearchPage() {
  const { investigationId } = useParams();
  const navigate = useNavigate();
  const queryClient = useQueryClient();
  const [question, setQuestion] = useState("");
  const [followup, setFollowup] = useState("");
  const [selected, setSelected] = useState<string | null>(null);
  const [inspector, setInspector] = useState(true);
  const list = useQuery({
    queryKey: ["ai", "list"],
    queryFn: () => get<List>("/ai/investigations", { limit: 50 }),
    refetchInterval: 5000,
  });
  const detail = useQuery({
    queryKey: ["ai", investigationId],
    enabled: Boolean(investigationId),
    queryFn: () => get<Investigation>(`/ai/investigations/${investigationId}`),
    refetchInterval: (query) => (active.has(query.state.data?.status ?? "") ? 750 : false),
  });
  const refresh = () => {
    void queryClient.invalidateQueries({ queryKey: ["ai"] });
  };
  const create = useMutation({
    mutationFn: async () => {
      const row = await post<Investigation>("/ai/investigations", { question });
      await post(`/ai/investigations/${row.id}/plan`);
      return row;
    },
    onSuccess: (row) => {
      setQuestion("");
      refresh();
      void navigate(`/ai-research/${row.id}`);
    },
  });
  const action = useMutation({
    mutationFn: ({ verb, body }: { verb: string; body?: unknown }) =>
      post<Investigation>(`/ai/investigations/${investigationId}/${verb}`, body),
    onSuccess: refresh,
  });
  const send = useMutation({
    mutationFn: () => post(`/ai/investigations/${investigationId}/messages`, { content: followup }),
    onSuccess: () => {
      setFollowup("");
      refresh();
    },
  });
  const row = detail.data;
  const report = row?.report;
  const selectedClaim = report?.claims.find((claim) => claim.evidence_ref === selected);
  const error = create.error ?? action.error ?? send.error ?? detail.error;

  return (
    <Page
      context={
        <ContextBar
          crumbs={[
            { label: "AI Research", to: "/ai-research" },
            ...(row ? [{ label: row.id, verbatim: true }] : []),
          ]}
          status={
            row ? (
              <span className="label text-accent">{row.status.replaceAll("_", " ")}</span>
            ) : undefined
          }
        />
      }
    >
      <div className="flex h-full min-h-0 flex-col bg-canvas lg:flex-row">
        <aside
          className="w-full shrink-0 border-b border-line bg-surface-0 lg:w-[260px] lg:overflow-auto lg:border-r lg:border-b-0"
          aria-label="Research context"
        >
          <div className="border-b border-line p-4">
            <h2 className="label text-fg-3">INVESTIGATIONS</h2>
          </div>
          {list.data?.investigations.map((item) => (
            <Link
              key={item.id}
              to={`/ai-research/${item.id}`}
              className={`block border-b border-line-subtle px-4 py-3 hover:bg-surface-hover ${item.id === investigationId ? "bg-surface-selected" : ""}`}
            >
              <div className="num mb-1 text-xs text-accent">{item.id}</div>
              <div className="line-clamp-2 text-sm text-fg">{item.question}</div>
              <div className="label mt-1 text-fg-3">{item.status.replaceAll("_", " ")}</div>
            </Link>
          ))}
          {list.isError && (
            <p className="p-4 text-sm text-neg">{describeError(list.error).detail}</p>
          )}
        </aside>
        <section
          className="min-w-0 flex-1 overflow-auto px-5 py-6 md:px-8"
          aria-label="Research workbench"
        >
          <div className="mx-auto max-w-[880px]">
            <p className="label mb-2 text-accent">MARKETTWIN / RESEARCH WORKBENCH</p>
            <h1 className="mb-6 text-2xl font-medium text-fg">AI research</h1>
            <form
              onSubmit={(event) => {
                event.preventDefault();
                if (question.trim()) create.mutate();
              }}
              className="mb-8 border border-line bg-surface-0 p-4"
            >
              <label htmlFor="research-question" className="label mb-2 block text-fg-2">
                RESEARCH QUESTION
              </label>
              <textarea
                id="research-question"
                value={question}
                onChange={(event) => setQuestion(event.target.value)}
                maxLength={2000}
                minLength={10}
                rows={3}
                placeholder="Compare inventory-aware and baseline market making over paired seeds during liquidity withdrawal…"
                className="w-full resize-y border border-line bg-surface-1 p-3 text-sm text-fg outline-none focus:border-accent"
              />
              <div className="mt-3 flex justify-end">
                <Button
                  variant="primary"
                  type="submit"
                  disabled={create.isPending || question.trim().length < 10}
                >
                  Build research plan
                </Button>
              </div>
            </form>
            {error && (
              <div role="alert" className="mb-5 border border-neg p-3 text-sm text-neg">
                {describeError(error).detail}
              </div>
            )}
            {detail.isLoading && <p className="text-fg-3">Loading investigation…</p>}
            {row && (
              <>
                <article className="mb-6 border border-line bg-surface-0 p-5">
                  <p className="label mb-2 text-fg-3">ORIGINAL QUESTION</p>
                  <p className="text-base text-fg">{row.question}</p>
                </article>
                {row.error_message && (
                  <div
                    role="status"
                    className="mb-6 border border-line bg-surface-1 p-4 text-sm text-fg-2"
                  >
                    <strong className="text-accent">{row.error_code}</strong> · {row.error_message}
                  </div>
                )}
                {row.plan && (
                  <article className="mb-6 border border-line bg-surface-0 p-5">
                    <h2 className="mb-4 text-lg text-fg">Experiment plan</h2>
                    <div className="grid gap-4 text-sm sm:grid-cols-2">
                      <Field title="Scenario" value={label(row.plan.scenario_key)} />
                      <Field title="Subject agent" value={String(row.plan.subject_agent_id)} />
                      <Field title="Strategy A" value={label(row.plan.baseline)} />
                      <Field title="Strategy B" value={label(row.plan.candidate)} />
                      <Field title="Seeds" value={`${row.plan.seed_count} paired`} />
                      <Field title="Expected work" value={`${row.plan.total_runs} native runs`} />
                      <Field title="Primary metric" value={label(row.plan.primary_metric)} />
                      <Field
                        title="Secondary metrics"
                        value={row.plan.secondary_metrics.map(label).join(", ") || "None"}
                      />
                      {row.plan.analysis_topics?.length > 0 && (
                        <Field
                          title="Analysis topics"
                          value={row.plan.analysis_topics.join(", ")}
                        />
                      )}
                    </div>
                    <p className="mt-5 text-sm text-fg-2">{row.plan.hypothesis}</p>
                    <p className="mt-2 text-xs text-fg-3">{row.plan.methodology}</p>
                    <p className="num mt-4 break-all text-xs text-fg-3">
                      Plan digest {row.plan.config_digest}
                    </p>
                    {row.status === "AWAITING_APPROVAL" && (
                      <div className="mt-5 flex flex-wrap gap-2 border-t border-line pt-4">
                        <Button
                          variant="solid"
                          disabled={action.isPending}
                          onClick={() =>
                            action.mutate({
                              verb: "approve",
                              body: { config_digest: row.plan?.config_digest },
                            })
                          }
                        >
                          Approve &amp; run
                        </Button>
                        <Button
                          disabled={action.isPending}
                          onClick={() => {
                            setQuestion(row.question);
                            void navigate("/ai-research");
                          }}
                        >
                          Edit request
                        </Button>
                        <Button
                          variant="danger"
                          disabled={action.isPending}
                          onClick={() => action.mutate({ verb: "reject" })}
                        >
                          Reject
                        </Button>
                      </div>
                    )}
                  </article>
                )}
                {row.status === "EXECUTING" && (
                  <div className="mb-6 border border-line bg-surface-0 p-5">
                    <h2 className="text-fg">Native experiment running</h2>
                    <p className="num mt-2 text-sm text-fg-2">
                      {row.progress
                        ? `${row.progress.completed} / ${row.progress.total} runs completed; ${row.progress.failed} failed`
                        : "Awaiting native progress"}
                    </p>
                    {row.experiment_id && (
                      <Link
                        className="mt-2 block text-sm text-accent underline"
                        to={`/experiments/${row.experiment_id}`}
                      >
                        Open experiment {row.experiment_id}
                      </Link>
                    )}
                    <Button
                      className="mt-4"
                      variant="danger"
                      onClick={() => action.mutate({ verb: "cancel" })}
                    >
                      Cancel experiment
                    </Button>
                  </div>
                )}
                {report && (
                  <article className="mb-6 border border-line bg-surface-0 p-5">
                    <p className="label text-accent">INSTITUTIONAL RESEARCH NOTE</p>
                    <h2 className="mt-2 text-xl text-fg">{report.question}</h2>
                    <h3 className="label mt-6 mb-2 text-fg-3">HYPOTHESIS &amp; METHODOLOGY</h3>
                    <p className="text-sm text-fg-2">{report.hypothesis}</p>
                    <p className="mt-2 text-sm text-fg-3">{row.plan?.methodology}</p>
                    <h3 className="label mt-6 mb-2 text-fg-3">
                      QUANTITATIVE FINDINGS · CANDIDATE MINUS BASELINE
                    </h3>
                    <div className="overflow-x-auto">
                      <table className="w-full text-left text-sm">
                        <thead>
                          <tr className="border-b border-line text-fg-3">
                            <th className="py-2">Metric</th>
                            <th>Baseline mean</th>
                            <th>Candidate mean</th>
                            <th>Paired effect</th>
                            <th>95% t interval</th>
                          </tr>
                        </thead>
                        <tbody>
                          {report.claims.map((claim) => (
                            <tr
                              key={claim.evidence_ref}
                              onClick={() => {
                                setSelected(claim.evidence_ref);
                                setInspector(true);
                              }}
                              onKeyDown={(event) => {
                                if (event.key === "Enter") {
                                  setSelected(claim.evidence_ref);
                                  setInspector(true);
                                }
                              }}
                              tabIndex={0}
                              className={`cursor-pointer border-b border-line-subtle hover:bg-surface-hover focus:outline-accent ${selected === claim.evidence_ref ? "bg-surface-selected" : ""}`}
                            >
                              <td className="py-2 text-accent">{label(claim.metric)}</td>
                              <td className="num">
                                {value(claim.baseline_mean_milli, claim.unit)}
                              </td>
                              <td className="num">
                                {value(claim.candidate_mean_milli, claim.unit)}
                              </td>
                              <td className="num">
                                {value(claim.difference_mean_milli, claim.unit)}
                              </td>
                              <td className="num">
                                {claim.difference_t_interval_milli
                                  ? `${value(claim.difference_t_interval_milli[0], claim.unit)} to ${value(claim.difference_t_interval_milli[1], claim.unit)}`
                                  : "undefined"}
                              </td>
                            </tr>
                          ))}
                        </tbody>
                      </table>
                    </div>
                    <h3 className="label mt-6 mb-2 text-fg-3">INTERPRETATION</h3>
                    <p className="text-sm leading-6 text-fg-2">{report.interpretation}</p>
                    <h3 className="label mt-6 mb-2 text-fg-3">LIMITATIONS</h3>
                    <ul className="list-disc space-y-1 pl-5 text-sm text-fg-2">
                      {report.limitations.map((item) => (
                        <li key={item}>{item}</li>
                      ))}
                    </ul>
                    <h3 className="label mt-6 mb-2 text-fg-3">FOLLOW-UP RESEARCH</h3>
                    <ul className="list-disc space-y-1 pl-5 text-sm text-fg-2">
                      {report.follow_up.map((item) => (
                        <li key={item}>{item}</li>
                      ))}
                    </ul>
                    <h3 className="label mt-6 mb-2 text-fg-3">REPRODUCIBILITY</h3>
                    <Link
                      to={`/experiments/${report.experiment_id}`}
                      className="text-sm text-accent underline"
                    >
                      Open experiment {report.experiment_id}
                    </Link>
                    <a
                      href={`/api/v1/ai/investigations/${row.id}/report/markdown`}
                      download={`markettwin-${row.id}.md`}
                      className="ml-4 text-sm text-accent underline"
                    >
                      Download Markdown report
                    </a>
                    <p className="num mt-2 break-all text-xs text-fg-3">
                      Config {report.config_digest}
                      <br />
                      Results {report.results_digest}
                      <br />
                      Engine {report.engine_version} · {report.pairs_completed} completed pairs
                    </p>
                  </article>
                )}
                {report && (
                  <section
                    className="mb-6 border border-line bg-surface-0 p-5"
                    aria-label="Follow-up research"
                  >
                    <h2 className="mb-4 text-lg text-fg">Follow-up</h2>
                    {row.messages.slice(1).map((message) => (
                      <div key={message.id} className="mb-3 border-l-2 border-line pl-3">
                        <p className="label text-fg-3">{message.role}</p>
                        <p className="text-sm text-fg-2">{message.content}</p>
                        {message.evidence.length > 0 && (
                          <p className="num text-xs text-accent">{message.evidence.join(", ")}</p>
                        )}
                      </div>
                    ))}
                    <form
                      onSubmit={(event) => {
                        event.preventDefault();
                        if (followup.trim()) send.mutate();
                      }}
                      className="flex gap-2"
                    >
                      <input
                        aria-label="Follow-up question"
                        value={followup}
                        onChange={(event) => setFollowup(event.target.value)}
                        maxLength={2000}
                        className="min-w-0 flex-1 border border-line bg-surface-1 px-3 text-sm text-fg outline-none focus:border-accent"
                      />
                      <Button type="submit" disabled={send.isPending || followup.trim().length < 2}>
                        Ask
                      </Button>
                    </form>
                  </section>
                )}
                <details className="mb-6 border border-line bg-surface-0 p-5">
                  <summary className="label cursor-pointer text-fg-2">
                    TOOL ACTIVITY · {row.events.length} EVENTS
                  </summary>
                  <ol className="mt-4 space-y-2">
                    {row.events.map((event) => (
                      <li
                        key={event.id}
                        className="grid grid-cols-[130px_1fr_auto] gap-3 text-xs text-fg-3"
                      >
                        <time className="num">{event.created_at}</time>
                        <span>{label(event.operation)}</span>
                        <span className="text-accent">{event.status}</span>
                      </li>
                    ))}
                  </ol>
                </details>
              </>
            )}
          </div>
        </section>
        <aside
          className={`shrink-0 border-t border-line bg-surface-0 lg:border-t-0 lg:border-l ${inspector ? "w-full lg:w-[320px]" : "w-auto"}`}
          aria-label="Evidence inspector"
        >
          <button
            className="label w-full border-b border-line px-4 py-4 text-left text-fg-2"
            onClick={() => setInspector(!inspector)}
            aria-expanded={inspector}
          >
            EVIDENCE INSPECTOR {inspector ? "−" : "+"}
          </button>
          {inspector && (
            <div className="space-y-4 p-4 text-sm">
              <Field title="Investigation" value={row?.id ?? "—"} />
              <Field title="Experiment" value={row?.experiment_id ?? "—"} />
              <Field title="Scenario" value={row?.plan?.scenario_key ?? "—"} />
              <Field title="Seed count" value={row?.plan ? String(row.plan.seed_count) : "—"} />
              <Field title="Engine" value={report?.engine_version ?? "—"} />
              <Field title="Model provider" value={row?.provider ?? "—"} />
              <Field title="Model" value={row?.model || "Not configured"} />
              <Field title="Replay" value={report?.replay_status ?? "Not checked"} />
              {selectedClaim && (
                <div className="border-t border-line pt-4">
                  <p className="label text-accent">SELECTED CLAIM</p>
                  <p className="mt-2 text-fg">{label(selectedClaim.metric)}</p>
                  <p className="num mt-2 break-all text-xs text-fg-3">
                    {selectedClaim.evidence_ref}
                  </p>
                  <p className="mt-2 text-fg-2">{selectedClaim.pairs} paired observations</p>
                  <Link
                    className="mt-3 block text-accent underline"
                    to={`/experiments/${row?.experiment_id ?? ""}`}
                  >
                    Inspect authoritative result
                  </Link>
                </div>
              )}
            </div>
          )}
        </aside>
      </div>
    </Page>
  );
}

function Field({ title, value: text }: { title: string; value: string }) {
  return (
    <dl>
      <dt className="label mb-1 text-fg-3">{title.toUpperCase()}</dt>
      <dd className="break-words text-fg-2">{text}</dd>
    </dl>
  );
}
