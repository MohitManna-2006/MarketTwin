import { Fragment, type ReactNode } from "react";
import { Link } from "react-router";

import { cx } from "@/lib/cx";

export interface Crumb {
  label: string;
  to?: string;
  /** Keeps the value's own case, for IDs and names. */
  verbatim?: boolean;
}

export interface ContextBarProps {
  crumbs: Crumb[];
  /** Status indicators for what is being inspected. */
  status?: ReactNode;
  /** Views of the same object, and actions on it. */
  children?: ReactNode;
}

/**
 * Says what is on screen: kind, name, seed, engine. It stays visible while a run or an
 * experiment is inspected, so provenance is never a click away.
 */
export function ContextBar({ crumbs, status, children }: ContextBarProps) {
  return (
    <div className="flex min-h-[var(--bar-context)] shrink-0 flex-wrap items-center gap-x-4 border-b border-line bg-canvas pr-2 pl-4 md:h-[var(--bar-context)] md:flex-nowrap">
      <nav
        aria-label="Context"
        className="flex min-w-0 items-center gap-2 max-md:w-full max-md:overflow-x-auto max-md:py-1 max-md:[&>*]:shrink-0"
      >
        {crumbs.map((crumb, index) => {
          const last = index === crumbs.length - 1;
          const text = (
            <span
              className={cx(
                "num truncate text-xs tracking-[0.04em]",
                !crumb.verbatim && "uppercase",
                last ? "text-fg" : "text-fg-3",
              )}
            >
              {crumb.label}
            </span>
          );
          return (
            <Fragment key={`${crumb.label}-${index}`}>
              {index > 0 ? (
                <span className="text-fg-3" aria-hidden>
                  /
                </span>
              ) : null}
              {crumb.to ? (
                <Link to={crumb.to} className="transition-hover min-w-0 hover:[&>span]:text-fg">
                  {text}
                </Link>
              ) : (
                text
              )}
            </Fragment>
          );
        })}
      </nav>
      {status ? <div className="flex shrink-0 items-center gap-4">{status}</div> : null}
      <span className="flex-1 max-md:hidden" />
      {children ? (
        <div className="flex max-w-full shrink-0 items-center gap-2 overflow-x-auto max-md:pb-1">
          {children}
        </div>
      ) : null}
    </div>
  );
}

export interface ViewTab {
  to: string;
  label: string;
  active: boolean;
}

/** Views of one object, shown in the context bar. */
export function ViewTabs({ tabs, label }: { tabs: ViewTab[]; label: string }) {
  return (
    <nav aria-label={label} className="flex h-[var(--bar-context)] items-stretch">
      {tabs.map((tab) => (
        <Link
          key={tab.to}
          to={tab.to}
          aria-current={tab.active ? "page" : undefined}
          className={cx(
            "transition-hover label flex items-center px-3",
            tab.active
              ? "text-fg shadow-[inset_0_-2px_0_var(--accent-primary)]"
              : "text-fg-3 hover:text-fg-2",
          )}
        >
          {tab.label}
        </Link>
      ))}
    </nav>
  );
}
