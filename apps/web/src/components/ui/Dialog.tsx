import { X } from "lucide-react";
import { Dialog as RadixDialog } from "radix-ui";
import type { ReactNode } from "react";

import { cx } from "@/lib/cx";

export interface DialogProps {
  open: boolean;
  onOpenChange: (open: boolean) => void;
  title: string;
  description?: string;
  children: ReactNode;
  footer?: ReactNode;
  /** "drawer" docks to the right edge and spans the workspace height. */
  variant?: "dialog" | "drawer";
  width?: number;
}

/** A compact dialog or a side drawer. Esc and the close control dismiss it. */
export function Dialog({
  open,
  onOpenChange,
  title,
  description,
  children,
  footer,
  variant = "dialog",
  width = 480,
}: DialogProps) {
  const drawer = variant === "drawer";
  return (
    <RadixDialog.Root open={open} onOpenChange={onOpenChange}>
      <RadixDialog.Portal>
        <RadixDialog.Overlay className="fixed inset-0 z-40 bg-canvas/70" />
        <RadixDialog.Content
          aria-describedby={description ? undefined : undefined}
          style={{ width: `min(${width}px, 100vw)` }}
          className={cx(
            "fixed z-50 flex flex-col border border-line-strong bg-surface-0 shadow-dialog",
            drawer
              ? "top-[var(--bar-top)] right-0 bottom-0 border-r-0"
              : "top-1/2 left-1/2 max-h-[84vh] -translate-x-1/2 -translate-y-1/2 rounded-lg",
          )}
        >
          <header className="flex h-8 shrink-0 items-center gap-2 border-b border-line-subtle bg-surface-1 pr-1 pl-3">
            <RadixDialog.Title className="label text-fg">{title}</RadixDialog.Title>
            <span className="flex-1" />
            <RadixDialog.Close
              aria-label="Close"
              className="transition-hover inline-flex size-6 items-center justify-center rounded-sm text-fg-3 hover:bg-surface-hover hover:text-fg"
            >
              <X size={14} aria-hidden />
            </RadixDialog.Close>
          </header>
          {description ? (
            <RadixDialog.Description className="border-b border-line-subtle px-3 py-2 text-sm text-fg-3">
              {description}
            </RadixDialog.Description>
          ) : (
            <RadixDialog.Description className="sr-only">{title}</RadixDialog.Description>
          )}
          <div className="min-h-0 flex-1 overflow-auto">{children}</div>
          {footer ? (
            <footer className="flex shrink-0 items-center justify-end gap-2 border-t border-line-subtle bg-surface-1 px-3 py-2">
              {footer}
            </footer>
          ) : null}
        </RadixDialog.Content>
      </RadixDialog.Portal>
    </RadixDialog.Root>
  );
}
