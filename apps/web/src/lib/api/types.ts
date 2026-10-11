// API types. The shapes are generated from the service's OpenAPI document; the names of
// the recorded columns, which OpenAPI describes only as "a map of arrays", are added here.

import type { components } from "@/types/api";

type Schemas = components["schemas"];

export type Health = Schemas["Health"];
export type ScenarioSummary = Schemas["ScenarioSummary"];
export type ScenarioDetail = Schemas["ScenarioDetail"];
export type FieldSpec = Schemas["FieldSpec"];
export type FieldCatalogue = Schemas["FieldCatalogue"];
export type PolicySpec = Schemas["PolicySpec"];
export type ShockSpec = Schemas["ShockSpec"];
export type ConfigError = Schemas["ConfigError"];
export type RunComplexity = Schemas["RunComplexity"];
export type RunValidation = Schemas["RunValidation"];
export type ExperimentValidation = Schemas["ExperimentValidation"];
export type RunSummary = Schemas["RunSummary"];
export type RunList = Schemas["RunList"];
export type RunDetail = Schemas["RunDetail"];
export type RunStatus = RunSummary["status"];
export type ReplayStatus = RunSummary["replay_status"];
export type ReplayView = Schemas["ReplayView"];
export type AgentView = Schemas["AgentView"];
export type ShockView = Schemas["ShockView"];
export type ArtifactFile = Schemas["ArtifactFile"];
export type CellProvenance = Schemas["CellProvenance"];
export type SampleTimes = Schemas["SampleTimes"];
export type BookSnapshot = Schemas["BookSnapshot"];
export type TradeRow = Schemas["TradeRow"];
export type TradePage = Schemas["TradePage"];
export type LedgerRow = Schemas["LedgerRow"];
export type LedgerPage = Schemas["LedgerPage"];
export type EventRow = Schemas["EventRow"];
export type EventPage = Schemas["EventPage"];
export type ExperimentSummary = Schemas["ExperimentSummary"];
export type ExperimentList = Schemas["ExperimentList"];
export type ExperimentDetail = Schemas["ExperimentDetail"];
export type ExperimentStatus = ExperimentSummary["status"];
export type CellRun = Schemas["CellRun"];

export interface HistoricalProvider {
  id: "local_file" | "alpaca";
  configured: boolean;
  feeds: string[];
  coverage: string;
  cost: string;
}

export interface HistoricalQuality {
  status: "ok" | "warnings";
  records_imported: number;
  trade_records: number;
  quote_records: number;
  traded_quantity: number;
  duplicates: number;
  out_of_order: number;
  locked_quotes: number;
  crossed_quotes: number;
  gaps_over_five_minutes: number;
  gap_markers: {
    kind: "trade" | "quote";
    from_timestamp_ns: string;
    to_timestamp_ns: string;
    duration_ns: string;
  }[];
}

export interface HistoricalMetrics {
  trade_count: number;
  traded_quantity: number;
  last_trade_micro_usd: number | null;
  high_trade_micro_usd: number | null;
  low_trade_micro_usd: number | null;
  vwap_micro_usd: number | null;
  quote_update_count: number;
  trade_arrival_rate_millihertz: number | null;
  quote_update_rate_millihertz: number | null;
  mean_quoted_spread_micro_usd: number | null;
  coverage_ns: number;
  trade_to_trade_return_observations: number;
  trade_to_trade_return_volatility_ppb: number | null;
  quote_state_time: Record<string, number>;
  methodology: Record<string, string>;
  units: Record<string, string>;
  scope: string;
}

export interface HistoricalImport {
  id: string;
  provider: string;
  feed: string;
  symbol: string;
  start_time: string | null;
  end_time: string | null;
  status: "pending" | "running" | "completed" | "failed";
  created_at: string;
  started_at: string | null;
  finished_at: string | null;
  manifest: Record<string, unknown> | null;
  error_code: string | null;
  error_message: string | null;
}

export interface HistoricalSession extends HistoricalImport {
  observation_kind: "REAL HISTORICAL DATA" | "FABRICATED TEST DATA";
  quality: HistoricalQuality;
  metrics: HistoricalMetrics;
}

export interface HistoricalEvent {
  sequence: number;
  kind: "trade" | "quote";
  timestamp_ns: string;
  source_timestamp: string;
  venue: string;
  price_micro_usd?: number;
  quantity?: number;
  trade_id?: string | number | null;
  bid_micro_usd?: number;
  ask_micro_usd?: number;
  bid_size?: number;
  ask_size?: number;
}

export interface HistoricalSample {
  sequence: number;
  timestamp_ns: string;
  last_trade_micro_usd: number | null;
  bid_micro_usd: number | null;
  ask_micro_usd: number | null;
  bid_size: number | null;
  ask_size: number | null;
  quote_state: string;
}

