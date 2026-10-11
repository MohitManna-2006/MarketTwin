import { Component, type ErrorInfo, type ReactNode } from "react";

import { ApiError, BackendUnavailableError, describeError } from "@/lib/api/client";
import { cx } from "@/lib/cx";

import { Button } from "./Button";

export interface EmptyStateProps {
  title: string;
  children?: ReactNode;
  actions?: ReactNode;
  className?: string;
  align?: "center" | "start";
}

/** Says what is absent and what to do about it. Never a placeholder chart. */
export function EmptyState({
  title,
  children,
  actions,
  className,
  align = "center",
}: EmptyStateProps) {
  return (
    <div
      className={cx(
        "flex h-full min-h-24 flex-col justify-center gap-2 p-4",
        align === "center" ? "items-center text-center" : "items-start",
        className,
      )}
    >
      <p className="label text-fg-2">{title}</p>
      {children ? <div className="max-w-[60ch] text-sm text-fg-3">{children}</div> : null}
      {actions ? <div className="mt-1 flex flex-wrap items-center gap-2">{actions}</div> : null}
    </div>
  );
}

/** A panel whose data is not part of this run, with the reason. */
export function Unavailable({ title, reason }: { title: string; reason: ReactNode }) {
  return (
    <EmptyState title={title}>
      <span>{reason}</span>
    </EmptyState>
  );
}

export function LoadingState({ label = "Loading", rows = 0 }: { label?: string; rows?: number }) {
  if (rows > 0) {
    return (
      <div className="flex flex-col gap-px p-px" role="status" aria-label={label}>
        {Array.from({ length: rows }, (_, index) => (
          <div key={index} className="skeleton h-6" style={{ opacity: 1 - index * 0.08 }} />
        ))}
      </div>
    );
  }
  return (
    <div className="flex h-full min-h-16 items-center justify-center gap-2 text-fg-3" role="status">
      <span className="status-running-dot inline-block size-1.5 bg-accent-muted" aria-hidden />
      <span className="label">{label}</span>
    </div>
  );
}

export interface ErrorStateProps {
  error: unknown;
  onRetry?: () => void;
  className?: string;
  compact?: boolean;
}

/** A specific failure: the service's or the engine's own code and message. */
export function ErrorState({ error, onRetry, className, compact }: ErrorStateProps) {
  const { title, detail } = describeError(error);
  const unreachable = error instanceof BackendUnavailableError;
  const fields = error instanceof ApiError ? error.fieldErrors : [];
  return (
    <div
      role="alert"
      className={cx(
        "flex h-full flex-col justify-center gap-2",
        compact ? "p-3" : "items-start p-4",
        className,
      )}
    >
      {error instanceof ApiError ? (
        // A service or engine code is an identifier: shown exactly as issued.
        <p className="num text-sm font-medium text-neg">{title}</p>
      ) : (
        <p className={cx("label", unreachable ? "text-warn" : "text-neg")}>{title}</p>
      )}
      <p className="max-w-[72ch] text-sm text-fg-2">{detail}</p>
      {fields.length > 0 ? (
        <ul className="flex flex-col gap-0.5 text-sm text-fg-2">
          {fields.slice(0, 8).map((field, index) => (
            <li key={index}>
              {field.path ? <span className="num text-fg-3">{field.path} </span> : null}
              {field.message}
            </li>
          ))}
        </ul>
      ) : null}
      {unreachable ? (
        <p className="text-xs text-fg-3">No recorded data is shown while the API is unreachable.</p>
      ) : null}
      {onRetry ? (
        <Button size="sm" onClick={onRetry} className="mt-1">
          Retry
        </Button>
      ) : null}
    </div>
  );
}

interface BoundaryProps {
  children: ReactNode;
  /** Changing this value resets the boundary, for example on navigation. */
  resetKey?: string;
  label?: string;
}

/** Contains a rendering failure to the panel it happened in. */
export class ErrorBoundary extends Component<BoundaryProps, { error: Error | null }> {
  override state: { error: Error | null } = { error: null };

  static getDerivedStateFromError(error: Error) {
    return { error };
  }

  override componentDidCatch(error: Error, info: ErrorInfo) {
    console.error("Panel failed to render", error, info.componentStack);
  }

  override componentDidUpdate(previous: BoundaryProps) {
    if (previous.resetKey !== this.props.resetKey && this.state.error) {
      this.setState({ error: null });
    }
  }

  override render() {
    if (!this.state.error) return this.props.children;
    return (
      <div role="alert" className="flex h-full flex-col justify-center gap-2 p-4">
        <p className="label text-neg">{this.props.label ?? "Panel"} failed to render</p>
        <p className="num text-sm text-fg-2">{this.state.error.message}</p>
        <Button size="sm" className="self-start" onClick={() => this.setState({ error: null })}>
          Try again
        </Button>
      </div>
    );
  }
}
