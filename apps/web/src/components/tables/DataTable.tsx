import {
  getCoreRowModel,
  getSortedRowModel,
  useReactTable,
  type ColumnDef,
  type SortingState,
} from "@tanstack/react-table";
import { useVirtualizer } from "@tanstack/react-virtual";
import { ArrowDown, ArrowUp } from "lucide-react";
import {
  useEffect,
  useId,
  useMemo,
  useRef,
  useState,
  type KeyboardEvent,
  type ReactNode,
} from "react";

import { cx } from "@/lib/cx";

export interface DataColumn<T> {
  id: string;
  header: string;
  /** A unit shown beside the header, such as "ticks". */
  unit?: string;
  cell: (row: T, index: number) => ReactNode;
  /** Makes the column sortable (client-side tables only). */
  sortValue?: (row: T) => number | string | null;
  align?: "left" | "right" | "center";
  /** CSS grid track, such as "96px" or "minmax(120px, 1fr)". */
  width?: string;
  title?: string;
  /** Hidden below this container width. */
  minContainer?: number;
}

export interface DataTableProps<T> {
  columns: DataColumn<T>[];
  /** Total rows. With `getRow`, rows may load lazily and outnumber what is in memory. */
  rowCount: number;
  getRow: (index: number) => T | undefined;
  getRowId: (row: T, index: number) => string;
  /** Called with the visible row range so a caller can load those rows. */
  onRangeChange?: (start: number, end: number) => void;
  selectedId?: string | null;
  onSelect?: (row: T, index: number) => void;
  onActivate?: (row: T, index: number) => void;
  /** Row the table should keep in view; changing it scrolls. */
  scrollToIndex?: number | null;
  rowClassName?: (row: T, index: number) => string | undefined;
  empty?: ReactNode;
  label: string;
  rowHeight?: number;
  sortable?: boolean;
  initialSort?: SortingState;
  className?: string;
}

const ALIGN = {
  left: "justify-start text-left",
  right: "justify-end text-right",
  center: "justify-center",
};

/**
 * The terminal's table: sticky header, fixed row height, right-aligned numerals, one
 * selected row, arrow-key navigation, and rows rendered only while visible. It reads rows
 * by index, so the same table shows a few runs or a hundred thousand recorded events.
 */
