import { Link } from "react-router";

import { KeyValue, type KeyValueRow } from "@/components/ui/KeyValue";
import { StatusIndicator } from "@/components/ui/StatusIndicator";
import { UNAVAILABLE, formatBytes } from "@/lib/formatting";

import { ACTIVITY_STATUS, FEED_STATUS, sessionShort } from "./derive";
import { formatAge, formatIsoClock, type DisplayZone } from "./format";
import type { LiveModel } from "./liveModel";

const STOP_REASON: Record<string, string> = {
  user_stopped: "stopped by the user",
  storage_limit: "storage bound reached",
  message_limit: "message bound reached",
  duration_limit: "duration bound reached",
  feed_stopped: "feed stopped",
  service_shutdown: "API shut down",
  process_interrupted: "API process interrupted",
};

/** Feed connection, market activity, subscription, loss counters, and recordings. */
export function MarketStatusPanel({
  model,
  now,
  zone,
}: {
  model: LiveModel;
  now: number;
  zone: DisplayZone;
}) {
  const status = model.status;
  if (!status) {
    return <p className="p-4 text-sm text-fg-3">No status has been received from the live hub.</p>;
  }
  const elapsed = model.connection === "open" ? Math.max(0, now - model.refreshedAt) : null;
  const age = (ms: number | null) =>
    ms === null || elapsed === null ? UNAVAILABLE : `${formatAge(ms + elapsed)} ago`;
  const { feed, market, pipeline, clients, recording } = status;
  const subscribed = feed.subscribed.trades.length;
  const rows: KeyValueRow[] = [
    { label: "Feed connection", value: <StatusIndicator spec={FEED_STATUS[feed.state]} /> },
    {
      label: "Market activity",
      value: <StatusIndicator spec={ACTIVITY_STATUS[market.activity]} />,
      title: "Whether observations are arriving, independent of the connection",
    },
    { label: "Source", value: `${feed.source_label} · ${feed.feed.toUpperCase()}` },
    { label: "Coverage", value: feed.scope.replaceAll("_", " ") },
    { label: "Session hint", value: sessionShort(market.session), title: market.note },
    {
      label: "Last message",
      value: `${formatIsoClock(feed.last_message_at, zone)} · ${age(feed.last_message_age_ms)}`,
      title: "Any frame from the provider, including control frames",
    },
    {
      label: "Last observation",
      value: `${formatIsoClock(market.last_observation_at, zone)} · ${age(market.last_observation_age_ms)}`,
      title: "The last trade or quote the core accepted",
    },
    {
      label: "Subscription",
      value: `${feed.subscription_status} · ${subscribed} of ${feed.desired_symbols.length}`,
    },
    { label: "Reconnects", value: feed.reconnects },
    {
      label: "Dropped (service)",
      value: pipeline.dropped_messages.toLocaleString("en-US"),
      title: "Provider messages the service could not process in time",
    },
    {
      label: "Dropped (readers)",
      value: `${clients.dropped_events.toLocaleString("en-US")} · this reader ${model.gaps.droppedEvents.toLocaleString("en-US")}`,
      title: "Events slow readers did not receive; each was resynchronized from a snapshot",
    },
    {
      label: "Rejected records",
      value: pipeline.rejected.toLocaleString("en-US"),
      title: "Records the native core refused, with reasons in diagnostics",
    },
  ];
  return (
    <div>
      <KeyValue rows={rows} />
      <section aria-label="Recording" className="border-t border-line">
        <h3 className="label bg-surface-1 px-3 py-1.5 text-fg-2">Recording</h3>
        {recording.active && recording.progress ? (
          <KeyValue
            rows={[
              {
                label: "State",
                value: (
                  <StatusIndicator
                    spec={{
                      label: `Recording ${recording.id ?? ""}`,
                      tone: "active",
                      glyph: "dot",
                      pulse: true,
                    }}
                  />
                ),
              },
              {
                label: "Captured",
                value: `${recording.progress.messages.toLocaleString("en-US")} messages · ${formatBytes(recording.progress.bytes)} of ${formatBytes(recording.progress.max_bytes)}`,
              },
              {
                label: "Upstream",
                value:
                  recording.upstream === "up"
                    ? "delivering"
                    : "interrupted · recorded as a coverage gap",
              },
              { label: "Coverage gaps", value: recording.gaps },
            ]}
          />
        ) : (
          <p className="px-3 py-2 text-xs text-fg-3">
            Not recording. A recording keeps every provider frame as received and becomes a
            Historical session when stopped.
          </p>
        )}
        {recording.latest.length > 0 ? (
          <ul className="border-t border-line-subtle" aria-label="Recent recordings">
            {recording.latest.map((item) => (
              <li key={item.id} className="border-b border-line-subtle px-3 py-1.5 text-xs">
                <div className="flex items-baseline gap-2">
                  <span className="num text-fg">{item.id}</span>
                  <span className="label text-fg-3">{item.status}</span>
                  <span className="text-fg-3">
                    {item.stop_reason ? (STOP_REASON[item.stop_reason] ?? item.stop_reason) : ""}
                  </span>
                </div>
                {item.status === "completed" ? (
                  <div className="mt-0.5 flex flex-wrap items-baseline gap-x-3 gap-y-0.5 text-fg-3">
                    <span>
                      {(item.accepted_records ?? 0).toLocaleString("en-US")} records ·{" "}
                      {item.coverage_gaps === 0
                        ? "no interruption recorded"
                        : `${item.coverage_gaps} coverage ${item.coverage_gaps === 1 ? "gap" : "gaps"}`}
                    </span>
                    {Object.entries(item.sessions).map(([symbol, session]) =>
                      session.session_id && session.status === "completed" ? (
                        <Link
                          key={symbol}
                          to={`/historical/${session.session_id}`}
                          className="label text-accent hover:text-accent-hover"
                        >
                          Open {symbol} in Historical
                        </Link>
                      ) : (
                        <span key={symbol} className="label text-neg">
                          {symbol} {session.error_code ?? "failed"}
                        </span>
                      ),
                    )}
                    {Object.keys(item.sessions).length === 0 ? (
                      <span>no valid records were captured</span>
                    ) : null}
                  </div>
                ) : null}
                {item.status === "failed" ? (
                  <div className="mt-0.5 text-neg">{item.error_code ?? "finalization failed"}</div>
                ) : null}
              </li>
            ))}
          </ul>
        ) : null}
      </section>
    </div>
  );
}
