import { useQuery, useQueryClient } from "@tanstack/react-query";
import { useEffect, useMemo } from "react";
import { useSearchParams } from "react-router";

import { WINDOW_SIZE, sampleTimesQuery, sampleWindowQuery } from "@/lib/api/queries";
import type { AgentColumn, BookSnapshot, MarketColumn, SampleWindow } from "@/lib/api/types";
import { indexAtOrBefore } from "@/lib/time/asof";
import { usePlayback } from "@/state/playback";

/**
 * Binds the playback cursor to a run's recorded samples. The cursor starts at the time in
 * the URL (`?t=` elapsed ns, resolved to the latest sample at or before it), or at the end
 * of the run.
 */
export function useRunCursor(runId: string, attachKey = runId) {
  const times = useQuery(sampleTimesQuery(runId));
  const [params] = useSearchParams();
  const requested = params.get("t");

  useEffect(() => {
    const values = times.data?.timestamp_ns;
    if (!values) return;
    let index: number | undefined;
    if (requested !== null && /^\d+$/.test(requested)) {
      const found = indexAtOrBefore(values, Number(requested));
      index = found >= 0 ? found : 0;
    }
    usePlayback.getState().attach(attachKey, values.length, index);
    // The URL seeds the cursor once; afterwards the cursor owns the time.
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [attachKey, times.data]);

  return times;
}

export function useCursorIndex(): number {
  return usePlayback((state) => state.index);
}

export interface CursorSample {
  index: number;
  /** Elapsed simulation time of the sample under the cursor. */
  timestampNs: number | null;
  market: Record<MarketColumn, number | null> | null;
  markSource: string | null;
  book: BookSnapshot | null;
  agents: Record<string, Record<AgentColumn, number | null>>;
  isPending: boolean;
  error: unknown;
}

function rowOf<K extends string>(
  columns: Record<K, (number | null)[]>,
  offset: number,
): Record<K, number | null> {
  const row = {} as Record<K, number | null>;
  for (const key of Object.keys(columns) as K[]) row[key] = columns[key][offset] ?? null;
  return row;
}

/**
 * The exact recorded state at a sample index: the market row, the depth snapshot, and
 * every requested agent's account. Windows of samples are fetched as the cursor reaches
 * them and the next one is fetched ahead, so playback does not wait at a boundary.
 */
export function useSampleAt(runId: string, index: number, agents = "all"): CursorSample {
  const client = useQueryClient();
  const window = Math.floor(index / WINDOW_SIZE);
  const query = useQuery(sampleWindowQuery(runId, window, agents));

  useEffect(() => {
    const data = query.data;
    if (!data) return;
    const offset = index - window * WINDOW_SIZE;
    const lastWindow = Math.floor(Math.max(0, data.sample_count - 1) / WINDOW_SIZE);
    if (offset > WINDOW_SIZE * 0.6 && window < lastWindow) {
      void client.query(sampleWindowQuery(runId, window + 1, agents)).catch(() => undefined);
    } else if (offset < WINDOW_SIZE * 0.4 && window > 0) {
      void client.query(sampleWindowQuery(runId, window - 1, agents)).catch(() => undefined);
    }
  }, [client, runId, index, window, agents, query.data]);

  return useMemo(() => {
    const data: SampleWindow | undefined = query.data;
    const offset = data ? index - data.start : -1;
    if (!data || offset < 0 || offset >= data.count) {
      return {
        index,
        timestampNs: null,
        market: null,
        markSource: null,
        book: null,
        agents: {},
        isPending: query.isPending,
        error: query.error,
      };
    }
    const market = rowOf(data.columns, offset);
    const agentRows: Record<string, Record<AgentColumn, number | null>> = {};
    for (const [agent, columns] of Object.entries(data.agents))
      agentRows[agent] = rowOf(columns, offset);
    return {
      index,
      timestampNs: market.timestamp_ns,
      market,
      markSource:
        market.mark_source === null ? null : (data.mark_sources[market.mark_source] ?? null),
      book: data.books[offset] ?? null,
      agents: agentRows,
      isPending: false,
      error: null,
    };
  }, [query.data, query.isPending, query.error, index]);
}
