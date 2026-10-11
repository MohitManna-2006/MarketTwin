import { beforeEach, describe, expect, it } from "vitest";

import { usePlayback } from "./playback";

const state = () => usePlayback.getState();

describe("playback cursor", () => {
  beforeEach(() => {
    usePlayback.setState({ key: null, count: 0, index: 0, playing: false, speed: 1 });
  });

  it("attaches at the end of a run unless told where to start", () => {
    state().attach("run-a", 3600);
    expect(state().index).toBe(3599);
    state().attach("run-b", 3600, 1500);
    expect(state().index).toBe(1500);
    // Attaching the same run again leaves the cursor where the reader put it.
    state().seek(10);
    state().attach("run-b", 3600, 1500);
    expect(state().index).toBe(10);
  });

  it("stays on a recorded sample at both ends", () => {
    state().attach("run", 10, 0);
    state().step(-1);
    expect(state().index).toBe(0);
    state().seek(99);
    expect(state().index).toBe(9);
    state().step(1);
    expect(state().index).toBe(9);
    state().seek(4.6);
    expect(state().index).toBe(5);
  });

  it("pauses when stepped by hand and stops at the last sample", () => {
    state().attach("run", 10, 0);
    state().play();
    expect(state().playing).toBe(true);
    state().advance(4);
    expect(state()).toMatchObject({ index: 4, playing: true });
    state().step(1);
    expect(state()).toMatchObject({ index: 5, playing: false });
    state().play();
    state().advance(100);
    expect(state()).toMatchObject({ index: 9, playing: false });
  });

  it("plays from the start when the cursor is already at the end", () => {
    state().attach("run", 10);
    state().play();
    expect(state()).toMatchObject({ index: 0, playing: true });
  });

  it("does not play a run with fewer than two samples", () => {
    state().attach("run", 1);
    state().toggle();
    expect(state().playing).toBe(false);
  });

  it("moves through the offered paces and no further", () => {
    state().setSpeed(1);
    state().faster();
    expect(state().speed).toBe(2);
    state().setSpeed(10);
    state().faster();
    expect(state().speed).toBe(10);
    state().setSpeed(0.5);
    state().slower();
    expect(state().speed).toBe(0.5);
  });
});
