import type { ReactNode } from "react";

import { Dialog } from "@/components/ui/Dialog";
import { KeyValue } from "@/components/ui/KeyValue";
import { StatusIndicator } from "@/components/ui/StatusIndicator";
import { UNAVAILABLE, formatBytes } from "@/lib/formatting";

import { AVAILABILITY_STATUS, FEED_STATUS, agesOf, availabilityOf } from "./derive";
import { formatAge, formatIsoClock, type DisplayZone } from "./format";
import type { LiveModel } from "./liveModel";

function Section({ title, children }: { title: string; children: ReactNode }) {
  return (
    <section className="border-b border-line-subtle">
      <h3 className="label bg-surface-1 px-3 py-1.5 text-fg-2">{title}</h3>
      {children}
    </section>
  );
}

const micros = (value: number | null) =>
  value === null ? UNAVAILABLE : value >= 1000 ? `${(value / 1000).toFixed(1)} ms` : `${value} µs`;

/** Everything needed to explain why the terminal shows what it shows. */
export function FeedDiagnostics({
  model,
  now,
  zone,
  open,
  onOpenChange,
}: {
  model: LiveModel;
  now: number;
  zone: DisplayZone;
  open: boolean;
  onOpenChange: (open: boolean) => void;
}) {
  const status = model.status;
  return (
    <Dialog
      open={open}
      onOpenChange={onOpenChange}
      title="Feed diagnostics"
      variant="drawer"
      width={560}
    >
      {!status ? (
        <p className="p-4 text-sm text-fg-3">No status has been received from the live hub.</p>
      ) : (
        <>
          <Section title="Transport health">
            <KeyValue
              rows={[
                {
                  label: "Provider connection",
                  value: <StatusIndicator spec={FEED_STATUS[status.feed.state]} />,
                },
                { label: "Endpoint", value: status.feed.endpoint_host ?? UNAVAILABLE },
                {
                  label: "Connected since",
                  value: formatIsoClock(status.feed.connected_since, zone),
                },
                {
                  label: "Connections / reconnects",
                  value: `${status.feed.connections} / ${status.feed.reconnects}`,
                },
                {
                  label: "Last reconnect",
                  value: formatIsoClock(status.feed.last_reconnect_at, zone),
                },
                {
                  label: "Frames / data frames",
                  value: `${status.feed.frames.toLocaleString("en-US")} / ${status.feed.data_frames.toLocaleString("en-US")}`,
                },
                { label: "Protocol errors", value: status.feed.protocol_errors },
                {
                  label: "Terminal stream",
                  value: `${model.connection} · ${model.reconnects} reconnects · seq ${model.seq ?? UNAVAILABLE}`,
                },
                {
                  label: "Credentials",
                  value: status.feed.credentials_configured
                    ? "configured on the API"
                    : "not configured",
                },
              ]}
            />
          </Section>
          <Section title="Recent errors">
            {status.feed.recent_errors.length === 0 ? (
              <p className="px-3 py-2 text-xs text-fg-3">
                No provider or transport error recorded.
              </p>
            ) : (
              <ul>
                {[...status.feed.recent_errors]
                  .reverse()
                  .slice(0, 12)
                  .map((error, index) => (
                    <li
                      key={`${error.at}-${index}`}
                      className="flex items-baseline gap-2 border-b border-line-subtle px-3 py-1 text-xs"
                    >
                      <span className="num text-fg-3">{formatIsoClock(error.at, zone)}</span>
                      <span className="label text-neg">{error.code}</span>
                      <span className="min-w-0 text-fg-2">
                        {error.message}
                        {error.provider_code ? ` (provider code ${error.provider_code})` : ""}
                      </span>
                    </li>
                  ))}
              </ul>
            )}
            {status.pipeline.recent_rejects.length > 0 ? (
              <ul aria-label="Rejected records">
                {status.pipeline.recent_rejects.map((reject, index) => (
                  <li
                    key={`${reject.at}-${index}`}
                    className="flex items-baseline gap-2 border-b border-line-subtle px-3 py-1 text-xs"
                  >
                    <span className="num text-fg-3">{formatIsoClock(reject.at, zone)}</span>
                    <span className="label text-warn">Rejected {reject.symbol ?? ""}</span>
                    <span className="min-w-0 text-fg-2">{reject.reason}</span>
                  </li>
                ))}
              </ul>
            ) : null}
          </Section>
          <Section title="Per-symbol freshness">
            <table className="w-full text-left text-xs">
              <thead>
                <tr className="label text-fg-3">
                  {["Symbol", "State", "Trade age", "Quote age", "Late", "Dup"].map((label) => (
                    <th key={label} className="border-b border-line-subtle px-3 py-1 font-semibold">
                      {label}
                    </th>
                  ))}
                </tr>
              </thead>
              <tbody>
                {model.watchlist.map((symbol) => {
                  const state = model.states[symbol];
                  const ages = agesOf(state, now - (model.stateAt.get(symbol) ?? now));
                  return (
                    <tr key={symbol} className="border-b border-line-subtle text-fg-2">
                      <td className="num px-3 py-1 text-fg">{symbol}</td>
                      <td className="px-3 py-1">
                        <StatusIndicator
                          spec={AVAILABILITY_STATUS[availabilityOf(symbol, model, status, ages)]}
                        />
                      </td>
                      <td className="num px-3 py-1">{formatAge(ages.trade)}</td>
                      <td className="num px-3 py-1">{formatAge(ages.quote)}</td>
                      <td className="num px-3 py-1">{state?.counts.out_of_order ?? 0}</td>
                      <td className="num px-3 py-1">{state?.counts.duplicates ?? 0}</td>
                    </tr>
                  );
                })}
              </tbody>
            </table>
          </Section>
          <Section title="Queue pressure">
            <KeyValue
              rows={[
                {
                  label: "Service inbox",
                  value: `${status.pipeline.inbox_depth} of ${status.pipeline.inbox_capacity} · peak ${status.pipeline.inbox_high_water}`,
                },
                {
                  label: "Reader queues",
                  value: `${status.clients.queue_depth_max} of ${status.clients.queue_capacity} · peak ${status.clients.queue_high_water}`,
                },
                {
                  label: "Readers",
                  value: `${status.clients.connected} of ${status.clients.capacity} · ${status.clients.slow_disconnects} disconnected as slow`,
                },
                {
                  label: "Throughput",
                  value: `${status.pipeline.messages_per_s} msg/s · ${status.pipeline.events_per_s} events/s`,
                },
                {
                  label: "Dropped by service",
                  value: `${status.pipeline.dropped_messages.toLocaleString("en-US")} messages in ${status.pipeline.dropped_frames.toLocaleString("en-US")} frames`,
                },
                {
                  label: "Dropped for readers",
                  value: `${status.clients.dropped_events.toLocaleString("en-US")} events in ${status.clients.overflows} overflows`,
                },
                {
                  label: "This reader",
                  value: `${model.gaps.sequence} sequence gaps · ${model.gaps.slowClient} slow-reader gaps · ${model.gaps.pipeline} service gaps`,
                },
                {
                  label: "Local latency",
                  value: `p50 ${micros(status.pipeline.local_latency_us.p50)} · p99 ${micros(status.pipeline.local_latency_us.p99)}`,
                  title:
                    "Gateway receipt to fan-out, on the service's monotonic clock; includes the coalescing window, excludes both networks",
                },
                {
                  label: "Native normalize",
                  value:
                    status.pipeline.native_ns_per_message.p50 === null
                      ? UNAVAILABLE
                      : `p50 ${status.pipeline.native_ns_per_message.p50.toLocaleString("en-US")} ns/message`,
                },
                {
                  label: "Late / duplicate / unsupported",
                  value: `${status.pipeline.out_of_order} / ${status.pipeline.duplicates} / ${Object.values(
                    status.pipeline.unsupported,
                  ).reduce((sum, count) => sum + count, 0)}`,
                  title: "Corrections and cancels are counted, kept in recordings, and not applied",
                },
              ]}
            />
          </Section>
          <Section title="Recording state">
            <KeyValue
              rows={[
                {
                  label: "Recording",
                  value: status.recording.active ? `active · ${status.recording.id}` : "inactive",
                },
                {
                  label: "Capture size",
                  value: status.recording.progress
                    ? `${formatBytes(status.recording.progress.bytes)} of ${formatBytes(status.recording.progress.max_bytes)}`
                    : UNAVAILABLE,
                },
                { label: "Coverage gaps", value: status.recording.gaps },
                { label: "Finalizing", value: status.recording.finalizing.join(", ") || "none" },
              ]}
            />
          </Section>
          <p className="px-3 py-2 text-xs text-fg-3">{status.clock.note}.</p>
        </>
      )}
    </Dialog>
  );
}
