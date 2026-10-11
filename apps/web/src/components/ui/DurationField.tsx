import { useId, useState } from "react";

import { cx } from "@/lib/cx";

const UNITS: readonly (readonly [string, number])[] = [
  ["h", 3_600_000_000_000],
  ["min", 60_000_000_000],
  ["s", 1_000_000_000],
  ["ms", 1_000_000],
  ["µs", 1_000],
  ["ns", 1],
];

function largestExactUnit(ns: number): number {
  if (ns === 0) return 1_000_000_000;
  for (const [, size] of UNITS) if (ns % size === 0) return size;
  return 1;
}

export interface DurationFieldProps {
  label: string;
  /** Nanoseconds, as the config stores it. */
  value: number;
  onChange: (ns: number) => void;
  min?: number | null;
  max?: number | null;
  hint?: string;
  error?: string | null;
  disabled?: boolean;
  inline?: boolean;
  className?: string;
}

/**
 * A simulation duration entered in a convenient unit and stored as integer nanoseconds.
 * The conversion is a multiplication by a power of the unit, so it is exact.
 */
export function DurationField({
  label,
  value,
  onChange,
  min,
  max,
  hint,
  error,
  disabled,
  inline,
  className,
}: DurationFieldProps) {
  const id = useId();
  const [unit, setUnit] = useState(() => largestExactUnit(value));
  const [text, setText] = useState(() => String(value / largestExactUnit(value)));
  const [local, setLocal] = useState<string | null>(null);
  const [seen, setSeen] = useState(value);

  // The stored value changed from outside: show it in its largest exact unit.
  if (seen !== value) {
    setSeen(value);
    const typed = /^\d+$/.test(text.trim()) ? Number(text) * unit : Number.NaN;
    if (typed !== value) {
      const exact = largestExactUnit(value);
      setUnit(exact);
      setText(String(value / exact));
      setLocal(null);
    }
  }

  function commit(nextText: string, nextUnit: number) {
    setText(nextText);
    setUnit(nextUnit);
    const trimmed = nextText.trim().replaceAll(",", "");
    if (!/^\d+$/.test(trimmed)) {
      setLocal("Whole number of the chosen unit");
      return;
    }
    const ns = Number(trimmed) * nextUnit;
    if (!Number.isSafeInteger(ns)) {
      setLocal("Too long to enter here");
      return;
    }
    if (min !== null && min !== undefined && ns < min)
      setLocal(`At least ${min.toLocaleString("en-US")} ns`);
    else if (max !== null && max !== undefined && ns > max)
      setLocal("Longer than the engine allows");
    else setLocal(null);
    setSeen(ns);
    onChange(ns);
  }

  const shown = local ?? error ?? null;
  return (
    <div
      className={cx(
        inline ? "grid grid-cols-[1fr_112px] items-center gap-2" : "flex flex-col gap-1",
        className,
      )}
    >
      <label htmlFor={id} className="label flex items-baseline gap-1 text-fg-3" title={hint}>
        <span className="truncate">{label}</span>
      </label>
      <div className="min-w-0">
        <div
          className={cx(
            "flex h-6 rounded-md border bg-canvas",
            shown ? "border-neg" : "border-line",
          )}
        >
          <input
            id={id}
            type="text"
            inputMode="numeric"
            autoComplete="off"
            disabled={disabled}
            value={text}
            aria-invalid={shown ? true : undefined}
            aria-describedby={shown ? `${id}-error` : undefined}
            onChange={(event) => commit(event.target.value, unit)}
            className="num w-full min-w-0 bg-transparent px-1.5 text-right text-sm text-fg outline-none disabled:text-fg-off"
          />
          <select
            aria-label={`${label} unit`}
            disabled={disabled}
            value={unit}
            onChange={(event) => commit(text, Number(event.target.value))}
            className="w-[46px] shrink-0 appearance-none border-l border-line bg-surface-1 px-1 text-center text-xs text-fg-2 outline-none"
          >
            {UNITS.map(([name, size]) => (
              <option key={name} value={size}>
                {name}
              </option>
            ))}
          </select>
        </div>
        {shown ? (
          <p id={`${id}-error`} role="alert" className="mt-0.5 text-2xs leading-tight text-neg">
            {shown}
          </p>
        ) : null}
      </div>
    </div>
  );
}
