/** The MarketTwin mark: one market and its twin, as two mirrored depth ladders. */
export function Wordmark() {
  return (
    <span className="flex items-center gap-2.5">
      <svg width="18" height="18" viewBox="0 0 18 18" aria-hidden className="shrink-0">
        <path d="M1 15V3h2.5v12H1Zm4.25 0V7.5h2.5V15h-2.5Z" fill="var(--accent-primary)" />
        <path d="M10.25 15V7.5h2.5V15h-2.5Zm4.25 0V3H17v12h-2.5Z" fill="var(--data-neutral)" />
      </svg>
      <span className="text-[15px] leading-none font-semibold tracking-[-0.01em] text-fg">
        MarketTwin
      </span>
      <span className="hidden h-3.5 w-px bg-line md:block" aria-hidden />
      <span className="label hidden text-fg-3 md:inline">Research terminal</span>
    </span>
  );
}
