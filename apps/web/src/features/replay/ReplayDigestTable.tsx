import { Digest } from "@/components/ui/Digest";
import type { ReplayView, RunDetail } from "@/lib/api/types";
import { formatBytes } from "@/lib/formatting";

interface Manifest {
  engine_version: string;
  schema_version: number;
  hash_algorithm: string;
  schemas: Record<string, number>;
  provenance: { git_revision: string; compiler: string; build_type: string };
  artifacts: Record<string, string>;
}

interface Summary {
  config_digest: string;
  commands_digest: string;
  trace_digest: string;
  book_digest: string;
  trade_digest: string;
}

/**
 * The digests a replay checks: the config, each recorded stream, the final book, and the
 * trade sequence. A digest proves integrity and reproducibility, not authorship.
 */
export function ReplayDigestTable({
  run,
  replay,
  engineVersion,
}: {
  run: RunDetail;
  replay: ReplayView;
  engineVersion: string | undefined;
}) {
  const manifest = run.manifest as Manifest | null;
  const summary = run.summary as Summary | null;
  const size = (name: string) => run.files.find((file) => file.name === name)?.bytes ?? null;
  const verified = replay.status === "verified";
  const rows: {
    label: string;
    note: string;
    digest: string | null | undefined;
    bytes: number | null;
  }[] = [
    {
      label: "Config",
      note: "canonical config.json content",
      digest: summary?.config_digest ?? run.config_digest,
      bytes: size("config.json"),
    },
    {
      label: "Commands",
      note: "commands.jsonl · inputs to the exchange",
      digest: summary?.commands_digest,
      bytes: size("commands.jsonl"),
    },
    {
      label: "Trace",
      note: "trace.jsonl · exchange outputs",
      digest: summary?.trace_digest,
      bytes: size("trace.jsonl"),
    },
    {
      label: "Trades",
      note: "every trade payload, in execution order",
      digest: summary?.trade_digest,
      bytes: null,
    },
    {
      label: "Final book",
      note: "every live order, with FIFO priority",
      digest: summary?.book_digest,
      bytes: null,
    },
    {
      label: "Accounting",
      note: "accounting.jsonl · ledger and samples",
      digest: manifest?.artifacts["accounting.jsonl"],
      bytes: size("accounting.jsonl"),
    },
    {
      label: "Metrics",
      note: "metrics.json",
      digest: manifest?.artifacts["metrics.json"],
      bytes: size("metrics.json"),
    },
  ];
  const sameEngine = manifest && engineVersion ? manifest.engine_version === engineVersion : null;

  return (
    <div
      tabIndex={0}
      role="group"
      aria-label="Digests checked by replay"
      className="flex h-full flex-col overflow-auto"
    >
      <dl className="grid grid-cols-2 border-b border-line-subtle">
        <div className="border-r border-line-subtle px-3 py-2">
          <dt className="label text-fg-3">Recorded by engine</dt>
          <dd className="num text-md font-medium text-fg">
            {manifest?.engine_version ?? run.engine_version}
          </dd>
          <dd className="num truncate text-2xs text-fg-3">
            {manifest
              ? `${manifest.provenance.build_type} · ${manifest.provenance.git_revision}`
              : "no manifest"}
          </dd>
        </div>
        <div className="px-3 py-2">
          <dt className="label text-fg-3">Replaying engine</dt>
          <dd className="num text-md font-medium text-fg">{engineVersion ?? "—"}</dd>
          <dd className="text-2xs text-fg-3">
            {sameEngine === null
              ? "unknown"
              : sameEngine
                ? "same version: replay is supported"
                : "different version: replay is not attempted"}
          </dd>
        </div>
      </dl>
      <table className="w-full text-sm">
        <caption className="sr-only">Digests checked by replay verification</caption>
        <thead>
          <tr className="border-b border-line bg-surface-1">
            <th scope="col" className="label px-3 py-1 text-left text-fg-3">
              Checked
            </th>
            <th scope="col" className="label px-3 py-1 text-right text-fg-3">
              SHA-256
            </th>
            <th scope="col" className="label px-3 py-1 text-right text-fg-3">
              Size
            </th>
            <th scope="col" className="label px-3 py-1 text-right text-fg-3">
              Replay
            </th>
          </tr>
        </thead>
        <tbody>
          {rows.map((row) => (
            <tr key={row.label} className="border-b border-line-subtle">
              <th scope="row" className="px-3 py-1.5 text-left font-normal" title={row.note}>
                <span className="block whitespace-nowrap text-fg">{row.label}</span>
                <span className="hidden text-2xs text-fg-3 2xl:block">{row.note}</span>
              </th>
              <td className="px-3 py-1 text-right">
                <Digest value={row.digest} head={8} tail={6} label={`${row.label} digest`} />
              </td>
              <td className="num px-3 py-1 text-right text-fg-3">
                {row.bytes === null ? "" : formatBytes(row.bytes)}
              </td>
              <td className="px-3 py-1 text-right">
                <span className={verified ? "label text-pos" : "label text-fg-3"}>
                  {verified ? "matched" : "—"}
                </span>
              </td>
            </tr>
          ))}
        </tbody>
      </table>
      <p className="px-3 py-2 text-xs leading-snug text-fg-3">
        {manifest
          ? `Schemas: manifest ${manifest.schema_version}, trace ${manifest.schemas.trace}, accounting ${manifest.schemas.accounting}, metrics ${manifest.schemas.metrics}. Hash: ${manifest.hash_algorithm}. `
          : ""}
        A match means the same config, seed, and engine version regenerate the same bytes. It does
        not protect against someone rewriting the whole run.
      </p>
    </div>
  );
}
