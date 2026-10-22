import { render, screen } from "@testing-library/react";
import { MemoryRouter } from "react-router";
import { describe, expect, it } from "vitest";

import type { Timeline, TimelineNode } from "@/lib/api/types";

import { TimelineTree, changeSummary } from "./TimelineTree";

const MINUTE = 60_000_000_000;

function node(overrides: Partial<TimelineNode>): TimelineNode {
  return {
    id: "run-root",
    name: "root",
    status: "completed",
    source_kind: "user",
    parent_run_id: null,
    fork_at_ns: null,
    fork_status: null,
    title: null,
    stage: null,
    desk: null,
    created_at: "2026-10-22T00:00:00.000Z",
    horizon_ns: 60 * MINUTE,
    divergence_ns: null,
    prefix_records: null,
    ...overrides,
  };
}

describe("changeSummary", () => {
  it("names what a fork changed, or its title", () => {
    expect(changeSummary(node({}))).toBe("root run");
    expect(
      changeSummary(
        node({
          parent_run_id: "run-root",
          stage: {
            at_ns: MINUTE,
            add_shocks: [{ kind: "QuoteWithdrawalStress" }],
            policy_overrides: [{ agent_id: 3, policy: { kind: "Momentum" } }],
            decisions: "live",
          },
        }),
      ),
    ).toBe("+ QuoteWithdrawalStress, agent 3 → Momentum, decisions live");
    expect(changeSummary(node({ parent_run_id: "run-root", title: "pull quotes" }))).toBe(
      "pull quotes",
    );
  });
});

describe("TimelineTree", () => {
  it("draws one linked row per run, the shown one marked", () => {
    const timeline: Timeline = {
      root_id: "run-root",
      focus_id: "run-fork",
      label: "ONE SAMPLE PATH",
      nodes: [
        node({}),
        node({
          id: "run-fork",
          name: "fork",
          parent_run_id: "run-root",
          fork_at_ns: 20 * MINUTE,
          fork_status: "verified",
          title: "pull quotes",
          divergence_ns: 20 * MINUTE,
        }),
      ],
    };
    render(
      <MemoryRouter>
        <TimelineTree timeline={timeline} />
      </MemoryRouter>,
    );
    expect(screen.getByRole("group", { name: /Tree of 2 timelines/ })).toBeInTheDocument();
    expect(screen.getByRole("link", { name: "root: root run" })).toBeInTheDocument();
    expect(screen.getByRole("link", { name: "fork: pull quotes, shown" })).toBeInTheDocument();
  });
});
