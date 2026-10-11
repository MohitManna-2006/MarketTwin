import { forwardRef, type ButtonHTMLAttributes } from "react";

import { cx } from "@/lib/cx";

type Variant = "primary" | "solid" | "secondary" | "ghost" | "danger";

const VARIANT: Record<Variant, string> = {
  // The single committed action of a workflow.
  solid:
    "border border-accent bg-accent text-canvas font-semibold hover:bg-accent-hover hover:border-accent-hover",
  // A visible action that does not shout.
  primary:
    "border border-accent-muted bg-accent-wash text-accent hover:border-accent hover:text-accent-hover",
  secondary: "border border-line bg-surface-1 text-fg-2 hover:bg-surface-hover hover:text-fg",
  ghost: "border border-transparent text-fg-2 hover:bg-surface-hover hover:text-fg",
  danger: "border border-line bg-surface-1 text-neg hover:border-neg",
};

export interface ButtonProps extends ButtonHTMLAttributes<HTMLButtonElement> {
  variant?: Variant;
  size?: "sm" | "md";
  /** Icon-only buttons need a label for assistive technology. */
  "aria-label"?: string;
}

export const Button = forwardRef<HTMLButtonElement, ButtonProps>(function Button(
  { variant = "secondary", size = "md", className, type = "button", ...rest },
  ref,
) {
  return (
    <button
      ref={ref}
      type={type}
      className={cx(
        "transition-hover inline-flex shrink-0 items-center justify-center gap-1.5 rounded-md whitespace-nowrap",
        "label disabled:border-line-subtle disabled:bg-transparent disabled:text-fg-off",
        size === "sm" ? "h-[22px] px-2" : "h-7 px-3",
        VARIANT[variant],
        className,
      )}
      {...rest}
    />
  );
});
