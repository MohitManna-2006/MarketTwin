import { useQuery } from "@tanstack/react-query";
import { Keyboard, Settings2 } from "lucide-react";
import { DropdownMenu } from "radix-ui";
import { useState } from "react";
import { NavLink } from "react-router";

import { Dialog } from "@/components/ui/Dialog";
import { KeyValue } from "@/components/ui/KeyValue";
import { StatusIndicator, type StatusSpec } from "@/components/ui/StatusIndicator";
import { Tooltip } from "@/components/ui/Tooltip";
import { useHotkeys } from "@/hooks/useHotkeys";
import { healthQuery } from "@/lib/api/queries";
import { cx } from "@/lib/cx";
import { formatSpan } from "@/lib/formatting";

import { ShortcutHelp } from "./ShortcutHelp";
import { Wordmark } from "./Wordmark";

const SECTIONS = [
  { to: "/", label: "Overview", end: true },
  { to: "/simulations", label: "Simulations", match: ["/simulations", "/runs"] },
  { to: "/experiments", label: "Experiments" },
  { to: "/replay", label: "Replay" },
];

const CONNECTED: StatusSpec = { label: "API connected", tone: "positive", glyph: "dot" };
const CONNECTING: StatusSpec = { label: "API connecting", tone: "muted", glyph: "ring" };
const UNREACHABLE: StatusSpec = { label: "API unreachable", tone: "negative", glyph: "cross" };

export function TopNavigation() {
  const health = useQuery(healthQuery());
  const [help, setHelp] = useState(false);
  const [about, setAbout] = useState(false);
  useHotkeys({ "?": () => setHelp(true) });

  const connection = health.isSuccess ? CONNECTED : health.isError ? UNREACHABLE : CONNECTING;
  const data = health.data;

  return (
    <header className="flex h-[var(--bar-top)] shrink-0 items-stretch border-b border-line bg-surface-0">
      <NavLink
        to="/"
        className="flex items-center px-4"
        aria-label="MarketTwin research terminal, overview"
      >
        <Wordmark />
      </NavLink>
      <nav
        aria-label="Sections"
        className="ml-0 flex min-w-0 items-stretch overflow-x-auto md:ml-4"
      >
        {SECTIONS.map((section) => (
          <NavLink
            key={section.to}
            to={section.to}
            end={section.end}
            className={({ isActive }) => {
              const active =
                isActive ||
                (section.match?.some((prefix) => window.location.pathname.startsWith(prefix)) ??
                  false);
              return cx(
                "transition-hover label flex items-center px-3.5",
                active
                  ? "text-fg shadow-[inset_0_-2px_0_var(--accent-primary)]"
                  : "text-fg-3 hover:text-fg-2",
              );
            }}
          >
            {section.label}
          </NavLink>
        ))}
      </nav>
      <span className="flex-1" />
      <div className="flex shrink-0 items-center gap-4 pr-2 pl-2 md:pl-4">
        {data && data.active_jobs > 0 ? (
          <span className="label flex items-baseline gap-1.5 text-accent">
            <span className="num tracking-normal">{data.active_jobs}</span>
            {data.active_jobs === 1 ? "active job" : "active jobs"}
          </span>
        ) : null}
        <Tooltip
          content={
            data
              ? `C++ core ${data.engine_version}, ${data.build_type} build ${data.git_revision}`
              : "Engine version is reported by the API"
          }
        >
          <span className="label hidden items-baseline gap-1.5 text-fg-3 md:flex">
            Engine
            <span className="num text-sm font-medium tracking-normal text-fg">
              {data?.engine_version ?? "—"}
            </span>
          </span>
        </Tooltip>
        <span className="label hidden items-baseline gap-1.5 text-fg-3 lg:flex">
          Workspace
          <span className="num text-sm font-medium tracking-normal text-fg-2 normal-case">
            {data?.workspace ?? "—"}
          </span>
        </span>
        <StatusIndicator spec={connection} className="max-sm:[&>.label]:sr-only" />
        <DropdownMenu.Root>
          <DropdownMenu.Trigger
            aria-label="Terminal settings"
            className="transition-hover inline-flex size-7 items-center justify-center rounded-md text-fg-3 hover:bg-surface-hover hover:text-fg"
          >
            <Settings2 size={15} aria-hidden />
          </DropdownMenu.Trigger>
          <DropdownMenu.Portal>
            <DropdownMenu.Content
              align="end"
              sideOffset={6}
              className="z-50 min-w-[220px] rounded-md border border-line-strong bg-surface-1 py-1 text-sm"
            >
              <DropdownMenu.Item
                onSelect={() => setHelp(true)}
                className="flex cursor-pointer items-center gap-2 px-3 py-1.5 text-fg-2 outline-none data-[highlighted]:bg-surface-hover data-[highlighted]:text-fg"
              >
                <Keyboard size={13} aria-hidden />
                Keyboard and pointer
                <span className="num ml-auto text-xs text-fg-3">?</span>
              </DropdownMenu.Item>
              <DropdownMenu.Item
                onSelect={() => setAbout(true)}
                className="flex cursor-pointer items-center gap-2 px-3 py-1.5 text-fg-2 outline-none data-[highlighted]:bg-surface-hover data-[highlighted]:text-fg"
              >
                <Settings2 size={13} aria-hidden />
                Engine and service
              </DropdownMenu.Item>
            </DropdownMenu.Content>
          </DropdownMenu.Portal>
        </DropdownMenu.Root>
      </div>
      <ShortcutHelp open={help} onOpenChange={setHelp} />
      <Dialog open={about} onOpenChange={setAbout} title="Engine and service" width={520}>
        {data ? (
          <KeyValue
            rows={[
              { label: "Engine", value: data.engine_version },
              { label: "Source revision", value: data.git_revision },
              { label: "Compiler", value: data.compiler },
              { label: "Build type", value: data.build_type },
              { label: "API", value: `v${data.api_version}` },
              { label: "Workspace", value: data.workspace },
              {
                label: "Stream schemas",
                value: `accounting ${data.schemas.accounting} · metrics ${data.schemas.metrics} · experiment ${data.schemas.experiment}`,
              },
              { label: "Concurrent jobs", value: data.max_concurrent_jobs },
              { label: "Longest horizon", value: formatSpan(data.limits.max_horizon_ns) },
              {
                label: "Largest experiment",
                value: `${(data.limits.max_experiment_runs ?? 0).toLocaleString("en-US")} runs`,
              },
            ]}
          />
        ) : (
          <p className="p-3 text-sm text-fg-3">
            The API is not reachable, so nothing is known about the engine.
          </p>
        )}
      </Dialog>
    </header>
  );
}
