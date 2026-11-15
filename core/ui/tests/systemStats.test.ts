import { describe, expect, it } from "vitest";
import { formatCpuPercent, formatTemperature, formatXruns, normalizeSystemStats, xrunsSinceReset } from "../ts/systemStats.js";

describe("footer system load readout", () => {
  it("has nothing to show where the host has no figures", () => {
    expect(normalizeSystemStats({ type: "systemStats", available: false })).toBeNull();
    expect(normalizeSystemStats({ available: true })).toBeNull();
    expect(normalizeSystemStats(null)).toBeNull();
  });

  it("reads the figures the host sends and leaves out the rest", () => {
    expect(normalizeSystemStats({ available: true, cpuPercent: 23.4, temperatureC: 54.3, xruns: 3 })).toEqual({ cpuPercent: 23.4, temperatureC: 54.3, xruns: 3 });
    expect(normalizeSystemStats({ available: true, cpuPercent: 20 })).toEqual({ cpuPercent: 20, temperatureC: null, xruns: null });
    expect(normalizeSystemStats({ available: true, cpuPercent: 140, xruns: -1 })).toEqual({ cpuPercent: 100, temperatureC: null, xruns: null });
    expect(normalizeSystemStats({ available: true, cpuPercent: "12", temperatureC: 40 })).toEqual({ cpuPercent: null, temperatureC: 40, xruns: null });
  });

  it("counts dropouts from the last reset", () => {
    expect(xrunsSinceReset(7, 0)).toBe(7);
    expect(xrunsSinceReset(7, 5)).toBe(2);
    expect(xrunsSinceReset(5, 5)).toBe(0);
    // The device was reopened and counts from nothing again: the old baseline means nothing now.
    expect(xrunsSinceReset(2, 5)).toBe(2);
    expect(xrunsSinceReset(0, 5)).toBe(0);
  });

  it("formats each figure for the bar", () => {
    expect(formatCpuPercent(23.4)).toBe("23%");
    expect(formatCpuPercent(99.6)).toBe("100%");
    expect(formatTemperature(54.5)).toBe("55°C");
    expect(formatXruns(0)).toBe("0");
    expect(formatXruns(12)).toBe("12");
    expect(formatXruns(123456)).toBe("9999+");
  });
});
