import { useId, type ReactNode } from "react";

import { cx } from "@/lib/cx";

export interface PanelHeaderProps {
  /** Workspace grouping number, such as "01". It groups panels; it is not a step. */
  index?: string;
  title: string;
  subtitle?: ReactNode;
  unit?: string;
  actions?: ReactNode;
  id?: string;
}

export function PanelHeader({ index, title, subtitle, unit, actions, id }: PanelHeaderProps) {
  return (
    <header className="flex h-7 shrink-0 items-center gap-2 border-b border-line-subtle bg-surface-1 pl-3 pr-1.5">
      <h2 id={id} className="label flex shrink-0 items-baseline gap-1.5 whitespace-nowrap text-fg">
        {index ? (
          <>
            <span className="num text-[10.5px] font-medium tracking-normal text-accent-label">
              {index}
            </span>
            <span className="text-fg-3" aria-hidden>
              /
            </span>
          </>
        ) : null}
        <span>{title}</span>
      </h2>
      {subtitle ? <span className="min-w-0 truncate text-xs text-fg-3">{subtitle}</span> : null}
      <span className="flex-1" />
      {actions ? <div className="flex shrink-0 items-center gap-1">{actions}</div> : null}
      {unit ? (
        <span className="label shrink-0 pr-1.5 whitespace-nowrap text-fg-3">{unit}</span>
      ) : null}
    </header>
  );
}

export interface InstrumentPanelProps extends PanelHeaderProps {
  children: ReactNode;
  className?: string;
  bodyClassName?: string;
  footer?: ReactNode;
}

/** A workspace panel: a flat surface with a one-line header and a hairline border. */
export function InstrumentPanel({
  children,
  className,
  bodyClassName,
  footer,
  ...header
}: InstrumentPanelProps) {
  const headingId = useId();
  return (
    <section
      aria-labelledby={headingId}
      className={cx("flex min-h-0 min-w-0 flex-col bg-surface-0", className)}
    >
      <PanelHeader {...header} id={headingId} />
      {bodyClassName?.includes("overflow-auto") ? (
        // A region that scrolls must be reachable from the keyboard (WCAG 2.1.1).
        <div
          tabIndex={0}
          role="group"
          aria-labelledby={headingId}
          className={cx("min-h-0 min-w-0 flex-1", bodyClassName)}
        >
          {children}
        </div>
      ) : (
        <div className={cx("min-h-0 min-w-0 flex-1", bodyClassName)}>{children}</div>
      )}
      {footer ? (
        <footer className="flex h-6 shrink-0 items-center gap-3 border-t border-line-subtle bg-surface-1 px-3 text-xs text-fg-3">
          {footer}
        </footer>
      ) : null}
    </section>
  );
}
