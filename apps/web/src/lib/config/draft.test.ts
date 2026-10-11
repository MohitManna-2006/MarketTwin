import { describe, expect, it } from "vitest";

import type { AgentConfig, RunConfig } from "@/lib/api/types";

import { addAgent, isValidName, removeAgent, setAccounting, setIn } from "./draft";

const maker = (id: number): AgentConfig => ({
  id,
  wakeup: { first_ns: 0, interval_ns: 500_000_000, distribution: "Fixed" },
  latency: {
    order_entry_ns: 200_000,
    order_entry_jitter_ns: 50_000,
    report_ns: 100_000,
    market_data_ns: 100_000,
  },
  limits: {
    max_position: 2000,
    max_order_quantity: 200,
    max_open_orders: 16,
    max_messages: 1_000_000,
  },
  policy: {
    kind: "MarketMaker",
    half_spread_ticks: 2,
    quote_size: 100,
    levels: 3,
    level_spacing_ticks: 1,
  },
});

const config = (): RunConfig => ({
  schema_version: 2,
  run_id: "draft",
  seed: 42,
  tick_size_minor: 1,
  exchange: {
    symbol: 1,
    agents: [1, 2],
    max_order_quantity: 1_000_000,
    max_active_orders: 100_000,
    max_accepted_orders: 5_000_000,
    max_trades: 5_000_000,
    max_snapshot_depth: 1000,
  },
  horizon_ns: 3_600_000_000_000,
  max_events: 20_000_000,
  max_queue: 100_000,
  max_trace_bytes: 1_073_741_824,
  commands: [],
  scenario: {
    reference_price_ticks: 15_000,
    fundamental: {
      initial_milliticks: 15_000_000,
      update_interval_ns: 1e9,
      volatility_milliticks: 1000,
      mean_reversion_ppm: 200,
    },
    sampling: { interval_ns: 1e9, depth: 5 },
    agents: [maker(1), maker(2)],
    shocks: [
      { kind: "LiquidityWithdrawal", start_ns: 1, end_ns: 2, agents: [1, 2], quote_size_ppm: 0 },
    ],
  },
});

describe("editing a run config", () => {
  it("copies along the edited path and shares everything else", () => {
    const before = config();
    const after = setIn(before, ["scenario", "agents", 1, "policy", "levels"], 5);
    expect(after.scenario?.agents[1]?.policy.levels).toBe(5);
    expect(before.scenario?.agents[1]?.policy.levels).toBe(3);
    expect(after.scenario?.agents[0]).toBe(before.scenario?.agents[0]);
    expect(after.exchange).toBe(before.exchange);
  });

  it("registers a new agent with the exchange under the next free ID", () => {
    const after = addAgent(config(), maker(1));
    expect(after.exchange.agents).toEqual([1, 2, 3]);
    expect(after.scenario?.agents.map((agent) => agent.id)).toEqual([1, 2, 3]);
  });

  it("removes an agent everywhere the config names it", () => {
    const withAccounts = setAccounting(config(), true);
    const edited = setIn(
      withAccounts,
      ["accounting", "accounts"],
      [{ agent_id: 2, initial_cash_minor: 5, credit_limit_minor: null }],
    );
    const after = removeAgent(edited, 2);
    expect(after.exchange.agents).toEqual([1]);
    expect(after.scenario?.agents.map((agent) => agent.id)).toEqual([1]);
    expect(after.scenario?.shocks[0]?.agents).toEqual([1]);
    expect(after.accounting?.accounts).toEqual([]);
  });

  it("moves between config schema 2 and 3 with the accounting section", () => {
    const on = setAccounting(config(), true);
    expect(on.schema_version).toBe(3);
    expect(on.accounting).toEqual({
      fees: { maker_ppm: 0, taker_ppm: 0 },
      default_initial_cash_minor: 0,
      accounts: [],
    });
    const off = setAccounting(on, false);
    expect(off.schema_version).toBe(2);
    expect("accounting" in off).toBe(false);
  });

  it("accepts the names the engine accepts", () => {
    expect(isValidName("baseline_accounting-2")).toBe(true);
    expect(isValidName("")).toBe(false);
    expect(isValidName("has space")).toBe(false);
    expect(isValidName("x".repeat(49), 48)).toBe(false);
  });
});
