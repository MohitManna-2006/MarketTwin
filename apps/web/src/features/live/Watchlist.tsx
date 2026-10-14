import { X } from "lucide-react";
import { useState } from "react";

import { Button } from "@/components/ui/Button";
import { StatusIndicator } from "@/components/ui/StatusIndicator";
import { cx } from "@/lib/cx";
import { UNAVAILABLE } from "@/lib/formatting";

import { AVAILABILITY_STATUS, agesOf, availabilityOf } from "./derive";
import { formatAge, formatMicroUsd } from "./format";
import type { LiveModel } from "./liveModel";

const SYMBOL = /^[A-Z][A-Z.]{0,11}$/;

export interface WatchlistProps {
  model: LiveModel;
  now: number;
  selected: string | null;
  onSelect: (symbol: string) => void;
  /** Absent when the feed's symbols are fixed, as on the test stream. */
  onChange?: (symbols: string[]) => void;
  busy?: boolean;
  maxSymbols?: number;
}

/** One row per subscribed instrument: last price, spread, age, and whether it is updating. */
export function Watchlist({
  model,
  now,
  selected,
  onSelect,
  onChange,
  busy,
  maxSymbols,
}: WatchlistProps) {
  const [draft, setDraft] = useState("");
  const symbols = model.watchlist;
  const candidate = draft.trim().toUpperCase();
  const draftError =
    candidate === ""
      ? null
      : !SYMBOL.test(candidate)
        ? "1 to 12 letters or dots"
        : symbols.includes(candidate)
          ? "Already watched"
          : maxSymbols !== undefined && symbols.length >= maxSymbols
            ? `At most ${maxSymbols} symbols`
            : null;

  return (
    <div className="flex h-full min-h-0 flex-col">
      <div className="min-h-0 flex-1 overflow-auto">
        <table className="w-full border-collapse text-left text-sm">
          <thead className="sticky top-0 z-10 bg-surface-1">
            <tr className="label text-fg-3">
              <th className="border-b border-line px-3 py-1.5 font-semibold">Symbol</th>
              <th className="border-b border-line px-2 py-1.5 text-right font-semibold">Last</th>
              <th className="border-b border-line px-2 py-1.5 text-right font-semibold">Spread</th>
              <th className="border-b border-line px-2 py-1.5 text-right font-semibold">Age</th>
              <th className="border-b border-line px-2 py-1.5 font-semibold">State</th>
              {onChange ? <th className="border-b border-line" aria-label="Remove" /> : null}
            </tr>
          </thead>
          <tbody>
            {symbols.length === 0 ? (
              <tr>
                <td colSpan={6} className="p-6 text-center text-sm text-fg-3">
                  The watchlist is empty. Add a symbol to subscribe to it.
                </td>
              </tr>
            ) : null}
            {symbols.map((symbol) => {
              const state = model.states[symbol];
              const ages = agesOf(state, now - (model.stateAt.get(symbol) ?? now));
              const availability = availabilityOf(symbol, model, model.status, ages);
              const dimmed = availability !== "updating";
              return (
                <tr
                  key={symbol}
                  data-symbol={symbol}
                  data-availability={availability}
                  aria-selected={selected === symbol}
                  className={cx(
                    "border-b border-line-subtle",
                    selected === symbol ? "bg-surface-selected" : "hover:bg-surface-hover",
                  )}
                >
                  <td className="px-3 py-1.5">
                    <button
                      type="button"
                      onClick={() => onSelect(symbol)}
                      aria-label={`Inspect ${symbol}`}
                      aria-pressed={selected === symbol}
                      className="num font-medium text-fg hover:text-accent"
                    >
                      {symbol}
                    </button>
                  </td>
                  <td
                    className={cx("num px-2 py-1.5 text-right", dimmed ? "text-fg-2" : "text-fg")}
                  >
                    {formatMicroUsd(state?.last_trade?.price_micro_usd)}
                  </td>
                  <td
                    className={cx(
                      "num px-2 py-1.5 text-right",
                      state?.freshness.spread_quality === "current" && !dimmed
                        ? "text-fg"
                        : "text-fg-3",
                    )}
                    title={
                      state?.freshness.spread_quality === "stale_quote"
                        ? "From a stale quote"
                        : undefined
                    }
                  >
                    {formatMicroUsd(state?.spread_micro_usd)}
                  </td>
                  <td className="num px-2 py-1.5 text-right text-fg-2">
                    {ages.update === null ? UNAVAILABLE : formatAge(ages.update)}
                  </td>
                  <td className="px-2 py-1.5">
                    <StatusIndicator spec={AVAILABILITY_STATUS[availability]} />
                  </td>
                  {onChange ? (
                    <td className="pr-1.5 text-right">
                      <button
                        type="button"
                        disabled={busy}
                        aria-label={`Unsubscribe from ${symbol}`}
                        onClick={() => onChange(symbols.filter((item) => item !== symbol))}
                        className="transition-hover inline-flex size-5 items-center justify-center rounded-sm text-fg-3 hover:bg-surface-hover hover:text-fg"
                      >
                        <X size={12} aria-hidden />
                      </button>
                    </td>
                  ) : null}
                </tr>
              );
            })}
          </tbody>
        </table>
      </div>
      {onChange ? (
        <form
          className="flex shrink-0 items-start gap-2 border-t border-line-subtle bg-surface-1 px-3 py-2"
          onSubmit={(event) => {
            event.preventDefault();
            if (candidate === "" || draftError !== null) return;
            onChange([...symbols, candidate]);
            setDraft("");
          }}
        >
          <div className="min-w-0 flex-1">
            <input
              aria-label="Symbol to subscribe"
              aria-invalid={draftError ? true : undefined}
              value={draft}
              maxLength={12}
              autoComplete="off"
              spellCheck={false}
              placeholder="ADD SYMBOL"
              onChange={(event) => setDraft(event.target.value.toUpperCase())}
              className={cx(
                "num transition-hover h-6 w-full rounded-md border bg-canvas px-1.5 text-sm text-fg outline-none placeholder:text-fg-off focus-visible:border-focus",
                draftError ? "border-neg" : "border-line",
              )}
            />
            {draftError ? (
              <p role="alert" className="mt-0.5 text-2xs text-neg">
                {draftError}
              </p>
            ) : null}
          </div>
          <Button
            size="sm"
            type="submit"
            disabled={busy || candidate === "" || draftError !== null}
          >
            Subscribe
          </Button>
        </form>
      ) : (
        <p className="shrink-0 border-t border-line-subtle bg-surface-1 px-3 py-2 text-xs text-fg-3">
          This feed's symbols are fixed by the provider.
        </p>
      )}
    </div>
  );
}
