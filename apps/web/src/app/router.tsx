import { createBrowserRouter } from "react-router";

import { ContextBar } from "@/components/shell/ContextBar";
import { Page, TerminalShell } from "@/components/shell/TerminalShell";
import { Button } from "@/components/ui/Button";
import { EmptyState } from "@/components/ui/States";
import { OverviewPage } from "@/features/overview/OverviewPage";

function NotFound() {
  return (
    <Page context={<ContextBar crumbs={[{ label: "Not found" }]} />}>
      <div className="h-full bg-surface-0">
        <EmptyState
          title="No screen at this address"
          actions={
            <Button size="sm" variant="primary" onClick={() => window.location.assign("/")}>
              Open the overview
            </Button>
          }
        >
          The terminal has four sections: Overview, Simulations, Experiments, and Replay.
        </EmptyState>
      </div>
    </Page>
  );
}

// Each section is loaded when it is first opened, so the overview does not wait for the
// charting code of the run workspace.
export function createRouter() {
  return createBrowserRouter([
    {
      element: <TerminalShell />,
      children: [
        { index: true, element: <OverviewPage /> },
        {
          path: "simulations",
          lazy: async () => ({
            Component: (await import("@/features/simulations/SimulationsPage")).SimulationsPage,
          }),
        },
        {
          path: "simulations/new",
          lazy: async () => ({
            Component: (await import("@/features/simulations/SimulationLabPage")).SimulationLabPage,
          }),
        },
        {
          path: "runs/:runId",
          lazy: async () => ({ Component: (await import("@/features/runs/RunLayout")).RunLayout }),
          children: [
            {
              index: true,
              lazy: async () => ({
                Component: (await import("@/features/market/MicroscopePage")).MicroscopePage,
              }),
            },
            {
              path: "agents",
              lazy: async () => ({
                Component: (await import("@/features/agents/AgentInspectorPage"))
                  .AgentInspectorPage,
              }),
            },
            {
              path: "agents/:agentId",
              lazy: async () => ({
                Component: (await import("@/features/agents/AgentInspectorPage"))
                  .AgentInspectorPage,
              }),
            },
            {
              path: "replay",
              lazy: async () => ({
                Component: (await import("@/features/replay/ReplayInspectorPage"))
                  .ReplayInspectorPage,
              }),
            },
          ],
        },
        {
          path: "experiments",
          lazy: async () => ({
            Component: (await import("@/features/experiments/ExperimentsPage")).ExperimentsPage,
          }),
        },
        {
          path: "experiments/new",
          lazy: async () => ({
            Component: (await import("@/features/experiments/ExperimentNewPage")).ExperimentNewPage,
          }),
        },
        {
          path: "experiments/:experimentId",
          lazy: async () => ({
            Component: (await import("@/features/experiments/ExperimentPage")).ExperimentPage,
          }),
        },
        {
          path: "experiments/:experimentId/seeds/:seed",
          lazy: async () => ({
            Component: (await import("@/features/experiments/SeedPairPage")).SeedPairPage,
          }),
        },
        {
          path: "replay",
          lazy: async () => ({
            Component: (await import("@/features/replay/ReplayIndexPage")).ReplayIndexPage,
          }),
        },
        { path: "*", element: <NotFound /> },
      ],
    },
  ]);
}
