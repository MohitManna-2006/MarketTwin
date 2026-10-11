import { Check, Copy } from "lucide-react";
import { useEffect, useState } from "react";

import { cx } from "@/lib/cx";
import { UNAVAILABLE } from "@/lib/formatting";

import { Tooltip } from "./Tooltip";

export interface DigestProps {
  value: string | null | undefined;
  /** Leading and trailing characters to show. The full value is always copied. */
  head?: number;
  tail?: number;
  label?: string;
  className?: string;
  full?: boolean;
}

/**
 * A hash in compact monospaced form. Only the display is shortened: the tooltip and the
 * copy action carry every character.
 */
export function Digest({
  value,
  head = 10,
  tail = 6,
  label = "digest",
  className,
  full,
}: DigestProps) {
  const [copied, setCopied] = useState(false);
  useEffect(() => {
    if (!copied) return;
    const timer = window.setTimeout(() => setCopied(false), 1200);
    return () => window.clearTimeout(timer);
  }, [copied]);

  if (!value) return <span className="num text-fg-3">{UNAVAILABLE}</span>;
  const short =
    full || value.length <= head + tail + 1
      ? value
      : `${value.slice(0, head)}…${value.slice(-tail)}`;

  return (
    <span
      className={cx(
        "group inline-flex max-w-full items-center gap-1 tracking-normal normal-case",
        className,
      )}
    >
      <Tooltip content={<span className="num break-all">{value}</span>}>
        <span className={cx("num text-fg-2", full && "break-all whitespace-normal")}>{short}</span>
      </Tooltip>
      <button
        type="button"
        aria-label={copied ? `${label} copied` : `Copy full ${label}`}
        className="transition-hover inline-flex size-4 shrink-0 items-center justify-center rounded-sm text-fg-3 hover:text-accent"
        onClick={() => {
          void navigator.clipboard.writeText(value).then(() => setCopied(true));
        }}
      >
        {copied ? <Check size={11} aria-hidden /> : <Copy size={11} aria-hidden />}
      </button>
    </span>
  );
}
