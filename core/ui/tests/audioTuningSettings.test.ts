import { describe, expect, it } from "vitest";
import { normalizeAudioTuningState, periodsHint, periodsLabel, realtimePriorityHint, realtimePriorityLabel, realtimePriorityOptions, type AudioTuningState } from "../ts/settings/audioTuning.js";

const base = { type: "audioTuningState", available: true, realtimePriority: 70, realtimePriorityMax: 95, realtimePriorityApplied: 70, periodsSupported: true, periods: 4, minPeriods: 2, maxPeriods: 4, activePeriodSize: 64, activeBufferSize: 256, error: "" };

function state(overrides: Record<string, unknown> = {}): AudioTuningState {
  const normalized = normalizeAudioTuningState({ ...base, ...overrides });
  if (!normalized) {
    throw new Error("expected a state");
  }
  return normalized;
}

describe("audio tuning settings", () => {
  it("has nothing to show where the host has no audio tuning", () => {
    expect(normalizeAudioTuningState({ type: "audioTuningState", available: false })).toBeNull();
    expect(normalizeAudioTuningState(null)).toBeNull();
    expect(normalizeAudioTuningState({})).toBeNull();
  });

  it("reads the engine's snapshot", () => {
    expect(state()).toEqual({ realtimePriority: 70, realtimePriorityMax: 95, realtimePriorityApplied: 70, periodsSupported: true, periods: 4, minPeriods: 2, maxPeriods: 4, activePeriodSize: 64, activeBufferSize: 256, error: "" });
    // No device open: no buffer to report.
    expect(state({ activePeriodSize: undefined, activeBufferSize: undefined }).activeBufferSize).toBeNull();
    expect(state({ activePeriodSize: 64, activeBufferSize: 0 }).activePeriodSize).toBeNull();
    expect(state({ periods: 9 }).periods).toBe(4);
    expect(state({ realtimePriorityApplied: undefined }).realtimePriorityApplied).toBe(-1);
  });

  it("offers the priorities the system allows, and always the current one", () => {
    expect(realtimePriorityOptions(state())).toEqual([0, 10, 20, 30, 40, 50, 60, 70, 80, 90, 95]);
    expect(realtimePriorityOptions(state({ realtimePriorityMax: 50, realtimePriority: 50 }))).toEqual([0, 10, 20, 30, 40, 50]);
    expect(realtimePriorityOptions(state({ realtimePriority: 55 }))).toContain(55);
    expect(realtimePriorityOptions(state({ realtimePriorityMax: 0 }))).toEqual([0, 70]);
    expect(realtimePriorityLabel(0)).toBe("Off");
    expect(realtimePriorityLabel(70)).toBe("70");
  });

  it("says what the audio thread really has", () => {
    expect(realtimePriorityHint(state())).toContain("Active: the audio thread runs at realtime priority 70");
    expect(realtimePriorityHint(state({ realtimePriority: 80 }))).toBe("Asked for 80; the audio thread has realtime priority 70.");
    expect(realtimePriorityHint(state({ realtimePriorityApplied: 0 }))).toContain("refused");
    expect(realtimePriorityHint(state({ realtimePriority: 0, realtimePriorityApplied: 0 }))).toContain("ordinary thread");
    expect(realtimePriorityHint(state({ realtimePriorityApplied: -1 }))).toContain("when audio starts");
    expect(realtimePriorityHint(state({ realtimePriorityMax: 0, realtimePriorityApplied: 0 }))).toContain("does not let the app use realtime priority");
  });

  it("reports the device buffer the periods give", () => {
    expect(periodsHint(state())).toContain("Device buffer: 256 frames (4 × 64).");
    expect(periodsHint(state({ periods: 3, activeBufferSize: 192 }))).toContain("192 frames (3 × 64)");
    // A device that did not give what was asked for is reported as it is.
    expect(periodsHint(state({ activeBufferSize: 160 }))).toContain("(2.5 × 64)");
    expect(periodsHint(state({ activePeriodSize: 0, activeBufferSize: 0 }))).not.toContain("Device buffer");
    expect(periodsLabel(2)).toBe("2 (lowest latency)");
    expect(periodsLabel(3)).toBe("3");
    expect(periodsLabel(4)).toBe("4 (default)");
  });
});
