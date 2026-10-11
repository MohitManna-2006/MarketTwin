import { render, screen } from "@testing-library/react";
import { describe, expect, it } from "vitest";

import { ApiError, BackendUnavailableError } from "@/lib/api/client";

import { MetricReadout } from "./MetricReadout";
import { EmptyState, ErrorState } from "./States";
import {
  EXPERIMENT_STATUS,
  REPLAY_STATUS,
  RUN_STATUS,
  StatusIndicator,
  type StatusSpec,
} from "./StatusIndicator";

describe("StatusIndicator", () => {
  it("always says the status in words, whatever the colour", () => {
    for (const spec of [
      ...Object.values(RUN_STATUS),
      ...Object.values(REPLAY_STATUS),
      ...Object.values(EXPERIMENT_STATUS),
    ]) {
      const { unmount } = render(<StatusIndicator spec={spec} />);
      expect(screen.getByText(spec.label)).toBeInTheDocument();
      unmount();
    }
  });

  it("gives states that share a colour different glyphs", () => {
    const glyph = (spec: StatusSpec | undefined) => {
      const { container, unmount } = render(<StatusIndicator spec={spec} />);
      const markup = container.querySelector("svg")?.innerHTML;
      unmount();
      return markup;
    };
    expect(glyph(REPLAY_STATUS.mismatch)).not.toBe(glyph(REPLAY_STATUS.error));
    expect(glyph(RUN_STATUS.completed)).not.toBe(glyph(RUN_STATUS.failed));
  });

  it("does not guess at an unknown status", () => {
    render(<StatusIndicator spec={undefined} />);
    expect(screen.getByText("Unknown")).toBeInTheDocument();
  });
});

describe("MetricReadout", () => {
  it("announces a missing value as unavailable and shows no unit for it", () => {
    render(<MetricReadout label="VWAP" value="—" unit="ticks" />);
    expect(screen.getByLabelText("VWAP: unavailable")).toHaveTextContent("—");
    expect(screen.queryByText("ticks")).not.toBeInTheDocument();
  });

  it("shows zero as zero, with its unit", () => {
    render(<MetricReadout label="Fees paid" value="0" unit="minor" />);
    expect(screen.getByText("0")).toBeInTheDocument();
    expect(screen.getByText("minor")).toBeInTheDocument();
  });
});

describe("ErrorState", () => {
  it("shows the engine's own code and message", () => {
    render(
      <ErrorState
        error={
          new ApiError(409, "RunNotCompleted", "run run-1 is failed and has no artifacts", null)
        }
      />,
    );
    expect(screen.getByRole("alert")).toHaveTextContent("RunNotCompleted");
    expect(screen.getByRole("alert")).toHaveTextContent("run run-1 is failed and has no artifacts");
  });

  it("lists field errors with their paths", () => {
    const error = new ApiError(422, "InvalidConfig", "the run config is not valid", [
      {
        source: "schema",
        path: "scenario.agents.2.policy.levels",
        message: "Input should be less than or equal to 32",
      },
    ]);
    render(<ErrorState error={error} />);
    expect(screen.getByText("scenario.agents.2.policy.levels")).toBeInTheDocument();
  });

  it("says the API is unreachable and that nothing recorded is being shown", () => {
    render(<ErrorState error={new BackendUnavailableError(new TypeError("fetch failed"))} />);
    const alert = screen.getByRole("alert");
    expect(alert).toHaveTextContent("RESEARCH API UNREACHABLE");
    expect(alert).toHaveTextContent("No recorded data is shown while the API is unreachable.");
  });
});

describe("EmptyState", () => {
  it("names what is absent and offers the next action", () => {
    render(
      <EmptyState
        title="No simulation selected"
        actions={<button type="button">New simulation</button>}
      >
        Choose an existing run or start a new simulation.
      </EmptyState>,
    );
    expect(screen.getByText("No simulation selected")).toBeInTheDocument();
    expect(screen.getByRole("button", { name: "New simulation" })).toBeInTheDocument();
  });
});
