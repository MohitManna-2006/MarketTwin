import { EmptyState, LoadingState } from "@/components/ui/States";
import type { BookSnapshot } from "@/lib/api/types";
import { cx } from "@/lib/cx";
import { formatElapsedAuto, formatInteger, formatTwiceTicks } from "@/lib/formatting";

export interface OrderBookLadderProps {
  /** The recorded snapshot to show; null when none exists at or before the cursor. */
  book: BookSnapshot | null;
  /** Levels per side the run recorded. */
  depth: number | null;
  isPending?: boolean;
}

interface Level {
  price: number;
  quantity: number;
  orders: number;
  cumulative: number;
}

function levels(side: number[][]): Level[] {
  let cumulative = 0;
  return side.map(([price = 0, quantity = 0, orders = 0]) => {
    cumulative += quantity;
    return { price, quantity, orders, cumulative };
  });
}

function Row({ level, side, scale }: { level: Level; side: "ask" | "bid"; scale: number }) {
  const width = `${Math.max(1.5, (level.cumulative / scale) * 100)}%`;
  return (
    <div
      role="row"
      className="relative grid h-[22px] grid-cols-[52px_1fr_1fr] items-center px-3 text-sm"
    >
      <span
        aria-hidden
        className={cx(
          "absolute inset-y-px right-0",
          side === "ask" ? "bg-neg-wash" : "bg-pos-wash",
        )}
        style={{ width }}
      />
      <span role="cell" className="num relative text-right text-fg-3">
        {level.orders}
      </span>
      <span role="cell" className="num relative text-right text-fg">
        {formatInteger(level.quantity)}
      </span>
      <span
        role="cell"
        className={cx(
          "num relative text-right font-medium",
          side === "ask" ? "text-neg" : "text-pos",
        )}
      >
        {formatInteger(level.price)}
      </span>
    </div>
  );
}

/**
 * A depth ladder of one recorded snapshot: asks above, bids below, best prices at the
 * spread. The bars are cumulative resting quantity from the touch outward. The ladder
 * shows exactly one snapshot and names its time; it never blends two.
 */
export function OrderBookLadder({ book, depth, isPending }: OrderBookLadderProps) {
  if (isPending) return <LoadingState label="Loading depth" />;
  if (!book) {
    return (
      <EmptyState title="No depth snapshot here">
        No snapshot was recorded at or before the cursor. Depth exists only at the run&apos;s
        sampling times.
      </EmptyState>
    );
  }
  const asks = levels(book.asks);
  const bids = levels(book.bids);
  const scale = Math.max(
    1,
    asks[asks.length - 1]?.cumulative ?? 0,
    bids[bids.length - 1]?.cumulative ?? 0,
  );
  const bestAsk = asks[0];
  const bestBid = bids[0];
  const twoSided = bestAsk !== undefined && bestBid !== undefined;

  return (
    <div className="flex h-full flex-col" role="table" aria-label="Order book depth at the cursor">
      <div
        role="row"
        className="grid h-6 shrink-0 grid-cols-[52px_1fr_1fr] items-center border-b border-line bg-surface-1 px-3"
      >
        <span role="columnheader" className="label text-right text-fg-3">
          Orders
        </span>
        <span role="columnheader" className="label text-right text-fg-3">
          Quantity <span className="font-normal tracking-normal text-fg-3 normal-case">shares</span>
        </span>
        <span role="columnheader" className="label text-right text-fg-3">
          Price <span className="font-normal tracking-normal text-fg-3 normal-case">ticks</span>
        </span>
      </div>
      <div className="flex min-h-0 flex-1 flex-col overflow-auto">
        <div role="rowgroup" aria-label="Asks" className="flex flex-1 flex-col justify-end">
          {asks.length === 0 ? (
            <p className="label px-3 py-2 text-right text-fg-3">No asks resting</p>
          ) : (
            asks
              .slice()
              .reverse()
              .map((level) => <Row key={level.price} level={level} side="ask" scale={scale} />)
          )}
        </div>
        <div className="grid h-7 shrink-0 grid-cols-[1fr_auto_1fr] items-center gap-3 border-y border-line bg-surface-1 px-3">
          <span className="label text-fg-3">
            Spread{" "}
            <span className="num text-sm font-medium tracking-normal text-fg">
              {twoSided ? formatInteger(bestAsk.price - bestBid.price) : "—"}
            </span>
            {twoSided ? (
              <span className="font-normal tracking-normal normal-case"> ticks</span>
            ) : null}
          </span>
          <span className="label text-fg-3" aria-hidden>
            ·
          </span>
          <span className="label text-right text-fg-3">
            Mid{" "}
            <span className="num text-sm font-medium tracking-normal text-accent">
              {twoSided ? formatTwiceTicks(bestAsk.price + bestBid.price, { fixed: true }) : "—"}
            </span>
          </span>
        </div>
        <div role="rowgroup" aria-label="Bids" className="flex flex-1 flex-col">
          {bids.length === 0 ? (
            <p className="label px-3 py-2 text-right text-fg-3">No bids resting</p>
          ) : (
            bids.map((level) => <Row key={level.price} level={level} side="bid" scale={scale} />)
          )}
        </div>
      </div>
      <div className="flex h-6 shrink-0 items-center gap-2 overflow-hidden border-t border-line-subtle bg-surface-1 px-3 whitespace-nowrap">
        <span className="label text-fg-3">Snapshot</span>
        <span className="num text-xs text-fg-2">{formatElapsedAuto(book.timestamp_ns)}</span>
        <span className="flex-1" />
        <span className="label text-fg-3">
          Seq <span className="num tracking-normal text-fg-2">{formatInteger(book.sequence)}</span>
        </span>
        {depth ? (
          <span className="label text-fg-3">
            <span className="num tracking-normal text-fg-2">{depth}</span> levels / side
          </span>
        ) : null}
      </div>
    </div>
  );
}
