import { useEffect, type ReactNode } from "react";
import { Outlet, useLocation } from "react-router";

import { ErrorBoundary } from "@/components/ui/States";
import { useHotkeys } from "@/hooks/useHotkeys";
import { cx } from "@/lib/cx";

import { TopNavigation } from "./TopNavigation";

/** The application frame: one slim bar, then the workspace for the current section. */
export function TerminalShell() {
  const location = useLocation();
  useHotkeys({
    "/": (event) => {
      const target = document.querySelector<HTMLInputElement>("[data-table-filter]");
      if (target) {
        event.preventDefault();
        target.focus();
        target.select();
      }
    },
  });
  useEffect(() => {
    document.getElementById("workspace")?.focus({ preventScroll: true });
  }, [location.pathname]);

  return (
    <div className="flex h-full flex-col">
      <a
        href="#workspace"
        className="label sr-only focus:not-sr-only focus:absolute focus:z-50 focus:bg-surface-2 focus:px-3 focus:py-1 focus:text-fg"
      >
        Skip to workspace
      </a>
      <TopNavigation />
      <ErrorBoundary resetKey={location.pathname} label="Workspace">
        <Outlet />
      </ErrorBoundary>
    </div>
  );
}

export interface PageProps {
  /** The context bar for this screen. */
  context: ReactNode;
  children: ReactNode;
  /** Workspaces fill the viewport and scroll inside panels; documents scroll as a page. */
  scroll?: boolean;
  className?: string;
}

export function Page({ context, children, scroll = false, className }: PageProps) {
  return (
    <>
      {context}
      <main
        id="workspace"
        tabIndex={-1}
        className={cx(
          "min-h-0 flex-1 outline-none",
          // Below the desktop breakpoint a workspace becomes a page that scrolls.
          scroll ? "overflow-auto" : "overflow-hidden max-lg:overflow-auto",
          className,
        )}
      >
        {children}
      </main>
    </>
  );
}
