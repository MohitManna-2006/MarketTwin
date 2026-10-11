import { useId, useState, type ReactNode } from "react";

import { cx } from "@/lib/cx";

const CONTROL =
  "h-6 w-full min-w-0 rounded-md border bg-canvas px-1.5 text-sm text-fg outline-none transition-hover " +
  "placeholder:text-fg-off focus-visible:border-focus disabled:text-fg-off";

interface Frame {
  label: string;
  unit?: string;
  hint?: string;
  error?: string | null;
  className?: string;
  /** Lays the label beside the control instead of above it. */
  inline?: boolean;
}

function FieldFrame({
  id,
  label,
  unit,
  hint,
  error,
  className,
  inline,
  children,
}: Frame & { id: string; children: ReactNode }) {
  return (
    <div
      className={cx(
        inline ? "grid grid-cols-[1fr_112px] items-center gap-2" : "flex flex-col gap-1",
        className,
      )}
    >
      <label htmlFor={id} className="label flex items-baseline gap-1 text-fg-3" title={hint}>
        <span className="truncate">{label}</span>
        {unit ? (
          <span className="font-normal tracking-normal text-fg-3 normal-case">{unit}</span>
        ) : null}
      </label>
      <div className="min-w-0">
        {children}
        {error ? (
          <p id={`${id}-error`} role="alert" className="mt-0.5 text-2xs leading-tight text-neg">
            {error}
          </p>
        ) : null}
      </div>
    </div>
  );
}

export interface IntegerFieldProps extends Frame {
  value: number | null;
  onChange: (value: number | null) => void;
  min?: number | null;
  max?: number | null;
  nullable?: boolean;
  disabled?: boolean;
  /** Allows a leading minus sign. */
  signed?: boolean;
}

/**
 * An integer, and only an integer: the core's configs carry no floating point. The field
 * keeps what was typed and reports a range or format problem under itself.
 */
export function IntegerField({
  value,
  onChange,
  min,
  max,
  nullable,
  disabled,
  signed,
  error,
  ...frame
}: IntegerFieldProps) {
  const id = useId();
  const [text, setText] = useState(value === null ? "" : String(value));
  const [local, setLocal] = useState<string | null>(null);
  const [seen, setSeen] = useState(value);

  // The stored value changed from outside (another scenario, a JSON edit): show it, unless
  // it is what the field already says.
  if (seen !== value) {
    setSeen(value);
    const typed = text.trim() === "" ? null : Number(text.replaceAll(",", ""));
    if (typed !== value) {
      setText(value === null ? "" : String(value));
      setLocal(null);
    }
  }

  function commit(next: string) {
    setText(next);
    const trimmed = next.trim().replaceAll(",", "").replaceAll("_", "");
    if (trimmed === "") {
      if (nullable) {
        setLocal(null);
        setSeen(null);
        onChange(null);
      } else {
        setLocal("Required");
      }
      return;
    }
    if (!(signed ? /^-?\d+$/ : /^\d+$/).test(trimmed)) {
      setLocal(signed ? "Whole number" : "Whole number, zero or greater");
      return;
    }
    const parsed = Number(trimmed);
    if (!Number.isSafeInteger(parsed)) {
      setLocal("Too large to enter here");
      return;
    }
    if (min !== null && min !== undefined && parsed < min) {
      setLocal(`At least ${min.toLocaleString("en-US")}`);
    } else if (max !== null && max !== undefined && parsed > max) {
      setLocal(`At most ${max.toLocaleString("en-US")}`);
    } else {
      setLocal(null);
    }
    setSeen(parsed);
    onChange(parsed);
  }

  const shown = local ?? error ?? null;
  return (
    <FieldFrame id={id} error={shown} {...frame}>
      <input
        id={id}
        type="text"
        inputMode="numeric"
        autoComplete="off"
        spellCheck={false}
        disabled={disabled}
        value={text}
        placeholder={nullable ? "none" : undefined}
        aria-invalid={shown ? true : undefined}
        aria-describedby={shown ? `${id}-error` : undefined}
        onChange={(event) => commit(event.target.value)}
        className={cx(CONTROL, "num text-right", shown ? "border-neg" : "border-line")}
      />
    </FieldFrame>
  );
}

export interface TextFieldProps extends Frame {
  value: string;
  onChange: (value: string) => void;
  placeholder?: string;
  disabled?: boolean;
  mono?: boolean;
  maxLength?: number;
}

export function TextField({
  value,
  onChange,
  placeholder,
  disabled,
  mono,
  maxLength,
  error,
  ...frame
}: TextFieldProps) {
  const id = useId();
  return (
    <FieldFrame id={id} error={error} {...frame}>
      <input
        id={id}
        type="text"
        autoComplete="off"
        spellCheck={false}
        disabled={disabled}
        value={value}
        maxLength={maxLength}
        placeholder={placeholder}
        aria-invalid={error ? true : undefined}
        aria-describedby={error ? `${id}-error` : undefined}
        onChange={(event) => onChange(event.target.value)}
        className={cx(CONTROL, mono && "num", error ? "border-neg" : "border-line")}
      />
    </FieldFrame>
  );
}

export interface SelectFieldProps extends Frame {
  value: string;
  onChange: (value: string) => void;
  options: { value: string; label: string }[];
  disabled?: boolean;
}

export function SelectField({
  value,
  onChange,
  options,
  disabled,
  error,
  ...frame
}: SelectFieldProps) {
  const id = useId();
  return (
    <FieldFrame id={id} error={error} {...frame}>
      <select
        id={id}
        disabled={disabled}
        value={value}
        onChange={(event) => onChange(event.target.value)}
        className={cx(CONTROL, "appearance-none pr-5", error ? "border-neg" : "border-line")}
        style={{
          backgroundImage:
            "linear-gradient(45deg, transparent 50%, var(--text-tertiary) 50%), linear-gradient(135deg, var(--text-tertiary) 50%, transparent 50%)",
          backgroundPosition: "calc(100% - 10px) 10px, calc(100% - 6px) 10px",
          backgroundSize: "4px 4px, 4px 4px",
          backgroundRepeat: "no-repeat",
        }}
      >
        {options.map((option) => (
          <option key={option.value} value={option.value}>
            {option.label}
          </option>
        ))}
      </select>
    </FieldFrame>
  );
}

export interface CheckboxProps {
  checked: boolean;
  onChange: (checked: boolean) => void;
  label: string;
  disabled?: boolean;
}

export function Checkbox({ checked, onChange, label, disabled }: CheckboxProps) {
  const id = useId();
  return (
    <label
      htmlFor={id}
      className="inline-flex items-center gap-1.5 text-sm whitespace-nowrap text-fg-2"
    >
      <input
        id={id}
        type="checkbox"
        checked={checked}
        disabled={disabled}
        onChange={(event) => onChange(event.target.checked)}
        className="size-3 shrink-0 appearance-none rounded-sm border border-line-strong bg-canvas checked:border-accent checked:bg-accent checked:shadow-[inset_0_0_0_2px_var(--canvas)]"
      />
      {label}
    </label>
  );
}
