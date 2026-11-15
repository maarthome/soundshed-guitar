import { postMessage } from "./bridge.js";
import type { UiSettings } from "./types.js";

let currentSettings: UiSettings = { zoom: 1 };
let pendingSend = false;
const sendDelayMs = 250;
/**
 * The interface zoom is fixed at 100% in this build, and the footer's zoom control is gone.
 *
 * The app runs full screen on a small panel, where the layout that fits is chosen from the
 * window's size (ts/compactMode.ts). A zoom on top of that changed which layout was chosen
 * and left controls too small to touch or too large to fit. Whatever zoom was saved before
 * is replaced: the first save after start writes this one back.
 */
const FIXED_ZOOM = 1;

function applyZoomToDocument(): number {
  document.body.style.zoom = `${FIXED_ZOOM}`;
  return FIXED_ZOOM;
}

function notifyUiSettingsApplied(): void {
  window.dispatchEvent(new CustomEvent("uiSettingsApplied", {
    detail: { settings: currentSettings },
  }));
}

function captureBounds() {
  return {
    x: typeof window.screenX === "number" ? window.screenX : 0,
    y: typeof window.screenY === "number" ? window.screenY : 0,
    width: typeof window.outerWidth === "number" ? window.outerWidth : window.innerWidth,
    height: typeof window.outerHeight === "number" ? window.outerHeight : window.innerHeight,
  };
}

function captureZoom(): number {
  return FIXED_ZOOM;
}

function scheduleSend() {
  if (pendingSend) return;
  pendingSend = true;
  window.setTimeout(() => {
    pendingSend = false;
    postMessage({ type: "uiSettingsChanged", settings: currentSettings });
  }, sendDelayMs);
}

export function updateUiSettings(patch: Partial<UiSettings>): void {
  currentSettings = { ...currentSettings, ...patch };
  // A zoom asked for is not taken: see FIXED_ZOOM.
  currentSettings.zoom = applyZoomToDocument();
  if (typeof patch.signalPathHeight === "number" && Number.isFinite(patch.signalPathHeight)) {
    currentSettings.signalPathHeight = patch.signalPathHeight;
  }
  notifyUiSettingsApplied();
  scheduleSend();
}

export function applyUiSettings(settings?: UiSettings): void {
  if (!settings) return;
  currentSettings = { ...currentSettings, ...settings };

  // The saved zoom is not applied: see FIXED_ZOOM.
  currentSettings.zoom = applyZoomToDocument();

  if (typeof settings.signalPathHeight === "number" && Number.isFinite(settings.signalPathHeight)) {
    currentSettings.signalPathHeight = settings.signalPathHeight;
  }

  notifyUiSettingsApplied();

  const bounds = settings.bounds;
  if (bounds && typeof window.resizeTo === "function") {
    try {
      let targetWidth = bounds.width;
      let targetHeight = bounds.height;

      if (window.screen) {
        const maxWidth = window.screen.availWidth || window.screen.width;
        const maxHeight = window.screen.availHeight || window.screen.height;
        if (maxWidth && targetWidth > maxWidth) {
          targetWidth = maxWidth;
        }
        if (maxHeight && targetHeight > maxHeight) {
          targetHeight = maxHeight;
        }
      }

      window.resizeTo(targetWidth, targetHeight);
      if (typeof window.moveTo === "function") {
        window.moveTo(bounds.x, bounds.y);
      }
    } catch {
      // Some hosts may block programmatic window moves/resizes; ignore.
    }
  }
}

export function startUiSettingsTracking(): void {
  const updateMetrics = () => {
    currentSettings = {
      ...currentSettings,
      zoom: captureZoom(),
      bounds: captureBounds(),
    };
  };

  updateMetrics();

  const onResize = () => {
    updateMetrics();
    scheduleSend();
  };

  window.addEventListener("resize", onResize);
  if (window.visualViewport) {
    window.visualViewport.addEventListener("resize", onResize);
  }

  window.addEventListener("beforeunload", () => {
    updateMetrics();
    postMessage({ type: "uiSettingsChanged", settings: currentSettings });
  });
}

/**
 * Takes values the engine keeps in the UI settings itself (presetRecents) into the blob this
 * module sends back whole, without sending it. uiSettingsChanged replaces the engine's copy,
 * so the blob must carry the engine's current list, never an older one.
 */
export function adoptEngineUiSettings(patch: Partial<UiSettings>): void {
  currentSettings = { ...currentSettings, ...patch };
}

export function getCurrentUiSettings(): UiSettings {
  return currentSettings;
}
