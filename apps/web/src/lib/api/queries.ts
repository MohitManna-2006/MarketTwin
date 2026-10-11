// Query definitions: one function per endpoint, with keys that make every request
// cacheable on its own. Selecting another agent refetches that agent's series and
// nothing else; a completed run's recorded data never goes stale.

import { queryOptions } from "@tanstack/react-query";

import { get } from "./client";
import type {
  AgentSeries,
  EventPage,
  ExperimentDetail,
  ExperimentList,
  ExperimentResultsDocument,
  FieldCatalogue,
  Health,
  HistoricalComparison,
  HistoricalEvent,
  HistoricalImport,
  HistoricalPage,
  HistoricalProvider,
  HistoricalSample,
  HistoricalSession,
  LedgerPage,
  MarketSeries,
  PairedValuesDocument,
  ReplayView,
  RunDetail,
  RunList,
  SampleTimes,
  SampleWindow,
  ScenarioDetail,
  ScenarioSummary,
  TradePage,
} from "./types";

const FOREVER = Number.POSITIVE_INFINITY;
export const WINDOW_SIZE = 512;

const active = (status: string | undefined) => status === "pending" || status === "running";

export const historicalProvidersQuery = () =>
  queryOptions({
    queryKey: ["historical", "providers"],
    queryFn: ({ signal }) =>
      get<{ providers: HistoricalProvider[] }>("/historical/providers", undefined, signal),
    staleTime: 30_000,
  });

export const historicalImportsQuery = (limit = 100) =>
  queryOptions({
    queryKey: ["historical", "imports", limit],
    queryFn: ({ signal }) =>
      get<{ imports: HistoricalImport[]; count: number }>("/historical/imports", { limit }, signal),
    refetchInterval: (query) =>
      query.state.data?.imports.some((item) => active(item.status)) ? 500 : 10_000,
  });

export const historicalSessionQuery = (sessionId: string) =>
  queryOptions({
    queryKey: ["historical", "sessions", sessionId],
    queryFn: ({ signal }) =>
      get<HistoricalSession>(`/historical/sessions/${sessionId}`, undefined, signal),
    staleTime: FOREVER,
  });

export const historicalSeriesQuery = (sessionId: string, limit = 500) =>
  queryOptions({
    queryKey: ["historical", "sessions", sessionId, "series", limit],
    queryFn: ({ signal }) =>
      get<HistoricalPage<HistoricalSample>>(
        `/historical/sessions/${sessionId}/series`,
        { limit },
        signal,
      ),
    staleTime: FOREVER,
  });

export const historicalEventsQuery = (sessionId: string, limit = 500) =>
  queryOptions({
    queryKey: ["historical", "sessions", sessionId, "events", limit],
    queryFn: ({ signal }) =>
      get<HistoricalPage<HistoricalEvent>>(
        `/historical/sessions/${sessionId}/events`,
        { limit },
        signal,
      ),
    staleTime: FOREVER,
  });

export const historicalComparisonQuery = (runId: string, sessionId: string, enabled: boolean) =>
  queryOptions({
    queryKey: ["historical", "comparisons", runId, sessionId],
    queryFn: ({ signal }) =>
      get<HistoricalComparison>(
        "/historical/comparisons",
        { run_id: runId, session_id: sessionId },
        signal,
      ),
    enabled,
  });

export const healthQuery = () =>
  queryOptions({
    queryKey: ["health"],
    queryFn: ({ signal }) => get<Health>("/health", undefined, signal),
    refetchInterval: 5_000,
    retry: false,
  });

export const scenariosQuery = () =>
  queryOptions({
    queryKey: ["scenarios"],
    queryFn: ({ signal }) => get<ScenarioSummary[]>("/scenarios", undefined, signal),
    staleTime: FOREVER,
  });

export const scenarioQuery = (key: string) =>
  queryOptions({
    queryKey: ["scenarios", key],
    queryFn: ({ signal }) => get<ScenarioDetail>(`/scenarios/${key}`, undefined, signal),
    staleTime: FOREVER,
  });

export const fieldCatalogueQuery = () =>
  queryOptions({
    queryKey: ["schema", "fields"],
    queryFn: ({ signal }) => get<FieldCatalogue>("/schema/fields", undefined, signal),
    staleTime: FOREVER,
  });

export const runsQuery = (limit = 50, includeCells = true) =>
  queryOptions({
    queryKey: ["runs", "list", limit, includeCells],
    queryFn: ({ signal }) => get<RunList>("/runs", { limit, include_cells: includeCells }, signal),
    refetchInterval: (query) =>
      query.state.data?.runs.some((run) => active(run.status) || run.replay_status === "running")
        ? 800
        : 10_000,
  });

export const runQuery = (runId: string) =>
  queryOptions({
    queryKey: ["runs", runId, "detail"],
    queryFn: ({ signal }) => get<RunDetail>(`/runs/${runId}`, undefined, signal),
    refetchInterval: (query) => {
      const run = query.state.data;
      return run && (active(run.status) || run.replay.status === "running") ? 500 : false;
    },
  });

export const replayQuery = (runId: string) =>
  queryOptions({
    queryKey: ["runs", runId, "replay"],
    queryFn: ({ signal }) => get<ReplayView>(`/runs/${runId}/replay`, undefined, signal),
    refetchInterval: (query) => (query.state.data?.status === "running" ? 400 : false),
  });

