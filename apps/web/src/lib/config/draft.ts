// Editing a run config without mutating it. The draft is always a complete config in the
// core's own schema; every edit produces the JSON that would be submitted.

import type { AgentConfig, RunConfig, ScenarioConfig, ShockConfig } from "@/lib/api/types";

export type Path = (string | number)[];

/** Returns a copy of `root` with `value` at `path`. Arrays and objects along it are copied. */
export function setIn<T>(root: T, path: Path, value: unknown): T {
  if (path.length === 0) return value as T;
  const [head, ...rest] = path as [string | number, ...Path];
  if (Array.isArray(root)) {
    const copy = (root as unknown[]).slice();
    copy[head as number] = setIn(copy[head as number], rest, value);
    return copy as T;
  }
  const source = (root ?? {}) as Record<string, unknown>;
  return { ...source, [head]: setIn(source[head as string], rest, value) } as T;
}

export function getIn(root: unknown, path: Path): unknown {
  let current = root;
  for (const key of path) {
    if (current === null || current === undefined) return undefined;
    current = (current as Record<string | number, unknown>)[key];
  }
  return current;
}

export function pathKey(path: Path): string {
  return path.join(".");
}

function withScenario(
  config: RunConfig,
  update: (scenario: ScenarioConfig) => ScenarioConfig,
): RunConfig {
  if (!config.scenario) return config;
  return { ...config, scenario: update(config.scenario) };
}

export function nextAgentId(config: RunConfig): number {
  return Math.max(0, ...config.exchange.agents) + 1;
}

/** Adds an agent and registers it with the exchange, which the core requires. */
export function addAgent(config: RunConfig, template: AgentConfig): RunConfig {
  const id = nextAgentId(config);
  const agent: AgentConfig = { ...structuredClone(template), id };
  return {
    ...withScenario(config, (scenario) => ({ ...scenario, agents: [...scenario.agents, agent] })),
    exchange: { ...config.exchange, agents: [...config.exchange.agents, id] },
  };
}

/** Removes an agent everywhere the config names it: exchange, shocks, and accounts. */
export function removeAgent(config: RunConfig, id: number): RunConfig {
  const next = withScenario(config, (scenario) => ({
    ...scenario,
    agents: scenario.agents.filter((agent) => agent.id !== id),
    shocks: scenario.shocks.map((shock) =>
      shock.agents ? { ...shock, agents: shock.agents.filter((agent) => agent !== id) } : shock,
    ),
  }));
  return {
    ...next,
    exchange: { ...next.exchange, agents: next.exchange.agents.filter((agent) => agent !== id) },
    ...(next.accounting
      ? {
          accounting: {
            ...next.accounting,
            accounts: next.accounting.accounts.filter((account) => account.agent_id !== id),
          },
        }
      : {}),
  };
}

/** Turns fees and cash on (schema 3) or off (schema 2, the core's defaults). */
export function setAccounting(config: RunConfig, enabled: boolean): RunConfig {
  if (enabled) {
    if (config.accounting) return config;
    return {
      ...config,
      schema_version: 3,
      scenario: config.scenario ?? null,
      accounting: {
        fees: { maker_ppm: 0, taker_ppm: 0 },
        default_initial_cash_minor: 0,
        accounts: [],
      },
    };
  }
  const { accounting: _dropped, ...rest } = config;
  if (!rest.scenario) {
    const { scenario: _scenario, ...scripted } = rest;
    return { ...scripted, schema_version: 1 };
  }
  return { ...rest, schema_version: 2 };
}

/** Which agents a shock may name, and how the editor labels the choice. */
export function shockAgentChoice(
  kind: ShockConfig["kind"],
): { legend: string; eligible: (policy: string) => boolean } | null {
  switch (kind) {
    case "LiquidityWithdrawal":
      return {
        legend: "Market makers withdrawn",
        eligible: (policy) => policy.endsWith("MarketMaker"),
      };
    case "OrderFlowImbalance":
      return { legend: "Noise traders shifted", eligible: (policy) => policy === "Noise" };
    case "LatencyDegradation":
      return { legend: "Agents slowed", eligible: () => true };
    case "QuoteWithdrawalStress":
      return { legend: "Agents that cancel", eligible: () => true };
    default:
      return null;
  }
}

export function defaultShock(kind: ShockConfig["kind"], config: RunConfig): ShockConfig {
  const horizon = config.horizon_ns;
  const third = Math.floor(horizon / 3);
  const agents = config.scenario?.agents ?? [];
  const ids = (policy: (kind: string) => boolean) =>
    agents.filter((agent) => policy(agent.policy.kind)).map((agent) => agent.id);
  switch (kind) {
    case "FundamentalJump":
      return { kind, at_ns: Math.floor(horizon / 2), delta_milliticks: -150_000 };
    case "LiquidityWithdrawal":
      return {
        kind,
        start_ns: third,
        end_ns: Math.floor(horizon / 2),
        agents: (config.scenario?.agents ?? [])
          .filter((agent) => agent.policy.kind === "MarketMaker")
          .map((agent) => agent.id),
        quote_size_ppm: 0,
      };
    case "VolatilityRegime":
      return { kind, start_ns: third, end_ns: 2 * third, volatility_ppm: 4_000_000 };
    case "OrderFlowImbalance":
      return {
        kind,
        start_ns: third,
        end_ns: Math.floor(horizon / 2),
        agents: ids((policy) => policy === "Noise"),
        buy_probability_ppm: 800_000,
        market_order_ppm: 600_000,
        arrival_rate_ppm: 2_000_000,
      };
    case "LatencyDegradation":
      return {
        kind,
        start_ns: third,
        end_ns: Math.floor(horizon / 2),
        agents: ids((policy) => policy.endsWith("MarketMaker")),
        added_order_entry_ns: 50_000_000,
        added_report_ns: 20_000_000,
        added_market_data_ns: 50_000_000,
      };
    case "QuoteWithdrawalStress":
      return {
        kind,
        at_ns: Math.floor(horizon / 2),
        agents: ids((policy) => policy.endsWith("MarketMaker")),
      };
  }
}

/** The run name the core accepts: 1 to 128 of A-Z a-z 0-9 _ -. */
export function isValidName(name: string, max = 128): boolean {
  return name.length >= 1 && name.length <= max && /^[A-Za-z0-9_-]+$/.test(name);
}
