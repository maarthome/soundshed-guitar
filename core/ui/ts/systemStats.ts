/**
 * The system load readout in the middle of the footer bar: processor load, processor
 * temperature and audio dropouts (xruns).
 *
 * On a small board these are what explain crackling: the processor is busy, it is hot
 * enough to slow itself down, or audio blocks are arriving late. The figures come from
 * the standalone app on Linux (juce/source/StandaloneSystemStats), once a second after
 * this module asks for them; everywhere else the answer is `available: false` and the
 * readout stays hidden. A figure the machine cannot give is left out, and so is its
 * place on the bar.
 */

import { postMessage } from "./bridge.js";

export interface SystemStats {
  /** All cores together, averaged over the last few seconds; null when unknown. */
  cpuPercent: number | null;
  /** Degrees Celsius; null with no sensor. */
  temperatureC: number | null;
  /** The audio device's dropouts since it was opened; null with no device. */
  xruns: number | null;
}

/** From here the processor is close to slowing itself down (a Pi throttles at 80 to 85 °C). */
export const SYSTEM_STATS_HOT_C = 75;

/** A load this high leaves little room for the audio thread. */
export const SYSTEM_STATS_BUSY_PERCENT = 85;

/** How long a new dropout stays marked. */
const XRUN_FLASH_MS = 4000;

let lastXruns: number | null = null;
let xrunFlashTimer: ReturnType<typeof setTimeout> | null = null;

function el<T extends HTMLElement = HTMLElement>(id: string): T | null {
  return document.getElementById(id) as T | null;
}

function finite(value: unknown): number | null {
  return typeof value === "number" && Number.isFinite(value) ? value : null;
}

/** The engine's figures, or nothing where there are none to show (`available: false`). */
export function normalizeSystemStats(raw: unknown): SystemStats | null {
  if (typeof raw !== "object" || raw === null) {
    return null;
  }
  const payload = raw as Record<string, unknown>;
  if (payload.available !== true) {
    return null;
  }
  const cpu = finite(payload.cpuPercent);
  const xruns = finite(payload.xruns);
  const stats: SystemStats = {
    cpuPercent: cpu === null ? null : Math.min(100, Math.max(0, cpu)),
    temperatureC: finite(payload.temperatureC),
    xruns: xruns === null || xruns < 0 ? null : Math.round(xruns),
  };
  return stats.cpuPercent === null && stats.temperatureC === null && stats.xruns === null ? null : stats;
}

export function formatCpuPercent(percent: number): string {
  return `${Math.round(percent)}%`;
}

export function formatTemperature(celsius: number): string {
  return `${Math.round(celsius)}°C`;
}

/** Past 9999 the count is only ever "a lot", and the bar has no room for more digits. */
export function formatXruns(count: number): string {
  return count > 9999 ? "9999+" : String(count);
}

function showFigure(id: string, text: string | null, alert: boolean): void {
  const figure = el(id);
  if (!figure) {
    return;
  }
  figure.hidden = text === null;
  figure.classList.toggle("is-alert", alert && text !== null);
  const value = figure.querySelector("strong");
  if (value && text !== null && value.textContent !== text) {
    value.textContent = text;
  }
}

function setReadoutVisible(visible: boolean): void {
  const readout = el("footer-system-stats");
  if (!readout || readout.hidden === !visible) {
    return;
  }
  readout.hidden = !visible;
  // The compact footer fits its items to the room on the bar (ts/footerFit.ts), and takes a
  // change of the bar's class as its cue to measure again.
  document.querySelector(".footer-bar")?.classList.toggle("has-system-stats", visible);
}

/** A `systemStats` message from the engine. */
export function onSystemStats(payload: Record<string, unknown>): void {
  const stats = normalizeSystemStats(payload);
  if (!stats) {
    setReadoutVisible(false);
    return;
  }

  // A count that went up since the last message is a dropout that just happened.
  if (stats.xruns !== null && lastXruns !== null && stats.xruns > lastXruns) {
    if (xrunFlashTimer !== null) {
      clearTimeout(xrunFlashTimer);
    }
    xrunFlashTimer = setTimeout(() => {
      xrunFlashTimer = null;
      el("footer-system-xruns")?.classList.remove("is-alert");
    }, XRUN_FLASH_MS);
  }
  lastXruns = stats.xruns;

  showFigure("footer-system-cpu", stats.cpuPercent === null ? null : formatCpuPercent(stats.cpuPercent), (stats.cpuPercent ?? 0) >= SYSTEM_STATS_BUSY_PERCENT);
  showFigure("footer-system-temp", stats.temperatureC === null ? null : formatTemperature(stats.temperatureC), (stats.temperatureC ?? 0) >= SYSTEM_STATS_HOT_C);
  showFigure("footer-system-xruns", stats.xruns === null ? null : formatXruns(stats.xruns), xrunFlashTimer !== null);
  setReadoutVisible(true);
}

/** Asks for the feed. Call once the bridge is up; a host without the figures says so and nothing is shown. */
export function initSystemStats(): void {
  postMessage({ type: "watchSystemStats", enabled: true });
}
