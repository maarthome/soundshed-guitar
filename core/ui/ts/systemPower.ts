/**
 * The power button under the gear (my_gui): a window with Cancel, POWER OFF and EXIT APP, for the
 * standalone app on a touch screen with nothing else to switch it off with. The engine side is
 * juce/source/StandaloneSystemPower: it saves the app's state and runs `systemctl poweroff`, or
 * quits the app as closing its window does. The button shows only once the engine answers
 * `systemPowerState` with `available: true`, so a plugin, or another platform, never shows it.
 */

import { postMessage } from "./bridge.js";

let initialized = false;
let requested = false;

function el<T extends HTMLElement = HTMLElement>(id: string): T | null {
  return document.getElementById(id) as T | null;
}

function request(action: string): void {
  postMessage({ type: "systemPower", action });
}

function setBusy(busy: boolean, status: string): void {
  const statusText = el("power-modal-status");
  if (statusText) {
    statusText.textContent = status;
  }
  for (const id of ["power-modal-poweroff", "power-modal-exit"]) {
    const button = el<HTMLButtonElement>(id);
    if (button) {
      button.disabled = busy;
    }
  }
}

function showError(message: string): void {
  const error = el("power-modal-error");
  if (error) {
    error.textContent = message;
    error.hidden = message === "";
  }
}

export function openPowerModal(): void {
  const modal = el("power-modal");
  if (!modal) {
    return;
  }
  showError("");
  setBusy(false, "Switch the computer off, or close Soundshed Guitar.");
  modal.style.display = "flex";
  el<HTMLButtonElement>("power-modal-cancel")?.focus();
}

export function closePowerModal(): void {
  const modal = el("power-modal");
  if (modal) {
    modal.style.display = "none";
  }
}

/** The engine's answer: whether the button is offered, and how a power-off went. */
export function onSystemPowerState(payload: Record<string, unknown>): void {
  const button = el<HTMLButtonElement>("nav-power-btn");
  const available = payload.available === true;
  if (button) {
    button.hidden = !available;
  }
  const powerOff = el<HTMLButtonElement>("power-modal-poweroff");
  if (powerOff) {
    powerOff.hidden = payload.canPowerOff === false;
  }
  if (payload.poweringOff === true) {
    setBusy(true, "Powering off… Wait until the screen goes dark before you unplug it.");
  } else {
    setBusy(false, "Switch the computer off, or close Soundshed Guitar.");
  }
  const error = typeof payload.error === "string" ? payload.error : "";
  if (error) {
    showError(`Could not power off: ${error}`);
  }
}

/** Asks once, when the app turns out to be the standalone one; a plugin never shows the button. */
export function syncSystemPowerAvailability(isStandalone: boolean): void {
  if (!isStandalone || requested) {
    return;
  }
  requested = true;
  request("getState");
}

export function initSystemPower(): void {
  if (initialized) {
    return;
  }
  initialized = true;

  el("nav-power-btn")?.addEventListener("click", openPowerModal);
  el("power-modal-cancel")?.addEventListener("click", closePowerModal);
  el("power-modal-poweroff")?.addEventListener("click", () => {
    showError("");
    setBusy(true, "Saving and powering off…");
    request("powerOff");
  });
  el("power-modal-exit")?.addEventListener("click", () => {
    showError("");
    setBusy(true, "Closing Soundshed Guitar…");
    request("exit");
  });

  const modal = el("power-modal");
  // A tap beside the window, or Escape, is Cancel.
  modal?.addEventListener("mousedown", (event) => {
    if (event.target === modal) {
      closePowerModal();
    }
  });
  document.addEventListener("keydown", (event) => {
    if (event.key === "Escape" && modal && modal.style.display !== "none") {
      closePowerModal();
    }
  });
}
