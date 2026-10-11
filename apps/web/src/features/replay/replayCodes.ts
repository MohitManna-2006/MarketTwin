/** What each verification code from the engine means, in the engine's own terms. */
export const REPLAY_CODE: Record<string, { title: string; meaning: string }> = {
  Verified: {
    title: "Verified",
    meaning:
      "The run was regenerated from its config and seed. Every command matched before it reached the exchange, every output and ledger record matched after, and the metrics, final book, trades, and file checksums agree.",
  },
  TraceMismatch: {
    title: "Trace mismatch",
    meaning:
      "A command or an execution record regenerated from the config and seed is not byte-identical to the stored one. Verification stopped at the first difference.",
  },
  AccountingMismatch: {
    title: "Accounting mismatch",
    meaning:
      "Commands and executions agreed up to this point, so the same market produced different books: a ledger entry or sample differs from the stored one. The sequence is the accounting stream's own.",
  },
  MetricsMismatch: {
    title: "Metrics mismatch",
    meaning: "The recomputed metrics document is not byte-identical to the stored metrics.json.",
  },
  SummaryMismatch: {
    title: "Summary mismatch",
    meaning: "The manifest's run identity or terminal summary differs from the regenerated run.",
  },
  ChecksumMismatch: {
    title: "Checksum mismatch",
    meaning: "A checksum in the manifest does not match the regenerated artifact.",
  },
  ExtraRecords: {
    title: "Extra records",
    meaning:
      "Records remain in a stored stream after the regenerated run reached its terminal state.",
  },
  IncompleteTrace: {
    title: "Incomplete trace",
    meaning:
      "A stored stream ended, or a record was not terminated, before the regenerated run finished.",
  },
  NonCanonicalConfig: {
    title: "Non-canonical config",
    meaning: "The stored config.json is not in canonical form, so its digest cannot be trusted.",
  },
  UnsupportedVersion: {
    title: "Unsupported version",
    meaning:
      "This recording was made by a different engine version. Replay regenerates a run with the engine that recorded it, so it is not attempted: this is not a mismatch.",
  },
  UnsupportedSchema: {
    title: "Unsupported schema",
    meaning: "The manifest or a stream uses a schema version this engine does not read.",
  },
  IncompleteArtifacts: {
    title: "Incomplete artifacts",
    meaning:
      "A file the verification needs is missing. A run that failed part-way has no manifest and cannot verify.",
  },
  IoError: { title: "I/O error", meaning: "An artifact could not be read." },
};

export function replayCode(code: string | null | undefined) {
  if (!code) return null;
  return (
    REPLAY_CODE[code] ?? {
      title: code,
      meaning: "The engine reported this code without further classification.",
    }
  );
}
