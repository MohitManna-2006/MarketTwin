import { useMutation, useQuery, useQueryClient } from "@tanstack/react-query";
import { useState } from "react";
import { useNavigate } from "react-router";

import { ContextBar } from "@/components/shell/ContextBar";
import { Page } from "@/components/shell/TerminalShell";
import { Button } from "@/components/ui/Button";
import { Checkbox, SelectField, TextField } from "@/components/ui/Field";
import { InstrumentPanel } from "@/components/ui/InstrumentPanel";
import { ErrorState, LoadingState } from "@/components/ui/States";
import { StatusIndicator, RUN_STATUS } from "@/components/ui/StatusIndicator";
import { post } from "@/lib/api/client";
import { historicalImportsQuery, historicalProvidersQuery } from "@/lib/api/queries";
import type { HistoricalImport } from "@/lib/api/types";

const UTC_INSTANT = /^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}(?:\.\d{1,9})?(?:Z|\+00:00)$/;

/** Provider ranges are UTC instants. Wall-clock text such as "9:30 AM ET" is not a timestamp. */
export function utcInstantError(value: string): string | null {
  const trimmed = value.trim();
  if (trimmed === "") return null;
  if (!UTC_INSTANT.test(trimmed) || Number.isNaN(Date.parse(trimmed))) {
    return "Use a UTC instant, such as 2026-10-09T13:30:00Z";
  }
  return null;
}