export interface HistoricalPage<T> {
  session_id: string;
  offset: number;
  records: T[];
  next_offset: number | null;
}

export interface HistoricalComparison {
  synthetic_run_id: string;
  historical_session_id: string;
  purpose: string;
  metrics: {
    name: string;
    synthetic: number | null;
    historical: number | null;
    unit: string;
    compatible: boolean;
    note: string;
  }[];
  limitations: string[];
}

export type Column = (number | null)[];

export const MARKET_COLUMNS = [
  "timestamp_ns",
  "sequence",
  "bid_price_ticks",
  "bid_quantity",
  "ask_price_ticks",
  "ask_quantity",
  "spread_ticks",
  "bid_depth",
  "ask_depth",
  "depth_levels",
  "mark_twice_ticks",
  "mark_source",
  "fundamental_milliticks",
  "trade_count",
  "volume",
] as const;
export type MarketColumn = (typeof MARKET_COLUMNS)[number];

export const AGENT_COLUMNS = [
  "cash_minor",
  "position",
  "cost_basis_minor",
  "realized_pnl_minor",
  "unrealized_pnl_minor",
  "equity_minor",
  "cum_bought",
  "cum_sold",
] as const;
export type AgentColumn = (typeof AGENT_COLUMNS)[number];

export type MarketSeries = Omit<Schemas["MarketSeries"], "columns"> & {
  columns: Record<MarketColumn | "mid_twice_ticks", Column>;
};

export type AgentSeries = Omit<Schemas["AgentSeries"], "columns"> & {
  columns: Record<AgentColumn | "timestamp_ns" | "drawdown_minor", Column>;
};

export type SampleWindow = Omit<Schemas["SampleWindow"], "columns" | "agents"> & {
  columns: Record<MarketColumn, Column>;
  agents: Record<string, Record<AgentColumn, Column>>;
};

/** One agent's entry in metrics.json, verbatim. A value the run did not define is null. */
export interface AgentReport {
  agent_id: number;
  policy: string;
  bought_quantity: number;
  sold_quantity: number;
  canceled_quantity: number;
  cancels_requested: number;
  cancels_succeeded: number;
  cash_minor: number;
  cash_refusals: number;
  cost_basis_minor: number;
  credit_limit_minor: number | null;
  equity_minor: number | null;
  equity_observations: number;
  expired_quantity: number;
  fees_paid_minor: number;
  fill_rate_ppm: number | null;
  fills: number;
  gross_notional_minor: number;
  initial_cash_minor: number;
  maker_fills: number;
  maker_quantity: number;
  maker_spread_capture_minor: number;
  market_value_minor: number | null;
  max_abs_position: number;
  max_drawdown_minor: number | null;
  mean_abs_position_millishares: number | null;
  min_cash_minor: number;
  orders_accepted: number;
  orders_rejected: number;
  orders_submitted: number;
  peak_equity_minor: number | null;
  position: number;
  realized_pnl_minor: number;
  resting_quantity: number;
  submitted_quantity: number;
  taker_fills: number;
  taker_quantity: number;
  taker_spread_cost_minor: number;
  total_pnl_minor: number | null;
  unrealized_pnl_minor: number | null;
}

/** The market section of metrics.json, verbatim. */
export interface MarketMetrics {
  trade_count: number;
  volume: number;
  vwap_milliticks: number | null;
  last_trade_price_ticks: number | null;
  high_trade_price_ticks: number | null;
  low_trade_price_ticks: number | null;
  orders_submitted: number;
  orders_accepted: number;
  orders_rejected: number;
  rejections: Record<string, number>;
  cancels_requested: number;
  cancels_succeeded: number;
  submitted_quantity: number;
  filled_quantity: number;
  expired_quantity: number;
  canceled_quantity: number;
  fill_rate_ppm: number | null;
  total_ns: number;
  two_sided_ns: number;
  bid_only_ns: number;
  ask_only_ns: number;
  empty_ns: number;
  samples: number;
  two_sided_samples: number;
  mean_spread_milliticks: number | null;
  mean_relative_spread_millibps: number | null;
  mean_best_bid_quantity_millishares: number | null;
  mean_best_ask_quantity_millishares: number | null;
  depth_levels: number | null;
  mean_bid_depth_millishares: number | null;
  mean_ask_depth_millishares: number | null;
  return_observations: number;
  midpoint_return_volatility_ppb: number | null;
  traded_notional_minor: number;
}

export interface RunMetrics {
  agents: AgentReport[];
  market: MarketMetrics;
  final_mark: { source: string; twice_price_ticks: number | null };
  fees: { maker_ppm: number; taker_ppm: number };
  units: Record<string, string | number>;
  mark_policy: string[];
  accounting_basis: string;
  rounding: string;
}

export interface Interval2 {
  0: number;
  1: number;
}

