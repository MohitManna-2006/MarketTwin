import { cx } from "@/lib/cx";

export interface SegmentOption<T extends string> {
  value: T;
  label: string;
  title?: string;
  disabled?: boolean;
}

export interface SegmentedControlProps<T extends string> {
  value: T;
  options: SegmentOption<T>[];
  onChange: (value: T) => void;
  label: string;
  size?: "sm" | "md";
}

/** A compact one-of-many switch for views and series, as a radio group. */
export function SegmentedControl<T extends string>({
  value,
  options,
  onChange,
  label,
  size = "sm",
}: SegmentedControlProps<T>) {
  return (
    <div role="radiogroup" aria-label={label} className="inline-flex shrink-0 border border-line">
      {options.map((option) => {
        const selected = option.value === value;
        return (
          <button
            key={option.value}
            type="button"
            role="radio"
            aria-checked={selected}
            disabled={option.disabled}
            title={option.title}
            onClick={() => onChange(option.value)}
            className={cx(
              "transition-hover label border-r border-line px-2 whitespace-nowrap last:border-r-0",
              size === "sm" ? "h-5" : "h-6",
              selected
                ? "bg-surface-selected text-fg"
                : "text-fg-3 hover:bg-surface-hover hover:text-fg-2",
              "disabled:text-fg-off disabled:hover:bg-transparent",
            )}
          >
            {option.label}
          </button>
        );
      })}
    </div>
  );
}

export interface ToggleChipProps {
  pressed: boolean;
  onChange: (pressed: boolean) => void;
  label: string;
  /** A swatch that ties the toggle to a plotted series. */
  swatch?: string;
  title?: string;
  disabled?: boolean;
}

/** Shows or hides one series. The swatch is the series' own colour and stroke. */
export function ToggleChip({ pressed, onChange, label, swatch, title, disabled }: ToggleChipProps) {
  return (
    <button
      type="button"
      aria-pressed={pressed}
      disabled={disabled}
      title={title}
      onClick={() => onChange(!pressed)}
      className={cx(
        "transition-hover label inline-flex h-5 shrink-0 items-center gap-1.5 border px-1.5 whitespace-nowrap",
        pressed ? "border-line bg-surface-2 text-fg-2" : "border-transparent text-fg-3",
        "hover:text-fg disabled:hover:text-fg-off",
      )}
    >
      {swatch ? (
        <span
          aria-hidden
          className="inline-block h-0.5 w-3"
          style={{ background: pressed ? swatch : "var(--text-disabled)" }}
        />
      ) : null}
      {label}
    </button>
  );
}
