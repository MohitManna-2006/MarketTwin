import { render, screen, within } from "@testing-library/react";
import userEvent from "@testing-library/user-event";
import { describe, expect, it, vi } from "vitest";

import { DataTable, type DataColumn } from "./DataTable";

interface Row {
  id: string;
  name: string;
  trades: number | null;
}

const rows: Row[] = [
  { id: "a", name: "baseline", trades: 8205 },
  { id: "b", name: "liquidity-shock", trades: 8311 },
  { id: "c", name: "failed-run", trades: null },
];

const columns: DataColumn<Row>[] = [
  { id: "name", header: "Run", cell: (row) => row.name, sortValue: (row) => row.name },
  {
    id: "trades",
    header: "Trades",
    align: "right",
    cell: (row) => (row.trades === null ? "—" : row.trades.toLocaleString("en-US")),
    sortValue: (row) => row.trades,
  },
];

function table(props: Partial<React.ComponentProps<typeof DataTable<Row>>> = {}) {
  return (
    <DataTable
      label="Runs"
      columns={columns}
      rowCount={rows.length}
      getRow={(index) => rows[index]}
      getRowId={(row) => row.id}
      {...props}
    />
  );
}

const names = () =>
  screen
    .getAllByRole("row")
    .slice(1)
    .map((row) => row.firstElementChild?.textContent);

describe("DataTable", () => {
  it("is a table with a header and one row per record", () => {
    render(table());
    const grid = screen.getByRole("table", { name: "Runs" });
    expect(
      within(grid)
        .getAllByRole("columnheader")
        .map((cell) => cell.textContent),
    ).toEqual(["Run", "Trades"]);
    expect(names()).toEqual(["baseline", "liquidity-shock", "failed-run"]);
    expect(grid).toHaveAttribute("aria-rowcount", "4");
  });

  it("selects by pointer and by arrow keys, and opens with Enter", async () => {
    const user = userEvent.setup();
    const onSelect = vi.fn();
    const onActivate = vi.fn();
    const { rerender } = render(table({ onSelect, onActivate, selectedId: null }));
    const grid = screen.getByRole("grid", { name: "Runs" });
    await user.click(screen.getByText("liquidity-shock"));
    expect(onSelect).toHaveBeenLastCalledWith(rows[1], 1);

    rerender(table({ onSelect, onActivate, selectedId: "b" }));
    expect(screen.getByRole("row", { selected: true })).toHaveTextContent("liquidity-shock");
    expect(grid).toHaveAttribute(
      "aria-activedescendant",
      screen.getByRole("row", { selected: true }).id,
    );
    grid.focus();
    await user.keyboard("{ArrowDown}");
    expect(onSelect).toHaveBeenLastCalledWith(rows[2], 2);
    await user.keyboard("{ArrowUp}");
    expect(onSelect).toHaveBeenLastCalledWith(rows[0], 0);
    await user.keyboard("{Enter}");
    expect(onActivate).toHaveBeenLastCalledWith(rows[1], 1);
  });

  it("sorts by a column and keeps a missing value last", async () => {
    const user = userEvent.setup();
    render(table({ sortable: true }));
    // Numbers sort largest first on the first click, then smallest first.
    await user.click(screen.getByRole("button", { name: /Trades/ }));
    expect(names()).toEqual(["liquidity-shock", "baseline", "failed-run"]);
    expect(screen.getByRole("columnheader", { name: /Trades/ })).toHaveAttribute(
      "aria-sort",
      "descending",
    );
    await user.click(screen.getByRole("button", { name: /Trades/ }));
    expect(names()).toEqual(["baseline", "liquidity-shock", "failed-run"]);
    expect(screen.getByRole("columnheader", { name: /Trades/ })).toHaveAttribute(
      "aria-sort",
      "ascending",
    );
  });

  it("shows a placeholder for a row that has not loaded, not a blank or a zero", () => {
    render(table({ rowCount: 4, getRow: (index) => rows[index] }));
    const pending = screen.getAllByRole("row")[4] as HTMLElement;
    expect(pending.textContent).toBe("");
    expect(pending.querySelectorAll(".skeleton")).toHaveLength(2);
  });

  it("explains an empty table", () => {
    render(table({ rowCount: 0, getRow: () => undefined, empty: <p>No simulations stored</p> }));
    expect(screen.getByText("No simulations stored")).toBeInTheDocument();
  });
});