/** A summary of one sample as the core reports it, in thousandths of the metric's unit. */
export interface StatSummary {
  count: number;
  minimum: number;
  maximum: number;
  mean_milli: number;
  median_milli: number;
  standard_deviation_milli: number | null;
  mean_t_interval_milli: [number, number] | null;
  mean_bootstrap_interval_milli: [number, number] | null;
}

export interface MetricComparison {
  name: string;
  unit: string;
  scope: "subject" | "market";
  pairs: number;
  pairs_missing: number;
  baseline: StatSummary | null;
  candidate: StatSummary | null;
  difference: StatSummary | null;
}

export interface ExperimentRunRecord {
  variant: string;
  seed: number;
  run_id: string;
  status: "Completed" | "Failed";
  error_code: string | null;
  error_message: string | null;
  artifacts: string | null;
  stop_reason: string | null;
  event_count: number | null;
  trade_count: number | null;
  final_time_ns: number | null;
  config_digest: string | null;
  commands_digest: string | null;
  trace_digest: string | null;
  book_digest: string | null;
  trade_digest: string | null;
  accounting_digest: string | null;
  metrics_digest: string | null;
}

export interface ExperimentResultsDocument {
  schema_version: number;
  engine_version: string;
  hash_algorithm: string;
  provenance: { git_revision: string; compiler: string; build_type: string };
  results_digest: string;
  artifacts: Record<string, string>;
  results: {
    config: ExperimentConfig;
    config_digest: string;
    metrics_schema_version: number;
    design: { seeds: number; variants: number; total_runs: number; pairing: string };
    statistics: {
      scale: string;
      rounding: string;
      confidence_level_ppm: number;
      t_interval: string;
      bootstrap_interval: string;
      rng_algorithm: string;
    };
    counts: { variant: string; completed: number; failed: number }[];
    runs: ExperimentRunRecord[];
    comparison: {
      baseline: string;
      candidate: string;
      subject_agent_id: number;
      difference: string;
      pairs_completed: number;
      metrics: MetricComparison[];
    } | null;
  };
}

export interface PairedValuesDocument {
  baseline: string | null;
  candidate: string | null;
  subject_agent_id: number | null;
  seeds: number[];
  metrics: {
    name: string;
    unit: string;
    scope: string;
    baseline: (number | null)[];
    candidate: (number | null)[];
    difference: (number | null)[];
  }[];
}

// ---- configs, as the core's schema defines them --------------------------------------

export interface PolicyConfig {
  kind: string;
  [parameter: string]: number | string;
}

export interface AgentConfig {
  id: number;
  wakeup: { first_ns: number; interval_ns: number; distribution: "Fixed" | "Exponential" };
  latency: {
    order_entry_ns: number;
    order_entry_jitter_ns: number;
    report_ns: number;
    market_data_ns: number;
  };
  limits: {
    max_position: number;
    max_order_quantity: number;
    max_open_orders: number;
    max_messages: number;
  };
  policy: PolicyConfig;
}

export interface ShockConfig {
  kind: "FundamentalJump" | "LiquidityWithdrawal" | "VolatilityRegime";
  at_ns?: number;
  delta_milliticks?: number;
  start_ns?: number;
  end_ns?: number;
  agents?: number[];
  quote_size_ppm?: number;
  volatility_ppm?: number;
}

export interface ScenarioConfig {
  reference_price_ticks: number;
  fundamental: {
    initial_milliticks: number;
    update_interval_ns: number;
    volatility_milliticks: number;
    mean_reversion_ppm: number;
  };
  sampling: { interval_ns: number; depth: number } | null;
  agents: AgentConfig[];
  shocks: ShockConfig[];
}

export interface AccountingConfig {
  fees: { maker_ppm: number; taker_ppm: number };
  default_initial_cash_minor: number;
  accounts: { agent_id: number; initial_cash_minor: number; credit_limit_minor: number | null }[];
}

export interface RunConfig {
  schema_version: 1 | 2 | 3;
  run_id: string;
  seed: number;
  tick_size_minor: number;
  exchange: {
    symbol: number;
    agents: number[];
    max_order_quantity: number;
    max_active_orders: number;
    max_accepted_orders: number;
    max_trades: number;
    max_snapshot_depth: number;
  };
  horizon_ns: number;
  max_events: number;
  max_queue: number;
  max_trace_bytes: number;
  commands: unknown[];
  scenario?: ScenarioConfig | null;
  accounting?: AccountingConfig;
}

export interface ExperimentConfig {
  schema_version: 1;
  experiment_id: string;
  base_config: RunConfig;
  horizon_ns: number | null;
  seeds: number[] | { first: number; count: number };
  variants: { name: string; policy_overrides: { agent_id: number; policy: PolicyConfig }[] }[];
  comparison: { baseline: string; candidate: string; subject_agent_id: number } | null;
  bootstrap: { resamples: number; seed: number };
  artifacts: "None" | "All";
  limits: { max_runs: number; max_output_bytes: number };
}
