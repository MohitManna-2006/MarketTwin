import type { ReactNode } from "react";

import { cx } from "@/lib/cx";

export interface KeyValueRow {
  label: string;
  value: ReactNode;
  title?: string;
}

/** Dense label-value rows for provenance and configuration. */
export function KeyValue({ rows, className }: { rows: KeyValueRow[]; className?: string }) {
  return (
    <dl className={cx("grid grid-cols-[minmax(96px,auto)_1fr] text-sm", className)}>
      {rows.map((row) => (
        <div key={row.label} className="contents" title={row.title}>
          <dt className="label flex items-center border-b border-line-subtle py-1 pr-3 pl-3 text-fg-3">
            {row.label}
          </dt>
          <dd className="num flex min-w-0 items-center justify-end border-b border-line-subtle py-1 pr-3 text-right text-fg">
            {row.value}
          </dd>
        </div>
      ))}
    </dl>
  );
}
