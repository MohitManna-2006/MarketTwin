// Builders for live-protocol messages in tests. Not imported by application code.

import type {
  EventsMessage,
  FeedStatus,
  LiveStatus,
  QuoteEvent,
  SnapshotMessage,
  StatusMessage,
  SymbolState,
  TradeEvent,
} from "./protocol";

const BASE_NS = 1_710_163_800_000_000_000n;

export function at(offsetMs: number): string {
  return String(BASE_NS + BigInt(offsetMs) * 1_000_000n);
}

let sequence = 0;
export function resetSequence(): void {
  sequence = 0;
}

const common = (symbol: string, offsetMs: number) => ({
  live_sequence: ++sequence,
  symbol,
  provider: "alpaca_live",
  feed: "iex",
  timestamp_ns: at(offsetMs),
  timestamp_precision_digits: 9,
  source_timestamp: "2024-03-11T13:30:00.000000000Z",
  receipt_wall_ns: at(offsetMs + 5),
  receipt_monotonic_ns: offsetMs * 1_000_000,
  classification: "in_order" as const,
  applied: true,
  conditions: ["@"],
  tape: "C",
});

export function trade(
  symbol: string,
  microUsd: number,
  offsetMs: number,
  extra: Partial<TradeEvent> = {},
): TradeEvent {
  return {
    ...common(symbol, offsetMs),
    kind: "trade",
    price_micro_usd: microUsd,
    source_price: String(microUsd / 1_000_000),
    quantity: 100,
    trade_id: sequence,
    venue: "V",
    ...extra,
  };
}

export function quote(
  symbol: string,
  bid: number | null,
  ask: number | null,
  offsetMs: number,
  extra: Partial<QuoteEvent> = {},
): QuoteEvent {
  return {
    ...common(symbol, offsetMs),
    kind: "quote",
    bid_micro_usd: bid,
    ask_micro_usd: ask,
    bid_size: bid === null ? null : 2,
    ask_size: ask === null ? null : 3,
    bid_venue: "V",
    ask_venue: "V",
    quote_state:
      bid !== null && ask !== null
        ? bid < ask
          ? "two_sided"
          : bid === ask
            ? "locked"
            : "crossed"
        : bid !== null
          ? "bid_only"
          : ask !== null
            ? "ask_only"
            : "empty",
    conditions: ["R"],
    ...extra,
  };
}

export function symbolState(
  symbol: string,
  lastTrade: TradeEvent | null,
  lastQuote: QuoteEvent | null,
  ageNs: number | null = 100_000_000,
  freshness: Partial<SymbolState["freshness"]> = {},
): SymbolState {
  const bid = lastQuote?.bid_micro_usd ?? null;
  const ask = lastQuote?.ask_micro_usd ?? null;
  const both = bid !== null && ask !== null;
  return {
    symbol,
    provider: "alpaca_live",
    feed: "iex",
    last_trade: lastTrade,
    quote: lastQuote,
    quote_state: lastQuote?.quote_state ?? "empty",
    spread_micro_usd: both ? ask - bid : null,
    midpoint_micro_usd: both && bid <= ask ? Math.floor((bid + ask) / 2) : null,
    quote_trade_skew_ns: null,
    counts: {
      trades: lastTrade ? 1 : 0,
      quotes: lastQuote ? 1 : 0,
      out_of_order: 0,
      duplicates: 0,
    },
    freshness: {
      status: ageNs === null ? "awaiting" : "fresh",
      trade_age_ns: lastTrade ? ageNs : null,
      quote_age_ns: lastQuote ? ageNs : null,
      update_age_ns: ageNs,
      trade_stale: false,
      quote_stale: false,
      skew_alert: false,
      spread_quality: both ? "current" : "unavailable",
      source_age_ns: ageNs,
      ...freshness,
    },
  };
}

export function liveStatus(
  feed: Partial<FeedStatus> = {},
  overrides: {
    market?: Partial<LiveStatus["market"]>;
    recording?: Partial<LiveStatus["recording"]>;
    pipeline?: Partial<LiveStatus["pipeline"]>;
  } = {},
): LiveStatus {
  const symbols = feed.desired_symbols ?? ["AAPL", "NVDA", "MSFT", "SPY"];
  return {
    feed: {
      state: "CONNECTED",
      feed: "iex",
      provider: "alpaca_live",
      source_label: "OBSERVED IEX DATA",
      test_data: false,
      scope: "single_venue",
      endpoint_host: "stream.data.alpaca.markets",
      desired_symbols: symbols,
      subscribed: { trades: symbols, quotes: symbols },
      subscription_status: "acknowledged",
      impairments: [],
      reconnects: 0,
      connections: 1,
      attempt: 0,
      next_retry_in_ms: null,
      connected_since: "2024-03-11T13:29:00.000Z",
      last_reconnect_at: null,
      last_message_at: "2024-03-11T13:30:00.000Z",
      last_message_age_ms: 100,
      frames: 10,
      data_frames: 8,
      messages: 16,
      protocol_errors: 0,
      last_error: null,
      recent_errors: [],
      credentials_configured: true,
      transitions: [],
      ...feed,
    },
    market: {
      activity: "active",
      last_observation_at: "2024-03-11T13:30:00.000Z",
      last_observation_age_ms: 100,
      stale_after_ms: 15_000,
      session: "regular_hours",
      note: "from the New York wall clock only",
      ...overrides.market,
    },
    pipeline: {
      inbox_depth: 0,
      inbox_capacity: 4096,
      inbox_high_water: 1,
      dropped_frames: 0,
      dropped_messages: 0,
      batches: 8,
      frames: 8,
      messages: 16,
      events: 16,
      rejected: 0,
      duplicates: 0,
      out_of_order: 0,
      unsupported: {},
      recent_rejects: [],
      messages_per_s: 4,
      events_per_s: 4,
      flush_interval_ms: 20,
      local_latency_us: { p50: 21_000, p99: 22_000, max: 23_000, samples: 8 },
      native_ns_per_message: { p50: 4_000, p99: 9_000, samples: 8 },
      ...overrides.pipeline,
    },
    clients: {
      connected: 1,
      capacity: 16,
      queue_capacity: 256,
      queue_depth_max: 0,
      queue_high_water: 1,
      dropped_messages: 0,
      dropped_events: 0,
      overflows: 0,
      slow_disconnects: 0,
      refused: 0,
    },
    recording: {
      active: false,
      id: null,
      started_at: null,
      upstream: null,
      gaps: 0,
      progress: null,
      finalizing: [],
      latest: [],
      ...overrides.recording,
    },
    clock: { server_wall_ns: at(0), note: "ages are intervals on the service's monotonic clock" },
  };
}

const envelope = (seq: number, stream = "stream-a") => ({
  v: 1,
  stream,
  seq,
  server_wall_ns: at(0),
});

export function snapshot(
  seq: number,
  status: LiveStatus,
  states: Record<string, SymbolState> = {},
  recent: (TradeEvent | QuoteEvent)[] = [],
  stream = "stream-a",
): SnapshotMessage {
  return {
    ...envelope(seq, stream),
    type: "snapshot",
    status,
    states,
    recent,
    watchlist: status.feed.desired_symbols,
  };
}

export function events(
  seq: number,
  list: (TradeEvent | QuoteEvent)[],
  states: Record<string, SymbolState> = {},
): EventsMessage {
  return { ...envelope(seq), type: "events", events: list, states, rejects: 0 };
}

export function statusMessage(
  seq: number,
  status: LiveStatus,
  states: Record<string, SymbolState> = {},
): StatusMessage {
  return { ...envelope(seq), type: "status", status, states };
}
