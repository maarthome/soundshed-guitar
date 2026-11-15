/**
 * Settings > Remote Access: the standalone app's screen over VNC, on Linux.
 *
 * For a device whose own screen is a small touch panel: a VNC viewer on another
 * computer, tablet or phone shows the app and operates it. The engine side is
 * juce/source/StandaloneRemoteAccess, which runs wayvnc. This module keeps no state
 * of its own beyond what is typed into the password field: every request is answered
 * with a whole `remoteAccessState` snapshot, and the controls are redrawn from it.
 *
 * Browser access is a second switch: the same screen as a web page (noVNC), for when
 * installing a viewer is not wanted. It only runs while remote access itself is on.
 *
 * The password is never sent back, so the field shows one only while it is being
 * typed or has just been generated. Generate exists because a touch-only device has
 * no keyboard to type one with: it makes a password, shows it, and Save stores it.
 */

import { postMessage } from "../bridge.js";

export interface RemoteAccessState {
  /** False where the server cannot run at all: no Wayland session, or wayvnc is not installed. */
  canRun: boolean;
  /** Why not, in words for the user. */
  problem: string;
  enabled: boolean;
  running: boolean;
  hasPassword: boolean;
  username: string;
  port: number;
  /** This device's addresses on the network, for the viewer. */
  addresses: string[];
  minPasswordLength: number;
  maxPasswordLength: number;
  /** From the last request that failed, or the server stopping by itself. */
  error: string;
  /** The browser switch. It is remembered while remote access is off, and runs with it. */
  webEnabled: boolean;
  webRunning: boolean;
  /** False where noVNC or websockify is not installed. */
  webCanRun: boolean;
  webProblem: string;
  webPort: number;
  webError: string;
}

const DEFAULT_MIN_PASSWORD_LENGTH = 6;
const DEFAULT_MAX_PASSWORD_LENGTH = 64;
const GENERATED_PASSWORD_LENGTH = 12;

/** No 0/O, 1/l/I: the password is read off one screen and typed into another. */
const GENERATED_PASSWORD_ALPHABET = "abcdefghijkmnpqrstuvwxyzABCDEFGHJKLMNPQRSTUVWXYZ23456789";

let initialized = false;
let requested = false;
let current: RemoteAccessState | null = null;
/** A local complaint about what is in the password field; cleared by the next edit. */
let passwordError = "";
/** Set by Save, so the answer to it can say the password was stored. */
let awaitingPasswordSave = false;
let passwordSaved = false;

function el<T extends HTMLElement = HTMLElement>(id: string): T | null {
  return document.getElementById(id) as T | null;
}

function request(action: string, args: Record<string, unknown> = {}): void {
  postMessage({ type: "remoteAccess", action, ...args });
}

// ── Narrowing ────────────────────────────────────────────────────────────────

function text(value: unknown): string {
  return typeof value === "string" ? value : "";
}

function wholeNumber(value: unknown, fallback: number): number {
  return typeof value === "number" && Number.isFinite(value) && value > 0 ? Math.round(value) : fallback;
}

/** The engine's snapshot, or nothing where this build has no remote access (`available: false`). */
export function normalizeRemoteAccessState(raw: unknown): RemoteAccessState | null {
  if (typeof raw !== "object" || raw === null) {
    return null;
  }
  const payload = raw as Record<string, unknown>;
  if (payload.available !== true) {
    return null;
  }
  return {
    canRun: payload.canRun === true,
    problem: text(payload.problem),
    enabled: payload.enabled === true,
    running: payload.running === true,
    hasPassword: payload.hasPassword === true,
    username: text(payload.username),
    port: wholeNumber(payload.port, 5900),
    addresses: Array.isArray(payload.addresses) ? payload.addresses.filter((item): item is string => typeof item === "string" && item !== "") : [],
    minPasswordLength: wholeNumber(payload.minPasswordLength, DEFAULT_MIN_PASSWORD_LENGTH),
    maxPasswordLength: wholeNumber(payload.maxPasswordLength, DEFAULT_MAX_PASSWORD_LENGTH),
    error: text(payload.error),
    webEnabled: payload.webEnabled === true,
    webRunning: payload.webRunning === true,
    webCanRun: payload.webCanRun === true,
    webProblem: text(payload.webProblem),
    webPort: wholeNumber(payload.webPort, 6080),
    webError: text(payload.webError),
  };
}

