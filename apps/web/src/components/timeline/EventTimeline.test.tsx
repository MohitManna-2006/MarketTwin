import { render, screen } from "@testing-library/react";
import userEvent from "@testing-library/user-event";
import { beforeEach, describe, expect, it } from "vitest";

import { TooltipProvider } from "@/components/ui/Tooltip";
import { usePlayback } from "@/state/playback";

import { EventTimeline } from "./EventTimeline";

const SECOND = 1_000_000_000;
const times = Array.from({ length: 600 }, (_, index) => (index + 1) * SECOND);

function timeline() {
  return (
    <TooltipProvider>
      <EventTimeline times={times} events={[{ ns: 300 * SECOND, label: "Fundamental jump" }]} />
      <input aria-label="Run name" />
      <button type="button">Elsewhere</button>
    </TooltipProvider>
  );
}

describe("EventTimeline", () => {
  beforeEach(() => {
    usePlayback.setState({ key: "run", count: times.length, index: 99, playing: false, speed: 1 });
  });

  it("states the cursor as elapsed time and as a recorded sample", () => {
    render(timeline());
    expect(screen.getByText("00:01:40.000")).toBeInTheDocument();
    expect(screen.getByRole("slider")).toHaveAttribute(
      "aria-valuetext",
      "00:01:40.000 elapsed, sample 100 of 600",
    );
  });

  it("steps, jumps, and plays from the keyboard", async () => {
    const user = userEvent.setup();
    render(timeline());
    await user.keyboard("{ArrowRight}");
    expect(usePlayback.getState().index).toBe(100);
    await user.keyboard("{ArrowLeft}{ArrowLeft}");
    expect(usePlayback.getState().index).toBe(98);
    await user.keyboard("{Shift>}{ArrowRight}{/Shift}");
    expect(usePlayback.getState().index).toBe(158);
    await user.keyboard("{End}");
    expect(usePlayback.getState().index).toBe(599);
    await user.keyboard("{Home}");
    expect(usePlayback.getState().index).toBe(0);
    await user.keyboard(" ");
    expect(usePlayback.getState().playing).toBe(true);
    await user.keyboard("]");
    expect(usePlayback.getState().speed).toBe(2);
  });

  it("leaves the keys alone while the reader types or uses another control", async () => {
    const user = userEvent.setup();
    render(timeline());
    await user.click(screen.getByLabelText("Run name"));
    await user.keyboard("a b{ArrowLeft}{Home}");
    expect(usePlayback.getState()).toMatchObject({ index: 99, playing: false });
    screen.getByRole("button", { name: "Elsewhere" }).focus();
    await user.keyboard(" ");
    expect(usePlayback.getState().playing).toBe(false);
  });

  it("moves with the transport buttons", async () => {
    const user = userEvent.setup();
    render(timeline());
    await user.click(screen.getByRole("button", { name: "Next sample" }));
    expect(usePlayback.getState().index).toBe(100);
    await user.click(screen.getByRole("button", { name: "Last sample" }));
    expect(usePlayback.getState().index).toBe(599);
    expect(screen.getByRole("button", { name: "Next sample" })).toBeDisabled();
  });
});
