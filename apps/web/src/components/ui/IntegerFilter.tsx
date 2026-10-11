import { useState } from "react";

/** A compact integer entry for filtering by an ID. Empty means no filter. */
export function IntegerFilter({
  label,
  value,
  onChange,
  width = 120,
}: {
  label: string;
  value: number | null;
  onChange: (value: number | null) => void;
  width?: number;
}) {
  const [text, setText] = useState(value === null ? "" : String(value));
  return (
    <label
      className="flex h-5 items-center border border-line focus-within:border-focus"
      style={{ width }}
    >
      <span className="sr-only">{label}</span>
      <input
        type="text"
        inputMode="numeric"
        autoComplete="off"
        value={text}
        placeholder={label}
        onChange={(event) => {
          const next = event.target.value.replace(/[^\d]/g, "");
          setText(next);
          onChange(next === "" ? null : Number(next));
        }}
        className="num w-full min-w-0 bg-transparent px-1.5 text-xs text-fg outline-none placeholder:font-sans placeholder:text-fg-off"
      />
    </label>
  );
}