// ── What the controls say ────────────────────────────────────────────────────

/** Where a viewer connects: each of the device's addresses with the port. */
export function remoteAccessTargets(state: RemoteAccessState): string {
  if (state.addresses.length === 0) {
    return `this device's network address, port ${state.port}`;
  }
  return state.addresses.map((address) => `${address}:${state.port}`).join(" or ");
}

/** The line under the switch. */
export function remoteAccessStatus(state: RemoteAccessState): string {
  if (!state.canRun) {
    return state.problem || "Remote access cannot run on this system.";
  }
  if (state.enabled && state.running) {
    const user = state.username ? `, user name "${state.username}"` : "";
    return `On. Connect a VNC viewer to ${remoteAccessTargets(state)}${user}, with the password below.`;
  }
  if (state.enabled) {
    return "Turned on, but the VNC server is not running. Turn it off and on again to retry.";
  }
  if (!state.hasPassword) {
    return "Off. Set a password to turn it on.";
  }
  return "Off.";
}

/** The addresses to type into a browser. */
export function remoteAccessWebAddresses(state: RemoteAccessState): string {
  if (state.addresses.length === 0) {
    return `https://<this device's network address>:${state.webPort}`;
  }
  return state.addresses.map((address) => `https://${address}:${state.webPort}`).join(" or ");
}

/** The line under the browser switch. */
export function remoteAccessWebStatus(state: RemoteAccessState): string {
  if (!state.webCanRun) {
    return state.webProblem || "Browser access cannot run on this system.";
  }
  if (state.webEnabled && state.webRunning) {
    const user = state.username ? `user name "${state.username}" and ` : "";
    return `On. Open ${remoteAccessWebAddresses(state)} in a web browser (https, not http), choose to continue past the certificate warning, and sign in with ${user}the password above.`;
  }
  if (state.webEnabled && !state.enabled) {
    return "On, and starts when remote access is turned on.";
  }
  if (state.webEnabled) {
    return "Turned on, but not running. Turn it off and on again to retry.";
  }
  return "Off. Lets a web browser show and operate this screen, with no viewer to install. The connection is encrypted with this device's own certificate, which a browser warns about the first time.";
}

/** The same rule the engine applies (StandaloneRemoteAccess::passwordProblem); empty when the password will do. */
export function remoteAccessPasswordProblem(password: string, minLength = DEFAULT_MIN_PASSWORD_LENGTH, maxLength = DEFAULT_MAX_PASSWORD_LENGTH): string {
  const length = Array.from(password).length;
  if (length < minLength) {
    return `The password needs at least ${minLength} characters.`;
  }
  if (length > maxLength) {
    return `The password can have at most ${maxLength} characters.`;
  }
  if (!/^[\x21-\x7e]+$/.test(password) || password.includes("#")) {
    return "Use letters, digits and punctuation only: no spaces, no accented letters and no #.";
  }
  return "";
}

/**
 * A password that is easy to read off the screen. `randomBytes` returns that many
 * random bytes (crypto.getRandomValues); bytes that would favour the start of the
 * alphabet are thrown away rather than folded in.
 */
export function generateRemoteAccessPassword(randomBytes: (count: number) => ArrayLike<number>, length = GENERATED_PASSWORD_LENGTH): string {
  const alphabet = GENERATED_PASSWORD_ALPHABET;
  const limit = 256 - (256 % alphabet.length);
  let password = "";
  // Bounded, so a source that only ever returns rejected bytes cannot hang the page.
  for (let round = 0; round < 64 && password.length < length; round += 1) {
    const bytes = randomBytes(length * 2);
    for (let index = 0; index < bytes.length && password.length < length; index += 1) {
      const byte = bytes[index];
      if (byte >= 0 && byte < limit) {
        password += alphabet[byte % alphabet.length];
      }
    }
  }
  return password;
}

// ── Rendering ────────────────────────────────────────────────────────────────