export const sampleTimesQuery = (runId: string) =>
  queryOptions({
    queryKey: ["runs", runId, "sample-times"],
    queryFn: ({ signal }) => get<SampleTimes>(`/runs/${runId}/samples/times`, undefined, signal),
    staleTime: FOREVER,
  });

export const marketSeriesQuery = (runId: string, maxPoints = 20_000) =>
  queryOptions({
    queryKey: ["runs", runId, "series", "market", maxPoints],
    queryFn: ({ signal }) =>
      get<MarketSeries>(`/runs/${runId}/series/market`, { max_points: maxPoints }, signal),
    staleTime: FOREVER,
  });

export const agentSeriesQuery = (runId: string, agentId: number, maxPoints = 20_000) =>
  queryOptions({
    queryKey: ["runs", runId, "series", "agent", agentId, maxPoints],
    queryFn: ({ signal }) =>
      get<AgentSeries>(
        `/runs/${runId}/series/agents/${agentId}`,
        { max_points: maxPoints },
        signal,
      ),
    staleTime: FOREVER,
  });

/** Exact recorded state for one fixed window of samples: market, book, and agents. */
export const sampleWindowQuery = (runId: string, window: number, agents: string) =>
  queryOptions({
    queryKey: ["runs", runId, "samples", window, agents],
    queryFn: ({ signal }) =>
      get<SampleWindow>(
        `/runs/${runId}/samples`,
        { start: window * WINDOW_SIZE, count: WINDOW_SIZE, agents },
        signal,
      ),
    staleTime: FOREVER,
  });

export interface TradeFilter {
  fromNs?: number;
  toNs?: number;
  agentId?: number;
  offset?: number;
  limit?: number;
}

export const tradesQuery = (runId: string, filter: TradeFilter) =>
  queryOptions({
    queryKey: ["runs", runId, "trades", filter],
    queryFn: ({ signal }) =>
      get<TradePage>(
        `/runs/${runId}/trades`,
        {
          from_ns: filter.fromNs,
          to_ns: filter.toNs,
          agent_id: filter.agentId,
          offset: filter.offset ?? 0,
          limit: filter.limit ?? 200,
        },
        signal,
      ),
    staleTime: FOREVER,
  });

export const ledgerQuery = (runId: string, agentId: number, offset: number, limit: number) =>
  queryOptions({
    queryKey: ["runs", runId, "ledger", agentId, offset, limit],
    queryFn: ({ signal }) =>
      get<LedgerPage>(`/runs/${runId}/agents/${agentId}/ledger`, { offset, limit }, signal),
    staleTime: FOREVER,
  });

export interface EventFilter {
  stream: "market" | "accounting";
  channel?: "commands" | "execution";
  kinds?: readonly string[];
  agentId?: number;
  orderId?: number;
}

export function eventParams(filter: EventFilter) {
  return {
    stream: filter.stream,
    channel: filter.channel,
    kind: filter.kinds && filter.kinds.length > 0 ? filter.kinds : undefined,
    agent_id: filter.agentId,
    order_id: filter.orderId,
  };
}

export const eventsQuery = (runId: string, filter: EventFilter, offset: number, limit: number) =>
  queryOptions({
    queryKey: ["runs", runId, "events", filter, "page", offset, limit],
    queryFn: ({ signal }) =>
      get<EventPage>(`/runs/${runId}/events`, { ...eventParams(filter), offset, limit }, signal),
    staleTime: FOREVER,
  });

/** The page of records ending at the latest one at or before a time or a sequence. */
export const eventsAtQuery = (
  runId: string,
  filter: EventFilter,
  at: { ns?: number; sequence?: number },
  limit: number,
) =>
  queryOptions({
    queryKey: ["runs", runId, "events", filter, "at", at, limit],
    queryFn: ({ signal }) =>
      get<EventPage>(
        `/runs/${runId}/events`,
        {
          ...eventParams(filter),
          at_or_before_ns: at.ns,
          at_or_before_sequence: at.sequence,
          limit,
        },
        signal,
      ),
    staleTime: FOREVER,
  });

export const experimentsQuery = (limit = 50) =>
  queryOptions({
    queryKey: ["experiments", "list", limit],
    queryFn: ({ signal }) => get<ExperimentList>("/experiments", { limit }, signal),
    refetchInterval: (query) =>
      query.state.data?.experiments.some((item) => active(item.status)) ? 800 : 10_000,
  });

export const experimentQuery = (experimentId: string) =>
  queryOptions({
    queryKey: ["experiments", experimentId, "detail"],
    queryFn: ({ signal }) =>
      get<ExperimentDetail>(`/experiments/${experimentId}`, undefined, signal),
    refetchInterval: (query) => (active(query.state.data?.status) ? 500 : false),
  });

export const experimentResultsQuery = (experimentId: string, enabled: boolean) =>
  queryOptions({
    queryKey: ["experiments", experimentId, "results"],
    queryFn: async ({ signal }) =>
      (
        await get<{ document: ExperimentResultsDocument }>(
          `/experiments/${experimentId}/results`,
          undefined,
          signal,
        )
      ).document,
    staleTime: FOREVER,
    enabled,
  });

export const pairedValuesQuery = (experimentId: string, enabled: boolean) =>
  queryOptions({
    queryKey: ["experiments", experimentId, "paired-values"],
    queryFn: async ({ signal }) =>
      (
        await get<{ document: PairedValuesDocument }>(
          `/experiments/${experimentId}/paired-values`,
          undefined,
          signal,
        )
      ).document,
    staleTime: FOREVER,
    enabled,
  });
