/**
 * Settings > Audio & MIDI: the realtime priority of the audio thread and the number of
 * ALSA periods, for the standalone app on Linux.
 *
 * With the buffer size above them these are the three numbers that decide latency and
 * dropouts on a small board: how big a block is, how many of them the device holds, and
 * whether the thread that fills them goes ahead of everything else. The engine side is
 * juce/source/StandaloneAudioTuning. Every request is answered with a whole
 * `audioTuningState`, and the two rows are redrawn from it; where the host has none
 * (`available: false`) they stay hidden.
 */

import { postMessage } from "../bridge.js";

export interface AudioTuningState {
  /** The priority chosen: 0 is off, an ordinary thread. */
  realtimePriority: number;
  /** The highest the system lets the app ask for; 0 when it may not use realtime at all. */
  realtimePriorityMax: number;
  /** What the audio thread actually has; 0 for an ordinary thread, -1 before audio has run. */
  realtimePriorityApplied: number;
  /** False where this build cannot change the periods. */
  periodsSupported: boolean;
  periods: number;
  minPeriods: number;
  maxPeriods: number;
  /** The open device's block and buffer, in frames, as the kernel reports them; null when unknown. */
  activePeriodSize: number | null;
  activeBufferSize: number | null;
  error: string;
}

/** The priorities offered. One the state holds that is not here is added to the list. */
export const REALTIME_PRIORITY_CHOICES = [0, 10, 20, 30, 40, 50, 60, 70, 80, 90, 95];

let initialized = false;
let requested = false;
let available = false;
let current: AudioTuningState | null = null;
let refreshTimer: ReturnType<typeof setTimeout> | null = null;

function el<T extends HTMLElement = HTMLElement>(id: string): T | null {
  return document.getElementById(id) as T | null;
}

function request(action: string, args: Record<string, unknown> = {}): void {
  postMessage({ type: "audioTuning", action, ...args });
}

function integer(value: unknown, fallback: number): number {
  return typeof value === "number" && Number.isFinite(value) ? Math.round(value) : fallback;
}

/** The engine's snapshot, or nothing where the host has no audio tuning (`available: false`). */
export function normalizeAudioTuningState(raw: unknown): AudioTuningState | null {
  if (typeof raw !== "object" || raw === null) {
    return null;
  }
  const payload = raw as Record<string, unknown>;
  if (payload.available !== true) {
    return null;
  }
  const minPeriods = Math.max(1, integer(payload.minPeriods, 2));
  const maxPeriods = Math.max(minPeriods, integer(payload.maxPeriods, 4));
  const periodSize = integer(payload.activePeriodSize, 0);
  const bufferSize = integer(payload.activeBufferSize, 0);
  return {
    realtimePriority: Math.max(0, integer(payload.realtimePriority, 0)),
    realtimePriorityMax: Math.max(0, integer(payload.realtimePriorityMax, 0)),
    realtimePriorityApplied: integer(payload.realtimePriorityApplied, -1),
    periodsSupported: payload.periodsSupported === true,
    periods: Math.min(maxPeriods, Math.max(minPeriods, integer(payload.periods, 4))),
    minPeriods,
    maxPeriods,
    activePeriodSize: periodSize > 0 && bufferSize > 0 ? periodSize : null,
    activeBufferSize: periodSize > 0 && bufferSize > 0 ? bufferSize : null,
    error: typeof payload.error === "string" ? payload.error : "",
  };
}

/** The priorities to list: the standard ones the system allows, and the current one. */
export function realtimePriorityOptions(state: AudioTuningState): number[] {
  const options = REALTIME_PRIORITY_CHOICES.filter((priority) => priority <= state.realtimePriorityMax);
  if (!options.includes(state.realtimePriority)) {
    options.push(state.realtimePriority);
  }
  return options.sort((a, b) => a - b);
}

export function realtimePriorityLabel(priority: number): string {
  return priority <= 0 ? "Off" : String(priority);
}

/** The line under the priority: what the audio thread has, which may not be what was asked. */
export function realtimePriorityHint(state: AudioTuningState): string {
  if (state.realtimePriorityMax <= 0) {
    return "This system does not let the app use realtime priority. Start it with run_pi_kiosk.sh, or add your user to the audio group.";
  }
  const asked = state.realtimePriority;
  const has = state.realtimePriorityApplied;
  if (has < 0) {
    return asked > 0 ? `The audio thread gets realtime priority ${asked} when audio starts.` : "Off: the audio thread will run as an ordinary thread.";
  }
  if (asked <= 0) {
    return has > 0 ? `Turning off: the audio thread still has realtime priority ${has}.` : "Off: the audio thread runs as an ordinary thread and waits its turn, which can cause dropouts at small buffer sizes.";
  }
  if (has === asked) {
    return `Active: the audio thread runs at realtime priority ${has}, ahead of the screen and other programs. Higher is more urgent; 70 suits most systems.`;
  }
  return has > 0 ? `Asked for ${asked}; the audio thread has realtime priority ${has}.` : `Asked for ${asked}, but the audio thread has no realtime priority. The system refused it.`;
}

