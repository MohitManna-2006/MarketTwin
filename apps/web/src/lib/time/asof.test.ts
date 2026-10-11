import { describe, expect, it } from "vitest";

import { indexAtOrAfter, indexAtOrBefore } from "./asof";

const times = [1_000, 2_000, 3_000, 5_000];

describe("as-of lookup", () => {
  it("returns the latest observation at or before the time", () => {
    expect(indexAtOrBefore(times, 1_000)).toBe(0);
    expect(indexAtOrBefore(times, 1_999)).toBe(0);
    expect(indexAtOrBefore(times, 2_000)).toBe(1);
    expect(indexAtOrBefore(times, 4_999)).toBe(2);
    expect(indexAtOrBefore(times, 9_999)).toBe(3);
  });

  it("has nothing to show before the first observation", () => {
    expect(indexAtOrBefore(times, 999)).toBe(-1);
    expect(indexAtOrBefore([], 5)).toBe(-1);
  });

  it("finds the first observation at or after a time", () => {
    expect(indexAtOrAfter(times, 0)).toBe(0);
    expect(indexAtOrAfter(times, 2_001)).toBe(2);
    expect(indexAtOrAfter(times, 5_001)).toBe(4);
  });
});
