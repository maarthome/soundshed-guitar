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
 *
 * Room on the bar. The compact bar fits its items around the readout by itself
 * (ts/footerFit.ts). The full bar does not: its four groups share what the readout
 * leaves, and below about 780px of that their controls slide over each other. So on
 * the full bar this module makes the readout give way: first it stacks into two short
 * rows, about half as wide, and if the groups still do not fit it steps off the bar
 * until there is room again. A bar too narrow for its own groups is left as it was.
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

/** True when the figure came or went, which changes how wide the readout is. */
function showFigure(id: string, text: string | null, alert: boolean): boolean {
  const figure = el(id);
  if (!figure) {
    return false;
  }
  const changed = figure.hidden !== (text === null);
  figure.hidden = text === null;
  figure.classList.toggle("is-alert", alert && text !== null);
  const value = figure.querySelector("strong");
  if (value && text !== null && value.textContent !== text) {
    value.textContent = text;
  }
  return changed;
}

/** Clearance two neighbouring controls keep, in CSS pixels. */
const GROUP_CLEARANCE_PX = 2;

/**
 * True when the full bar's groups run into each other, or off its end.
 *
 * The groups share the bar equally and a group's controls spill out of it when its share
 * is too small. A little spill into the gap between groups does no harm and the bar is
 * drawn that way at its narrower sizes, so this asks the question that matters: does one
 * group's last control reach the next group's first?
 */
function fullBarGroupsCollide(): boolean {
  const panel = document.querySelector(".footer-actions-panel");
  if (!panel) {
    return false;
  }
  let previousRight = Number.NEGATIVE_INFINITY;
  for (const group of Array.from(panel.children)) {
    let left = Number.POSITIVE_INFINITY;
    let right = Number.NEGATIVE_INFINITY;
    for (const control of Array.from(group.children)) {
      const box = control.getBoundingClientRect();
      if (box.width > 0 && box.height > 0) {
        left = Math.min(left, box.left);
        right = Math.max(right, box.right);
      }
    }
    if (left === Number.POSITIVE_INFINITY) {
      continue;
    }
    if (left < previousRight + GROUP_CLEARANCE_PX) {
      return true;
    }
    previousRight = right;
  }
  return previousRight > panel.getBoundingClientRect().right + 1;
}

/**
 * Makes the readout as wide as the full bar can afford: in a row, stacked, or off the bar.
 * Each call starts from the row again, so it also undoes itself when the window grows.
 */
export function fitSystemStatsToBar(): void {
  const readout = el("footer-system-stats");
  if (!readout || readout.hidden) {
    return;
  }
  readout.classList.remove("is-stacked", "is-squeezed-out");
  // The compact bar measures its own room and moves other items out of the way.
  if (document.documentElement.dataset.density === "compact") {
    return;
  }
  if (!fullBarGroupsCollide()) {
    return;
  }
  readout.classList.add("is-stacked");
  if (!fullBarGroupsCollide()) {
    return;
  }
  readout.classList.add("is-squeezed-out");
}

let fitObserver: ResizeObserver | null = null;

/** Fits again whenever the room changes: the window, the zoom, the density, or a group's contents. */
function watchBarRoom(): void {
  if (fitObserver || typeof ResizeObserver === "undefined") {
    return;
  }
  const bar = document.querySelector(".footer-bar");
  const panel = document.querySelector(".footer-actions-panel");
  if (!bar || !panel) {
    return;
  }
  // A fit ends in the same state for the same room, so the resize it causes itself settles at once.
  fitObserver = new ResizeObserver(() => fitSystemStatsToBar());
  fitObserver.observe(bar);
  fitObserver.observe(panel);
  window.addEventListener("densityChanged", () => fitSystemStatsToBar());
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
  if (visible) {
    watchBarRoom();
    fitSystemStatsToBar();
  }
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

  const cpuChanged = showFigure("footer-system-cpu", stats.cpuPercent === null ? null : formatCpuPercent(stats.cpuPercent), (stats.cpuPercent ?? 0) >= SYSTEM_STATS_BUSY_PERCENT);
  const tempChanged = showFigure("footer-system-temp", stats.temperatureC === null ? null : formatTemperature(stats.temperatureC), (stats.temperatureC ?? 0) >= SYSTEM_STATS_HOT_C);
  const xrunsChanged = showFigure("footer-system-xruns", stats.xruns === null ? null : formatXruns(stats.xruns), xrunFlashTimer !== null);

  const readout = el("footer-system-stats");
  const wasVisible = readout !== null && !readout.hidden;
  setReadoutVisible(true);
  // Showing it fits it; after that only a figure coming or going changes its width.
  if (wasVisible && (cpuChanged || tempChanged || xrunsChanged)) {
    fitSystemStatsToBar();
  }
}

/** Asks for the feed. Call once the bridge is up; a host without the figures says so and nothing is shown. */
export function initSystemStats(): void {
  postMessage({ type: "watchSystemStats", enabled: true });
}