export function HistoricalLibraryPage() {
  const navigate = useNavigate();
  const cache = useQueryClient();
  const providers = useQuery(historicalProvidersQuery());
  const imports = useQuery(historicalImportsQuery());
  const [provider, setProvider] = useState<"local_file" | "alpaca">("local_file");
  const [symbol, setSymbol] = useState("AAPL");
  const [feed, setFeed] = useState("fabricated_l1");
  const [start, setStart] = useState("");
  const [end, setEnd] = useState("");
  const [confirmed, setConfirmed] = useState(false);
  const submit = useMutation({
    mutationFn: () =>
      post<HistoricalImport>("/historical/imports", {
        provider,
        symbol,
        feed,
        start: provider === "alpaca" ? start : null,
        end: provider === "alpaca" ? end : null,
        confirm_access: confirmed,
      }),
    onSuccess: async () => {
      await cache.invalidateQueries({ queryKey: ["historical", "imports"] });
    },
  });

  const configured = providers.data?.providers.find((item) => item.id === provider);
  const startError = provider === "alpaca" ? utcInstantError(start) : null;
  const endError = provider === "alpaca" ? utcInstantError(end) : null;
  const rangeError =
    provider === "alpaca" && startError === null && endError === null && start !== "" && end !== ""
      ? Date.parse(start) >= Date.parse(end)
        ? "End must be after start"
        : null
      : null;
  function selectProvider(next: string) {
    const selected = next as "local_file" | "alpaca";
    setProvider(selected);
    setFeed(selected === "local_file" ? "fabricated_l1" : "iex");
    setConfirmed(false);
  }

  return (
    <Page
      scroll
      context={
        <ContextBar crumbs={[{ label: "Historical" }, { label: "Session library" }]}>
          <Button size="sm" onClick={() => void navigate("/historical/compare")}>
            Compare with synthetic
          </Button>
        </ContextBar>
      }
    >
      <div className="grid min-h-full grid-cols-1 gap-px bg-line-subtle lg:grid-cols-[minmax(0,22rem)_minmax(0,1fr)]">
        <InstrumentPanel
          index="01"
          title="Import workspace"
          subtitle="authorized acquisition only"
          className="min-h-[420px]"
          bodyClassName="overflow-auto"
        >
          <form
            className="flex min-w-0 flex-col gap-4 p-4"
            onSubmit={(event) => {
              event.preventDefault();
              submit.mutate();
            }}
          >
            <p className="text-sm text-fg-2">
              Python acquires immutable raw records. C++ validates prices, timestamps, ordering,
              playback, and digests. Imports never enter the synthetic exchange.
            </p>
            <SelectField
              label="Provider"
              value={provider}
              onChange={selectProvider}
              options={[
                { value: "local_file", label: "Local fabricated fixture" },
                { value: "alpaca", label: "Alpaca historical equities" },
              ]}
            />
            <TextField
              label="Symbol"
              value={symbol}
              onChange={(value) => setSymbol(value.toUpperCase())}
            />
            <SelectField
              label="Feed"
              value={feed}
              onChange={setFeed}
              options={(configured?.feeds ?? [feed]).map((value) => ({
                value,
                label: value.toUpperCase(),
              }))}
            />
            {provider === "alpaca" ? (
              <>
                <TextField
                  label="Start (UTC RFC3339)"
                  value={start}
                  onChange={setStart}
                  placeholder="2026-10-09T13:30:00Z"
                  error={startError}
                  mono
                />
                <TextField
                  label="End (UTC RFC3339)"
                  value={end}
                  onChange={setEnd}
                  placeholder="2026-10-09T13:35:00Z"
                  error={endError ?? rangeError}
                  mono
                />
                <Checkbox
                  label="I confirmed feed entitlement and possible provider charges"
                  checked={confirmed}
                  onChange={setConfirmed}
                />
              </>
            ) : null}
            <div className="border-l-2 border-warn bg-surface-1 p-2 text-xs text-fg-2">
              {configured?.cost ?? "Provider cost and entitlement information unavailable."}
            </div>
            <Button
              variant="solid"
              type="submit"
              disabled={
                submit.isPending ||
                !configured?.configured ||
                (provider === "alpaca" &&
                  (!confirmed ||
                    !start ||
                    !end ||
                    startError !== null ||
                    endError !== null ||
                    rangeError !== null))
              }
              className="self-start"
            >
              {submit.isPending ? "Submitting…" : "Import observations"}
            </Button>
            {submit.isError ? <ErrorState error={submit.error} compact /> : null}
          </form>
        </InstrumentPanel>

        <InstrumentPanel
          index="02"
          title="Historical session library"
          subtitle="observed records remain separate from simulations"
          className="min-h-[420px]"
          bodyClassName="overflow-auto"
        >
          {imports.isPending ? (
            <LoadingState rows={10} label="Loading historical imports" />
          ) : imports.isError ? (
            <ErrorState error={imports.error} onRetry={() => void imports.refetch()} />
          ) : (
            <table className="w-full border-collapse text-left text-sm">
              <thead className="sticky top-0 bg-surface-1">
                <tr className="label text-fg-3">
                  {["Symbol", "Provider / feed", "Coverage", "Status", "Created"].map((label) => (
                    <th key={label} className="border-b border-line px-3 py-2 font-medium">
                      {label}
                    </th>
                  ))}
                </tr>
              </thead>
              <tbody>
                {imports.data?.imports.length === 0 ? (
                  <tr>
                    <td colSpan={5} className="p-8 text-center text-sm text-fg-3">
                      No historical sessions are stored. Import the fabricated fixture to exercise
                      the complete offline pipeline.
                    </td>
                  </tr>
                ) : null}
                {imports.data?.imports.map((item) => (
                  <tr
                    key={item.id}
                    tabIndex={item.status === "completed" ? 0 : undefined}
                    className="cursor-pointer border-b border-line-subtle hover:bg-surface-hover"
                    onClick={() =>
                      item.status === "completed" && void navigate(`/historical/${item.id}`)
                    }
                    onKeyDown={(event) => {
                      if (event.key === "Enter" && item.status === "completed")
                        void navigate(`/historical/${item.id}`);
                    }}
                  >
                    <td className="num px-3 py-2 text-fg">{item.symbol}</td>
                    <td className="px-3 py-2 text-fg-2">
                      {item.provider} / {item.feed}
                    </td>
                    <td className="num px-3 py-2 text-xs text-fg-3">
                      {item.start_time && item.end_time
                        ? `${item.start_time} → ${item.end_time}`
                        : "bundled offline fixture"}
                    </td>
                    <td className="px-3 py-2">
                      <StatusIndicator spec={RUN_STATUS[item.status]} />
                      {item.error_code ? (
                        <span className="num ml-2 text-xs text-neg">{item.error_code}</span>
                      ) : null}
                    </td>
                    <td className="num px-3 py-2 text-xs text-fg-3">{item.created_at}</td>
                  </tr>
                ))}
              </tbody>
            </table>
          )}
        </InstrumentPanel>
      </div>
    </Page>
  );
}