export function DataTable<T>({
  columns,
  rowCount,
  getRow,
  getRowId,
  onRangeChange,
  selectedId,
  onSelect,
  onActivate,
  scrollToIndex,
  rowClassName,
  empty,
  label,
  rowHeight = 24,
  sortable = false,
  initialSort = [],
  className,
}: DataTableProps<T>) {
  const scroller = useRef<HTMLDivElement>(null);
  const baseId = useId();
  const [sorting, setSorting] = useState<SortingState>(initialSort);
  const interactive = onSelect !== undefined;

  // Client-side sorting needs every row; lazily loaded tables stay in recorded order.
  const allRows = useMemo(() => {
    if (!sortable) return [];
    const rows: T[] = [];
    for (let index = 0; index < rowCount; index += 1) {
      const row = getRow(index);
      if (row !== undefined) rows.push(row);
    }
    return rows;
  }, [sortable, rowCount, getRow]);

  const definitions = useMemo<ColumnDef<T>[]>(
    () =>
      columns.map((column) => ({
        id: column.id,
        // A missing value is undefined to the sorter, which keeps it last either way.
        accessorFn: (row: T) => column.sortValue?.(row) ?? undefined,
        enableSorting: sortable && column.sortValue !== undefined,
        sortUndefined: "last" as const,
      })),
    [columns, sortable],
  );

  const table = useReactTable({
    data: allRows,
    columns: definitions,
    state: { sorting },
    onSortingChange: setSorting,
    getCoreRowModel: getCoreRowModel(),
    getSortedRowModel: getSortedRowModel(),
    enableSortingRemoval: true,
  });
  const sortedRows = sortable ? table.getRowModel().rows : null;
  const rowAt = (index: number): T | undefined =>
    sortedRows ? sortedRows[index]?.original : getRow(index);

  const virtualizer = useVirtualizer({
    count: rowCount,
    getScrollElement: () => scroller.current,
    estimateSize: () => rowHeight,
    overscan: 12,
    // A first measurement for environments without layout; the real size replaces it.
    initialRect: { width: 960, height: 480 },
  });
  const items = virtualizer.getVirtualItems();
  const first = items[0]?.index ?? 0;
  const last = items[items.length - 1]?.index ?? 0;

  useEffect(() => {
    if (rowCount > 0) onRangeChange?.(first, last);
  }, [first, last, rowCount, onRangeChange]);

  useEffect(() => {
    if (scrollToIndex !== null && scrollToIndex !== undefined && scrollToIndex >= 0) {
      virtualizer.scrollToIndex(scrollToIndex, { align: "center" });
    }
    // The virtualizer instance is stable; only the target row matters.
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [scrollToIndex]);

  const template = columns.map((column) => column.width ?? "minmax(72px, 1fr)").join(" ");

  function selectedIndex(): number {
    if (selectedId === null || selectedId === undefined) return -1;
    for (const item of items) {
      const row = rowAt(item.index);
      if (row !== undefined && getRowId(row, item.index) === selectedId) return item.index;
    }
    return -1;
  }

  function onKeyDown(event: KeyboardEvent<HTMLDivElement>) {
    if (!onSelect || rowCount === 0) return;
    const current = selectedIndex();
    let next: number | null = null;
    if (event.key === "ArrowDown") next = Math.min(rowCount - 1, current + 1);
    else if (event.key === "ArrowUp") next = Math.max(0, current < 0 ? 0 : current - 1);
    else if (event.key === "PageDown") next = Math.min(rowCount - 1, Math.max(0, current) + 20);
    else if (event.key === "PageUp") next = Math.max(0, Math.max(0, current) - 20);
    else if (event.key === "Enter" && current >= 0) {
      const row = rowAt(current);
      if (row !== undefined) onActivate?.(row, current);
      return;
    }
    if (next === null) return;
    event.preventDefault();
    const row = rowAt(next);
    if (row !== undefined) {
      onSelect(row, next);
      virtualizer.scrollToIndex(next, { align: "auto" });
    }
  }

  const body = (
    <>
      <div role="rowgroup" className="sticky top-0 z-10">
        <div
          role="row"
          aria-rowindex={1}
          className="grid h-6 w-max min-w-full items-stretch border-b border-line bg-surface-1"
          style={{ gridTemplateColumns: template }}
        >
          {columns.map((column) => {
            const header = table.getColumn(column.id);
            const direction = header?.getIsSorted();
            const canSort = header?.getCanSort() ?? false;
            const content = (
              <>
                <span className="truncate">{column.header}</span>
                {column.unit ? (
                  <span className="font-normal tracking-normal text-fg-3 normal-case">
                    {column.unit}
                  </span>
                ) : null}
                {direction === "asc" ? <ArrowUp size={10} aria-hidden /> : null}
                {direction === "desc" ? <ArrowDown size={10} aria-hidden /> : null}
              </>
            );
            return (
              <div
                key={column.id}
                role="columnheader"
                title={column.title}
                aria-sort={
                  direction === "asc"
                    ? "ascending"
                    : direction === "desc"
                      ? "descending"
                      : undefined
                }
                className={cx(
                  "label flex min-w-0 items-center px-2 text-fg-3",
                  ALIGN[column.align ?? "left"],
                )}
              >
                {canSort ? (
                  <button
                    type="button"
                    onClick={header?.getToggleSortingHandler()}
                    className={cx(
                      "transition-hover label flex min-w-0 items-center gap-1 hover:text-fg",
                      direction ? "text-fg" : "text-fg-3",
                    )}
                  >
                    {content}
                  </button>
                ) : (
                  <span className="flex min-w-0 items-center gap-1">{content}</span>
                )}
              </div>
            );
          })}
        </div>
      </div>
      {rowCount === 0 ? (
        <div className="absolute inset-x-0 top-6 bottom-0">{empty}</div>
      ) : (
        <div
          role="rowgroup"
          className="relative w-max min-w-full"
          style={{ height: virtualizer.getTotalSize() }}
        >
          {items.map((item) => {
            const row = rowAt(item.index);
            const id = row === undefined ? `pending-${item.index}` : getRowId(row, item.index);
            const selected = row !== undefined && id === selectedId;
            return (
              // Keyboard interaction is on the grid itself (arrow keys and Enter); a row is
              // its pointer target, not a second tab stop.
              // eslint-disable-next-line jsx-a11y/click-events-have-key-events, jsx-a11y/interactive-supports-focus
              <div
                key={item.key}
                id={`${baseId}-${id}`}
                role="row"
                aria-rowindex={item.index + 2}
                aria-selected={interactive ? selected : undefined}
                data-row-id={id}
                onClick={
                  row !== undefined && onSelect ? () => onSelect(row, item.index) : undefined
                }
                onDoubleClick={
                  row !== undefined && onActivate ? () => onActivate(row, item.index) : undefined
                }
                className={cx(
                  "absolute inset-x-0 grid items-stretch border-b border-line-subtle text-sm",
                  onSelect && "cursor-pointer",
                  selected
                    ? "bg-surface-selected shadow-[inset_2px_0_0_var(--accent-primary)]"
                    : "hover:bg-surface-hover",
                  row !== undefined && rowClassName?.(row, item.index),
                )}
                style={{
                  gridTemplateColumns: template,
                  height: rowHeight,
                  transform: `translateY(${item.start}px)`,
                }}
              >
                {columns.map((column) => (
                  <div
                    key={column.id}
                    role={interactive ? "gridcell" : "cell"}
                    className={cx("flex min-w-0 items-center px-2", ALIGN[column.align ?? "left"])}
                  >
                    {row === undefined ? (
                      <span className="skeleton h-2.5 w-2/3" aria-hidden />
                    ) : (
                      <span className="truncate">{column.cell(row, item.index)}</span>
                    )}
                  </div>
                ))}
              </div>
            );
          })}
        </div>
      )}
    </>
  );

  // A table whose rows can be selected is an ARIA grid: one tab stop, arrow keys move the
  // selection, and the selected row is announced through aria-activedescendant.
  if (interactive) {
    return (
      <div
        ref={scroller}
        role="grid"
        aria-label={label}
        aria-rowcount={rowCount + 1}
        aria-activedescendant={selectedId ? `${baseId}-${selectedId}` : undefined}
        tabIndex={0}
        onKeyDown={onKeyDown}
        className={cx(
          "relative h-full overflow-auto outline-none focus-visible:outline-1",
          className,
        )}
      >
        {body}
      </div>
    );
  }
  return (
    <div
      ref={scroller}
      role="table"
      aria-label={label}
      aria-rowcount={rowCount + 1}
      className={cx("relative h-full overflow-auto", className)}
    >
      {body}
    </div>
  );
}

/** Rows held in memory, for client-side tables. */
export function useArrayRows<T>(rows: readonly T[]) {
  return useMemo(
    () => ({ rowCount: rows.length, getRow: (index: number): T | undefined => rows[index] }),
    [rows],
  );
}
