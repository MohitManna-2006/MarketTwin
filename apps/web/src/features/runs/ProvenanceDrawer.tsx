import { Download } from "lucide-react";

import { Dialog } from "@/components/ui/Dialog";
import { Digest } from "@/components/ui/Digest";
import { KeyValue, type KeyValueRow } from "@/components/ui/KeyValue";
import { REPLAY_STATUS, RUN_STATUS, StatusIndicator } from "@/components/ui/StatusIndicator";
import { artifactUrl } from "@/lib/api/client";
import type { RunDetail } from "@/lib/api/types";
import {
  formatBytes,
  formatInteger,
  formatProcessing,
  formatSpan,
  formatWallClock,
} from "@/lib/formatting";

interface Manifest {
  engine_version: string;
  hash_algorithm: string;
  schema_version: number;
  schemas: Record<string, number>;
  provenance: { git_revision: string; compiler: string; build_type: string };
  artifacts: Record<string, string>;
}

interface Summary {
  book_digest: string;
  trade_digest: string;
  commands_digest: string;
  trace_digest: string;
  config_digest: string;
  stop_reason: string;
  scenario?: { rng_algorithm?: string; rng_digest?: string };
}

function Section({ title, children }: { title: string; children: React.ReactNode }) {
  return (
    <section className="border-b border-line-subtle">
      <h3 className="label bg-surface-1 px-3 py-1.5 text-fg-2">{title}</h3>
      {children}
    </section>
  );
}

/**
 * Where a run came from and what it produced: config, seed, engine build, and the digest
 * of every artifact. Each analytical number in the workspace traces back to these files.
 */
export function ProvenanceDrawer({
  run,
  open,
  onOpenChange,
}: {
  run: RunDetail;
  open: boolean;
  onOpenChange: (open: boolean) => void;
}) {
  const manifest = run.manifest as Manifest | null;
  const summary = run.summary as Summary | null;

  const identity: KeyValueRow[] = [
    { label: "Run ID", value: run.id },
    { label: "Run name", value: run.name },
    { label: "Scenario template", value: run.scenario_key ?? "none (custom config)" },
    { label: "Seed", value: run.seed },
    { label: "Simulated horizon", value: formatSpan(run.horizon_ns) },
    { label: "Status", value: <StatusIndicator spec={RUN_STATUS[run.status]} /> },
    { label: "Replay", value: <StatusIndicator spec={REPLAY_STATUS[run.replay.status]} /> },
    { label: "Stop reason", value: summary?.stop_reason ?? "—" },
  ];
  const engine: KeyValueRow[] = [
    { label: "Engine version", value: manifest?.engine_version ?? run.engine_version },
    { label: "Source revision", value: manifest?.provenance.git_revision ?? "—" },
    { label: "Compiler", value: manifest?.provenance.compiler ?? "—" },
    { label: "Build type", value: manifest?.provenance.build_type ?? "—" },
    {
      label: "Schemas",
      value: manifest
        ? `manifest ${manifest.schema_version} · config ${String((run.config as { schema_version: number }).schema_version)} · trace ${manifest.schemas.trace} · accounting ${manifest.schemas.accounting} · metrics ${manifest.schemas.metrics}`
        : "—",
    },
    { label: "Hash", value: manifest?.hash_algorithm ?? "—" },
    { label: "RNG", value: summary?.scenario?.rng_algorithm ?? "none (scripted run)" },
  ];
  const wall: KeyValueRow[] = [
    { label: "Submitted", value: formatWallClock(run.created_at), title: "Wall-clock, UTC" },
    { label: "Finished", value: formatWallClock(run.finished_at) },
    {
      label: "Engine time",
      value: formatProcessing(run.processing_ms),
      title: "Wall-clock time inside the native run",
    },
    {
      label: "Indexing time",
      value: formatProcessing(run.indexing_ms),
      title: "Wall-clock time building the terminal's read index",
    },
  ];
  const digests: KeyValueRow[] = [
    { label: "Config", value: <Digest value={run.config_digest} label="config digest" /> },
    {
      label: "Commands",
      value: <Digest value={summary?.commands_digest} label="commands digest" />,
    },
    { label: "Trace", value: <Digest value={summary?.trace_digest} label="trace digest" /> },
    { label: "Final book", value: <Digest value={summary?.book_digest} label="book digest" /> },
    { label: "Trades", value: <Digest value={summary?.trade_digest} label="trade digest" /> },
    {
      label: "Accounting",
      value: <Digest value={manifest?.artifacts["accounting.jsonl"]} label="accounting digest" />,
    },
    {
      label: "Metrics",
      value: <Digest value={manifest?.artifacts["metrics.json"]} label="metrics digest" />,
    },
    {
      label: "RNG streams",
      value: <Digest value={summary?.scenario?.rng_digest} label="RNG digest" />,
    },
  ];

  return (
    <Dialog open={open} onOpenChange={onOpenChange} title="Provenance" variant="drawer" width={460}>
      <Section title="Run">
        <KeyValue rows={identity} />
      </Section>
      {run.cell ? (
        <Section title="Experiment cell">
          <KeyValue
            rows={[
              { label: "Experiment", value: run.cell.experiment_id },
              { label: "Variant", value: run.cell.variant },
              { label: "Seed", value: run.cell.seed },
              {
                label: "Digests",
                value:
                  run.cell.digests_match === null
                    ? "not compared yet"
                    : run.cell.digests_match
                      ? "match the experiment's record"
                      : `differ: ${run.cell.mismatched.join(", ")}`,
              },
            ]}
          />
        </Section>
      ) : null}
      <Section title="Native engine">
        <KeyValue rows={engine} />
      </Section>
      <Section title="Digests · SHA-256">
        <KeyValue rows={digests} />
      </Section>
      <Section title="Artifacts">
        {run.files.length === 0 ? (
          <p className="px-3 py-2 text-sm text-fg-3">
            This run has no artifacts: the engine publishes them only when a run completes.
          </p>
        ) : (
          <ul>
            {run.files.map((file) => (
              <li
                key={file.name}
                className="flex items-center gap-2 border-b border-line-subtle px-3 py-1 text-sm last:border-b-0"
              >
                <span className="num w-[132px] shrink-0 text-fg">{file.name}</span>
                <span className="num w-[72px] shrink-0 text-right text-fg-3">
                  {formatBytes(file.bytes)}
                </span>
                <span className="min-w-0 flex-1 text-right">
                  <Digest value={file.sha256} head={8} tail={4} label={`${file.name} checksum`} />
                </span>
                <a
                  href={artifactUrl(run.id, file.name)}
                  download
                  aria-label={`Download ${file.name}`}
                  className="transition-hover inline-flex size-5 shrink-0 items-center justify-center rounded-sm text-fg-3 hover:text-accent"
                >
                  <Download size={12} aria-hidden />
                </a>
              </li>
            ))}
          </ul>
        )}
      </Section>
      <Section title="Wall clock · not simulation time">
        <KeyValue rows={wall} />
      </Section>
      <p className="px-3 py-2 text-xs leading-snug text-fg-3">
        {formatInteger(run.sample_count)} recorded samples. Digests give integrity and
        reproducibility: the same config, seed, and engine version regenerate the same bytes.
      </p>
    </Dialog>
  );
}