export function periodsLabel(periods: number): string {
  if (periods === 2) {
    return "2 (lowest latency)";
  }
  return periods === 4 ? "4 (default)" : String(periods);
}

/** The line under the periods: the buffer the device really has, and what the number trades. */
export function periodsHint(state: AudioTuningState): string {
  const trade = "Fewer periods give less output latency and less margin for a late block. Changing it reopens the audio device.";
  if (state.activePeriodSize === null || state.activeBufferSize === null) {
    return trade;
  }
  const actual = state.activeBufferSize / state.activePeriodSize;
  const count = Number.isInteger(actual) ? String(actual) : actual.toFixed(1);
  return `Device buffer: ${state.activeBufferSize} frames (${count} × ${state.activePeriodSize}). ${trade}`;
}

function fillSelect(select: HTMLSelectElement, values: number[], selected: number, label: (value: number) => string): void {
  const wanted = values.map((value) => `${value}:${label(value)}`).join("|");
  if (select.dataset.options !== wanted) {
    select.innerHTML = "";
    for (const value of values) {
      const option = document.createElement("option");
      option.value = String(value);
      option.textContent = label(value);
      select.appendChild(option);
    }
    select.dataset.options = wanted;
  }
  select.value = String(selected);
}

function setHidden(id: string, hidden: boolean): void {
  const element = el(id);
  if (element) {
    element.hidden = hidden;
  }
}

function render(): void {
  const state = current;
  setHidden("audio-tuning-priority-row", state === null);
  setHidden("audio-tuning-priority-hint", state === null);
  setHidden("audio-tuning-periods-row", state === null || !state.periodsSupported);
  setHidden("audio-tuning-periods-hint", state === null || !state.periodsSupported);
  setHidden("audio-tuning-error", state === null || state.error === "");
  if (!state) {
    return;
  }

  const periods = el<HTMLSelectElement>("audio-tuning-periods");
  if (periods) {
    const values: number[] = [];
    for (let count = state.minPeriods; count <= state.maxPeriods; count += 1) {
      values.push(count);
    }
    fillSelect(periods, values, state.periods, periodsLabel);
  }
  const periodsText = el("audio-tuning-periods-hint");
  if (periodsText) {
    periodsText.textContent = periodsHint(state);
  }

  const priority = el<HTMLSelectElement>("audio-tuning-priority");
  if (priority) {
    fillSelect(priority, realtimePriorityOptions(state), state.realtimePriority, realtimePriorityLabel);
    priority.disabled = state.realtimePriorityMax <= 0;
  }
  const priorityText = el("audio-tuning-priority-hint");
  if (priorityText) {
    priorityText.textContent = realtimePriorityHint(state);
  }

  const error = el("audio-tuning-error");
  if (error) {
    error.textContent = state.error;
  }
}

/** An `audioTuningState` from the engine: the whole state, or `available: false`. */
export function onAudioTuningState(payload: Record<string, unknown>): void {
  current = normalizeAudioTuningState(payload);
  available = current !== null;
  render();
}

/**
 * Asks for the state once, when the app turns out to be the standalone one. A plugin runs on
 * its host's audio thread and device, so there the rows stay hidden without asking.
 */
export function syncAudioTuningAvailability(isStandalone: boolean): void {
  if (!isStandalone || requested) {
    return;
  }
  requested = true;
  request("getState");
}

/**
 * Asks again shortly: for when the audio device changed (another buffer size, another
 * device), which changes the buffer the periods line reports. Several changes in a row
 * make one request.
 */
export function refreshAudioTuningSoon(): void {
  if (!available || refreshTimer !== null) {
    return;
  }
  refreshTimer = setTimeout(() => {
    refreshTimer = null;
    request("getState");
  }, 300);
}

export function initAudioTuningSettings(): void {
  if (initialized) {
    return;
  }
  initialized = true;

  el<HTMLSelectElement>("audio-tuning-priority")?.addEventListener("change", (event) => {
    const priority = Number.parseInt((event.target as HTMLSelectElement).value, 10);
    if (Number.isFinite(priority)) {
      request("setRealtimePriority", { priority });
    }
  });

  el<HTMLSelectElement>("audio-tuning-periods")?.addEventListener("change", (event) => {
    const periods = Number.parseInt((event.target as HTMLSelectElement).value, 10);
    if (Number.isFinite(periods)) {
      request("setPeriods", { periods });
    }
  });

  render();
}
