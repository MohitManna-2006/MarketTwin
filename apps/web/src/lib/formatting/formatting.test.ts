import { describe, expect, it } from "vitest";

import {
  UNAVAILABLE,
  axisNumber,
  formatDuration,
  formatElapsed,
  formatElapsedAuto,
  formatInteger,
  formatMetricValue,
  formatMilliOf,
  formatMilliticks,
  formatPpmAsPercent,
  formatScaled,
  formatSpan,
  formatTwiceTicks,
  formatWallClock,
  toneOf,
} from ".";

describe("recorded integers", () => {
  it("groups thousands and uses a real minus sign", () => {
    expect(formatInteger(15004)).toBe("15,004");
    expect(formatInteger(-830)).toBe("−830");
    expect(formatInteger(12450, { signed: true })).toBe("+12,450");
  });

  it("keeps missing, zero, and invalid apart", () => {
    expect(formatInteger(null)).toBe(UNAVAILABLE);
    expect(formatInteger(undefined)).toBe(UNAVAILABLE);
    expect(formatInteger(0)).toBe("0");
    expect(formatInteger(0, { signed: true })).toBe("0");
    expect(formatInteger(Number.NaN)).toBe("INVALID");
    expect(formatInteger(2 ** 53)).toBe("INVALID");
  });

  it("gives direction only to a nonzero value", () => {
    expect(toneOf(5)).toBe("positive");
    expect(toneOf(-5)).toBe("negative");
    expect(toneOf(0)).toBe("neutral");
    expect(toneOf(null)).toBe("neutral");
  });
});

describe("fixed-point values, without floating-point division", () => {
  it("prints thousandths exactly", () => {
    expect(formatScaled(45_609_100, 3)).toBe("45,609.100");
    expect(formatScaled(-66_655_100, 3, { digits: 1 })).toBe("−66,655.1");
    expect(formatScaled(5, 3)).toBe("0.005");
    expect(formatScaled(null, 3)).toBe(UNAVAILABLE);
  });

  it("rounds half away from zero, as the engine does", () => {
    expect(formatScaled(1_250, 3, { digits: 1 })).toBe("1.3");
    expect(formatScaled(-1_250, 3, { digits: 1 })).toBe("−1.3");
    expect(formatScaled(1_249, 3, { digits: 1 })).toBe("1.2");
    expect(formatScaled(-4, 3, { digits: 0 })).toBe("0");
  });

  it("prints a half-tick midpoint from its doubled integer", () => {
    expect(formatTwiceTicks(30_001)).toBe("15,000.5");
    expect(formatTwiceTicks(30_000)).toBe("15,000");
    expect(formatTwiceTicks(30_000, { fixed: true })).toBe("15,000.0");
    expect(formatTwiceTicks(null)).toBe(UNAVAILABLE);
  });

  it("prints milliticks as ticks and ppm as percent", () => {
    expect(formatMilliticks(15_000_149)).toBe("15,000.149");
    expect(formatMilliticks(-3_250, { signed: true })).toBe("−3.250");
    expect(formatPpmAsPercent(121_794)).toBe("12.18%");
    expect(formatPpmAsPercent(999_999, 4)).toBe("99.9999%");
    expect(formatPpmAsPercent(null)).toBe(UNAVAILABLE);
  });

  it("folds fixed-point metric units into their base unit", () => {
    // A mean in thousandths of millishares is shares to three decimals.
    expect(formatMilliOf("millishares", 223_417_000)).toBe("223.417");
    expect(formatMilliOf("milliticks", -140_265, { signed: true })).toBe("−0.140");
    expect(formatMilliOf("minor", 45_609_100, { signed: true })).toBe("+45,609.1");
    expect(formatMilliOf("ppm", 125_298_000)).toBe("12.530");
    expect(formatMetricValue("millishares", 219_382)).toBe("219.382");
    expect(formatMetricValue("minor", -57_308, { signed: true })).toBe("−57,308");
  });
});

describe("elapsed simulation time", () => {
  it("is a duration since the start of the run, never a time of day", () => {
    expect(formatElapsed(0)).toBe("00:00:00.000");
    expect(formatElapsed(1_800_000_000_000, "s")).toBe("00:30:00");
    expect(formatElapsed(1_002_304_274, "ms")).toBe("00:00:01.002");
    expect(formatElapsed(1_002_304_274, "ns")).toBe("00:00:01.002 304 274");
    expect(formatElapsed(null)).toBe(UNAVAILABLE);
  });

  it("shows nanoseconds only where a timestamp has them", () => {
    expect(formatElapsedAuto(1_000_000_000)).toBe("00:00:01.000");
    expect(formatElapsedAuto(232_649)).toBe("00:00:00.000 232 649");
  });

  it("names configured durations in their largest exact unit", () => {
    expect(formatDuration(500_000_000)).toBe("500 ms");
    expect(formatDuration(3_600_000_000_000)).toBe("1 h");
    expect(formatDuration(200_000)).toBe("200 µs");
    expect(formatDuration(1_500_000_001)).toBe("1,500,000,001 ns");
    expect(formatSpan(5_400_000_000_000)).toBe("1 h 30 min");
    expect(formatSpan(100)).toBe("100 ns");
  });

  it("labels wall-clock time as UTC", () => {
    expect(formatWallClock("2026-10-11T18:22:13.132Z")).toBe("2026-10-11 18:22:13Z");
    expect(formatWallClock(null)).toBe(UNAVAILABLE);
  });
});

describe("axis ticks", () => {
  it("never prints a negative zero", () => {
    expect(axisNumber(-0.0001, 1)).toBe("0.0");
    expect(axisNumber(-1250.5, 1)).toBe("−1,250.5");
  });
});
