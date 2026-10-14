// Live stream protocol, version 1, as the hub writes it (services/api live_service.py).
//
// Prices are exact integers in millionths of a US dollar. UTC nanosecond instants are
// decimal strings, because a JavaScript number cannot hold one exactly.

export const LIVE_PROTOCOL = 1;

export type FeedState =
  | "STOPPED"
  | "CONNECTING"
  | "AUTHENTICATING"
  | "SUBSCRIBING"
  | "CONNECTED"
  | "RECONNECTING"
  | "DEGRADED"
  | "STOPPING";

export type FeedName = "test" | "iex" | "mock";
export type QuoteState = "empty" | "bid_only" | "ask_only" | "two_sided" | "locked" | "crossed";
export type Classification = "in_order" | "out_of_order" | "duplicate";
export type MarketActivity = "offline" | "none_yet" | "active" | "quiet";

interface EventBase {
  live_sequence: number;
  symbol: string;
  provider: string;
  feed: string;
  timestamp_ns: string;
  timestamp_precision_digits: number;
  source_timestamp: string;
  receipt_wall_ns: string;
  receipt_monotonic_ns: number;
  classification: Classification;
  applied: boolean;
  conditions: string[];
  tape: string;
}

export interface TradeEvent extends EventBase {
  kind: "trade";
  price_micro_usd: number;
  source_price: string;
  quantity: number;
  trade_id: number | string | null;
  venue: string;
}

export interface QuoteEvent extends EventBase {
  kind: "quote";
  bid_micro_usd: number | null;
  ask_micro_usd: number | null;
  bid_size: number | null;
  ask_size: number | null;
  bid_venue: string;
  ask_venue: string;
  quote_state: QuoteState;
}

export type LiveEvent = TradeEvent | QuoteEvent;

export interface Freshness {
  status: "awaiting" | "fresh" | "stale";
  trade_age_ns: number | null;
  quote_age_ns: number | null;
  update_age_ns: number | null;
  trade_stale: boolean;
  quote_stale: boolean;
  skew_alert: boolean;
  spread_quality: "unavailable" | "current" | "stale_quote" | "locked_or_crossed";
  source_age_ns: number | null;
}

export interface SymbolState {
  symbol: string;
  provider: string;
  feed: string;
  last_trade: TradeEvent | null;
  quote: QuoteEvent | null;
  quote_state: QuoteState;
  spread_micro_usd: number | null;
  midpoint_micro_usd: number | null;
  quote_trade_skew_ns: number | null;
  counts: { trades: number; quotes: number; out_of_order: number; duplicates: number };
  freshness: Freshness;
}

export interface FeedError {
  code: string;
  message: string;
  provider_code?: number | null;
  at: string;
}

export interface FeedStatus {
  state: FeedState;
  feed: FeedName;
  provider: string;
  source_label: string;
  test_data: boolean;
  scope: string;
  endpoint_host: string | null;
  desired_symbols: string[];
  subscribed: { trades: string[]; quotes: string[] };
  subscription_status: "none" | "pending" | "acknowledged" | "mismatch";
  impairments: { code: string; message: string }[];
  reconnects: number;
  connections: number;
  attempt: number;
  next_retry_in_ms: number | null;
  connected_since: string | null;
  last_reconnect_at: string | null;
  last_message_at: string | null;
  last_message_age_ms: number | null;
  frames: number;
  data_frames: number;
  messages: number;
  protocol_errors: number;
  last_error: FeedError | null;
  recent_errors: FeedError[];
  credentials_configured: boolean;
  transitions: { at: string; from_state: FeedState; to_state: FeedState; detail: string }[];
}

export interface MarketStatus {
  activity: MarketActivity;
  last_observation_at: string | null;
  last_observation_age_ms: number | null;
  stale_after_ms: number;
  session: string;
  note: string;
}

export interface PipelineStatus {
  inbox_depth: number;
  inbox_capacity: number;
  inbox_high_water: number;
  dropped_frames: number;
  dropped_messages: number;
  batches: number;
  frames: number;
  messages: number;
  events: number;
  rejected: number;
  duplicates: number;
  out_of_order: number;
  unsupported: Record<string, number>;
  recent_rejects: { at: string; reason: string; symbol?: string | null; type?: string }[];
  messages_per_s: number;
  events_per_s: number;
  flush_interval_ms: number;
  local_latency_us: { p50: number | null; p99: number | null; max: number | null; samples: number };
  native_ns_per_message: { p50: number | null; p99: number | null; samples: number };
}

export interface ClientsStatus {
  connected: number;
  capacity: number;
  queue_capacity: number;
  queue_depth_max: number;
  queue_high_water: number;
  dropped_messages: number;
  dropped_events: number;
  overflows: number;
  slow_disconnects: number;
  refused: number;
}

export interface RecordingSymbol {
  records: number;
  session_id: string | null;
  status: string;
  error_code: string | null;
}

export interface RecordingStatus {
  active: boolean;
  id: string | null;
  started_at: string | null;
  upstream: "up" | "down" | null;
  gaps: number;
  progress: {
    frames: number;
    messages: number;
    bytes: number;
    max_bytes: number;
    max_messages: number;
    elapsed_ms: number;
  } | null;
  finalizing: string[];
  latest: {
    id: string;
    feed: string;
    status: "recording" | "finalizing" | "completed" | "failed";
    created_at: string;
    finished_at: string | null;
    stop_reason: string | null;
    error_code: string | null;
    sessions: Record<string, RecordingSymbol>;
    coverage_gaps: number;
    accepted_records: number | null;
  }[];
}

export interface LiveStatus {
  feed: FeedStatus;
  market: MarketStatus;
  pipeline: PipelineStatus;
  clients: ClientsStatus;
  recording: RecordingStatus;
  clock: { server_wall_ns: string; note: string };
}

interface Envelope {
  v: number;
  stream: string;
  seq: number;
  server_wall_ns: string;
}

export interface HelloMessage extends Envelope {
  type: "hello";
  protocol: number;
  limits: { client_queue_messages: number; recent_events_per_symbol: number; heartbeat_ms: number };
}

export interface SnapshotMessage extends Envelope {
  type: "snapshot";
  status: LiveStatus;
  states: Record<string, SymbolState>;
  recent: LiveEvent[];
  watchlist: string[];
}

export interface EventsMessage extends Envelope {
  type: "events";
  events: LiveEvent[];
  states: Record<string, SymbolState>;
  rejects: number;
}

export interface StatusMessage extends Envelope {
  type: "status";
  status: LiveStatus;
  states: Record<string, SymbolState>;
}

export interface GapMessage extends Envelope {
  type: "gap";
  reason: "slow_client" | "pipeline_overflow";
  scope: "this_client" | "all_clients";
  dropped_messages: number;
  dropped_events?: number;
}

export interface PongMessage extends Envelope {
  type: "pong";
}

export type LiveMessage =
  HelloMessage | SnapshotMessage | EventsMessage | StatusMessage | GapMessage | PongMessage;
