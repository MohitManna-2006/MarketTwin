import { cx } from "@/lib/cx";

// A status is a glyph, a colour, and a word. The glyph differs by state, so the meaning
// survives without colour; the word is always present.

export type StatusTone = "positive" | "negative" | "warning" | "active" | "neutral" | "muted";
type Glyph = "dot" | "ring" | "half" | "cross" | "diamond" | "square" | "slash";

export interface StatusSpec {
  label: string;
  tone: StatusTone;
  glyph: Glyph;
  pulse?: boolean;
}

const TONE_CLASS: Record<StatusTone, string> = {
  positive: "text-pos",
  negative: "text-neg",
  warning: "text-warn",
  active: "text-accent",
  neutral: "text-info",
  muted: "text-fg-3",
};

export const RUN_STATUS: Record<string, StatusSpec> = {
  pending: { label: "Pending", tone: "muted", glyph: "ring" },
  running: { label: "Running", tone: "active", glyph: "half", pulse: true },
  completed: { label: "Completed", tone: "positive", glyph: "dot" },
  failed: { label: "Failed", tone: "negative", glyph: "cross" },
};

export const REPLAY_STATUS: Record<string, StatusSpec> = {
  not_checked: { label: "Replay not checked", tone: "muted", glyph: "ring" },
  running: { label: "Replay running", tone: "active", glyph: "half", pulse: true },
  verified: { label: "Replay verified", tone: "positive", glyph: "dot" },
  mismatch: { label: "Replay mismatch", tone: "negative", glyph: "diamond" },
  unsupported_version: { label: "Unsupported version", tone: "warning", glyph: "square" },
  error: { label: "Replay error", tone: "negative", glyph: "cross" },
};

export const REPLAY_STATUS_SHORT: Record<string, string> = {
  not_checked: "Not checked",
  running: "Running",
  verified: "Verified",
  mismatch: "Mismatch",
  unsupported_version: "Unsupported",
  error: "Error",
};

export const EXPERIMENT_STATUS: Record<string, StatusSpec> = {
  pending: { label: "Pending", tone: "muted", glyph: "ring" },
  running: { label: "Running", tone: "active", glyph: "half", pulse: true },
  completed: { label: "Completed", tone: "positive", glyph: "dot" },
  partially_completed: { label: "Partially completed", tone: "warning", glyph: "half" },
  failed: { label: "Failed", tone: "negative", glyph: "cross" },
  canceled: { label: "Canceled", tone: "muted", glyph: "slash" },
};

function GlyphMark({ glyph }: { glyph: Glyph }) {
  const common = { width: 8, height: 8, viewBox: "0 0 8 8", "aria-hidden": true } as const;
  switch (glyph) {
    case "dot":
      return (
        <svg {...common}>
          <circle cx="4" cy="4" r="3" fill="currentColor" />
        </svg>
      );
    case "ring":
      return (
        <svg {...common}>
          <circle cx="4" cy="4" r="2.5" fill="none" stroke="currentColor" strokeWidth="1" />
        </svg>
      );
    case "half":
      return (
        <svg {...common}>
          <circle cx="4" cy="4" r="2.5" fill="none" stroke="currentColor" strokeWidth="1" />
          <path d="M4 1a3 3 0 0 1 0 6Z" fill="currentColor" />
        </svg>
      );
    case "cross":
      return (
        <svg {...common}>
          <path d="M1.5 1.5l5 5M6.5 1.5l-5 5" stroke="currentColor" strokeWidth="1.4" />
        </svg>
      );
    case "diamond":
      return (
        <svg {...common}>
          <path d="M4 0.6 7.4 4 4 7.4 0.6 4Z" fill="currentColor" />
        </svg>
      );
    case "square":
      return (
        <svg {...common}>
          <rect x="1.5" y="1.5" width="5" height="5" fill="none" stroke="currentColor" />
        </svg>
      );
    case "slash":
      return (
        <svg {...common}>
          <circle cx="4" cy="4" r="2.5" fill="none" stroke="currentColor" strokeWidth="1" />
          <path d="M2.2 5.8 5.8 2.2" stroke="currentColor" strokeWidth="1" />
        </svg>
      );
  }
}

export interface StatusIndicatorProps {
  spec: StatusSpec | undefined;
  /** Replaces the spec's label, for a shorter column form. */
  label?: string;
  className?: string;
}

export function StatusIndicator({ spec, label, className }: StatusIndicatorProps) {
  const resolved: StatusSpec = spec ?? { label: "Unknown", tone: "muted", glyph: "ring" };
  return (
    <span
      className={cx("inline-flex items-center gap-1.5 whitespace-nowrap", className)}
      data-status-tone={resolved.tone}
    >
      <span className={cx(TONE_CLASS[resolved.tone], resolved.pulse && "status-running-dot")}>
        <GlyphMark glyph={resolved.glyph} />
      </span>
      <span className="label text-fg-2">{label ?? resolved.label}</span>
    </span>
  );
}
