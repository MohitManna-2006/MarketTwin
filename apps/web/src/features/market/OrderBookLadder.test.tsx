import { render, screen, within } from "@testing-library/react";
import { describe, expect, it } from "vitest";

import type { BookSnapshot } from "@/lib/api/types";

import { OrderBookLadder } from "./OrderBookLadder";

// The first depth snapshot of the shipped baseline run, as recorded in its trace.
const snapshot: BookSnapshot = {
  timestamp_ns: 1_000_000_000,
  sequence: 57,
  bids: [
    [14999, 10, 1],
    [14998, 200, 2],
    [14997, 350, 3],
  ],
  asks: [
    [15002, 100, 1],
    [15003, 200, 2],
    [15004, 350, 3],
  ],
};

describe("OrderBookLadder", () => {
  it("shows the recorded levels with asks above bids and the best prices at the spread", () => {
    render(<OrderBookLadder book={snapshot} depth={5} />);
    const asks = within(screen.getByRole("rowgroup", { name: "Asks" })).getAllByRole("row");
    const bids = within(screen.getByRole("rowgroup", { name: "Bids" })).getAllByRole("row");
    expect(asks.map((row) => within(row).getAllByRole("cell")[2]?.textContent)).toEqual([
      "15,004",
      "15,003",
      "15,002",
    ]);
    expect(bids.map((row) => within(row).getAllByRole("cell")[2]?.textContent)).toEqual([
      "14,999",
      "14,998",
      "14,997",
    ]);
    expect(
      within(bids[1] as HTMLElement)
        .getAllByRole("cell")
        .map((cell) => cell.textContent),
    ).toEqual(["2", "200", "14,998"]);
  });

  it("prints the spread and a half-tick midpoint exactly", () => {
    render(<OrderBookLadder book={snapshot} depth={5} />);
    expect(screen.getByText("Spread").textContent).toContain("3");
    expect(screen.getByText("15,000.5")).toBeInTheDocument();
  });

  it("names the snapshot it shows: its own time, sequence, and depth", () => {
    render(<OrderBookLadder book={snapshot} depth={5} />);
    expect(screen.getByText("00:00:01.000")).toBeInTheDocument();
    expect(screen.getByText("57")).toBeInTheDocument();
    expect(screen.getByText(/levels \/ side/)).toHaveTextContent("5 levels / side");
  });

  it("says a side is empty and quotes no spread or midpoint for a one-sided book", () => {
    render(<OrderBookLadder book={{ ...snapshot, asks: [] }} depth={5} />);
    expect(screen.getByText("No asks resting")).toBeInTheDocument();
    expect(screen.queryByText("15,000.5")).not.toBeInTheDocument();
    expect(screen.getByText("Mid").textContent).toContain("—");
  });

  it("explains the absence of a snapshot instead of drawing an empty book", () => {
    render(<OrderBookLadder book={null} depth={5} />);
    expect(screen.getByText("No depth snapshot here")).toBeInTheDocument();
    expect(screen.queryByRole("table")).not.toBeInTheDocument();
  });
});
