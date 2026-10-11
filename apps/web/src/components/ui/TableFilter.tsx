import { Search } from "lucide-react";

export interface TableFilterProps {
  value: string;
  onChange: (value: string) => void;
  placeholder: string;
  width?: number;
}

/** The filter of the current table. The "/" key focuses it from anywhere on the screen. */
export function TableFilter({ value, onChange, placeholder, width = 260 }: TableFilterProps) {
  return (
    <label
      className="flex h-5 items-center gap-1.5 rounded-md border border-line bg-canvas px-1.5 focus-within:border-focus"
      style={{ width }}
    >
      <Search size={11} aria-hidden className="shrink-0 text-fg-3" />
      <span className="sr-only">Filter</span>
      <input
        data-table-filter
        type="search"
        autoComplete="off"
        spellCheck={false}
        value={value}
        placeholder={placeholder}
        onChange={(event) => onChange(event.target.value)}
        onKeyDown={(event) => {
          if (event.key === "Escape") {
            onChange("");
            event.currentTarget.blur();
          }
        }}
        className="w-full min-w-0 bg-transparent text-xs text-fg outline-none placeholder:text-fg-off [&::-webkit-search-cancel-button]:hidden"
      />
      <kbd className="num shrink-0 text-2xs text-fg-3" aria-hidden>
        /
      </kbd>
    </label>
  );
}
