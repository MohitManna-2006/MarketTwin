import { cx } from "@/lib/cx";
import { UNAVAILABLE } from "@/lib/formatting";

import { formatEventTime, formatMicroUsd, type DisplayZone } from "./format";
import type { TapeRow } from "./liveModel";

const FLAG: Record<string, { label: string; className: string; title: string }> = {
  in_order: { label: "OK", className: "text-fg-3", title: "Applied to the instrument's state" },
  out_of_order: {
    label: "LATE",
    className: "text-warn",
    title: "Older than an observation already received; shown, not applied",
  },
  duplicate: {
    label: "DUP",
    className: "text-warn",
    title: "An exact repeat of a recent record; shown, not applied",
  },
};

const MAX_ROWS = 200;

/** The newest observations of one instrument, newest first, exactly as normalized. */
export function LiveTape({ rows, zone }: { rows: TapeRow[]; zone: DisplayZone }) {
  const shown = rows.slice(-MAX_ROWS).reverse();
  return (
    <table className="w-full border-collapse text-left text-xs">
      <thead className="sticky top-0 z-10 bg-surface-1">
        <tr className="label text-fg-3">
          {["Event time", "Type", "Venue", "Price", "Size", "Cond", "Stream"].map((label) => (
            <th key={label} className="border-b border-line px-2 py-1.5 font-semibold">
              {label}
            </th>
          ))}
        </tr>
      </thead>
      <tbody>
        {shown.length === 0 ? (
          <tr>
            <td colSpan={7} className="p-6 text-center text-sm text-fg-3">
              No observations have been received for this instrument.
            </td>
          </tr>
        ) : null}
        {shown.map((row) => {
          const event = row.event;
          if (!event) {
            return (
              <tr key={row.key} className="border-b border-line-subtle bg-surface-1">
                <td colSpan={7} className="label px-2 py-1 text-warn">
                  Gap · {row.gap?.reason}
                  {row.gap && row.gap.dropped > 0
                    ? ` · ${row.gap.dropped.toLocaleString("en-US")} observations not delivered`
                    : " · observations may be missing here"}
                </td>
              </tr>
            );
          }
          const flag = FLAG[event.classification];
          return (
            <tr
              key={row.key}
              data-kind={event.kind}
              className={cx(
                "border-b border-line-subtle",
                event.applied ? "text-fg-2" : "text-fg-3",
              )}
            >
              <td className="num px-2 py-1">
                {formatEventTime(event.timestamp_ns, event.timestamp_precision_digits, zone)}
              </td>
              <td className={cx("label px-2 py-1", event.kind === "trade" ? "text-fg" : "")}>
                {event.kind}
              </td>
              <td className="num px-2 py-1">
                {event.kind === "trade"
                  ? event.venue || UNAVAILABLE
                  : `${event.bid_venue || UNAVAILABLE}/${event.ask_venue || UNAVAILABLE}`}
              </td>
              <td className={cx("num px-2 py-1", event.kind === "trade" ? "text-fg" : "")}>
                {event.kind === "trade"
                  ? formatMicroUsd(event.price_micro_usd)
                  : `${formatMicroUsd(event.bid_micro_usd)} / ${formatMicroUsd(event.ask_micro_usd)}`}
              </td>
              <td className="num px-2 py-1">
                {event.kind === "trade"
                  ? event.quantity.toLocaleString("en-US")
                  : `${event.bid_size ?? UNAVAILABLE} × ${event.ask_size ?? UNAVAILABLE}`}
              </td>
              <td className="num px-2 py-1">{event.conditions.join(" ") || UNAVAILABLE}</td>
              <td className={cx("label px-2 py-1", flag?.className)} title={flag?.title}>
                {flag?.label}
              </td>
            </tr>
          );
        })}
      </tbody>
    </table>
  );
}