function render(): void {
  const heading = el("settings-remote-access-heading");
  const section = el("settings-remote-access-section");
  if (!section) {
    return;
  }
  const visible = current !== null;
  section.hidden = !visible;
  if (heading) {
    heading.hidden = !visible;
  }
  if (!current) {
    return;
  }
  const state = current;

  const toggle = el<HTMLInputElement>("remote-access-toggle");
  if (toggle) {
    toggle.checked = state.enabled;
    // Turning it off is always allowed; turning it on needs somewhere to run and a password.
    toggle.disabled = !state.enabled && (!state.canRun || !state.hasPassword);
  }

  const status = el("remote-access-status");
  if (status) {
    status.textContent = remoteAccessStatus(state);
  }

  const password = el<HTMLInputElement>("remote-access-password");
  if (password) {
    password.placeholder = state.hasPassword ? "A password is set. Type a new one to change it" : "Choose a password";
    password.maxLength = state.maxPasswordLength;
  }

  const hint = el("remote-access-password-hint");
  if (hint) {
    const saved = passwordSaved ? "Password saved. " : "";
    hint.textContent = `${saved}At least ${state.minPasswordLength} characters, without spaces. Generate makes one for you: note it down, then Save. Changing the password disconnects anyone who is connected.`;
  }

  const error = el("remote-access-error");
  if (error) {
    const message = passwordError || state.error;
    error.textContent = message;
    error.hidden = message === "";
  }

  const webToggle = el<HTMLInputElement>("remote-access-web-toggle");
  if (webToggle) {
    webToggle.checked = state.webEnabled;
    // Turning it off is always allowed.
    webToggle.disabled = !state.webEnabled && !state.webCanRun;
  }

  const webStatus = el("remote-access-web-status");
  if (webStatus) {
    webStatus.textContent = remoteAccessWebStatus(state);
  }

  const webError = el("remote-access-web-error");
  if (webError) {
    webError.textContent = state.webError;
    webError.hidden = state.webError === "";
  }
}

function setPasswordVisible(visible: boolean): void {
  const password = el<HTMLInputElement>("remote-access-password");
  if (password) {
    password.type = visible ? "text" : "password";
  }
  const show = el<HTMLInputElement>("remote-access-password-show");
  if (show) {
    show.checked = visible;
  }
}

// ── Messages ─────────────────────────────────────────────────────────────────

/** A `remoteAccessState` from the engine: the whole state, or `available: false`. */
export function onRemoteAccessState(payload: Record<string, unknown>): void {
  current = normalizeRemoteAccessState(payload);
  passwordSaved = awaitingPasswordSave && current !== null && current.error === "" && current.hasPassword;
  awaitingPasswordSave = false;
  render();
}

/**
 * Asks for the state once, when the app turns out to be the standalone one. A plugin
 * format has no screen of its own to share, so there the section stays hidden without
 * asking. Called on every settings refresh: the environment arrives after the panel binds.
 */
export function syncRemoteAccessAvailability(isStandalone: boolean): void {
  if (!isStandalone || requested) {
    return;
  }
  requested = true;
  request("getState");
}

export function initRemoteAccessSettings(): void {
  if (initialized) {
    return;
  }
  initialized = true;

  el<HTMLInputElement>("remote-access-toggle")?.addEventListener("change", (event) => {
    passwordError = "";
    passwordSaved = false;
    request("setEnabled", { enabled: (event.target as HTMLInputElement).checked });
  });

  el<HTMLInputElement>("remote-access-web-toggle")?.addEventListener("change", (event) => {
    request("setWebEnabled", { enabled: (event.target as HTMLInputElement).checked });
  });

  const password = el<HTMLInputElement>("remote-access-password");
  password?.addEventListener("input", () => {
    if (passwordError || passwordSaved) {
      passwordError = "";
      passwordSaved = false;
      render();
    }
  });

  el<HTMLInputElement>("remote-access-password-show")?.addEventListener("change", (event) => {
    setPasswordVisible((event.target as HTMLInputElement).checked);
  });

  el("remote-access-password-generate")?.addEventListener("click", () => {
    if (!password || typeof crypto === "undefined" || typeof crypto.getRandomValues !== "function") {
      return;
    }
    password.value = generateRemoteAccessPassword((count) => crypto.getRandomValues(new Uint8Array(count)));
    // Shown, or there would be no way to learn what it is.
    setPasswordVisible(true);
    passwordError = "";
    passwordSaved = false;
    render();
  });

  el("remote-access-password-save")?.addEventListener("click", () => {
    if (!password) {
      return;
    }
    passwordSaved = false;
    passwordError = remoteAccessPasswordProblem(password.value, current?.minPasswordLength, current?.maxPasswordLength);
    if (passwordError) {
      render();
      return;
    }
    awaitingPasswordSave = true;
    request("setPassword", { password: password.value });
  });

  render();
}
