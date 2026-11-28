/**
 * An on-screen keyboard for typing a value or a name on a touch screen with no keyboard of its
 * own (the Pi's kiosk). The web view there has no system keyboard, so the page draws one: a
 * number pad for number fields, a QWERTY layout for the rest. It opens when such a field takes
 * focus and closes when focus leaves it or on Done.
 *
 * Settings > User Interface > On-screen keyboard: Auto (the compact layout or a coarse pointer),
 * On, or Off. App setting `ui.onScreenKeyboard`.
 */

import { getAppSetting, updateAppSetting } from "./appSettingsStore.js";
import { isCompact } from "./compactMode.js";

export const ON_SCREEN_KEYBOARD_SETTING = "ui.onScreenKeyboard";

type KeyboardMode = "auto" | "on" | "off";
type Layout = "lower" | "upper" | "symbols" | "numeric";
type Field = HTMLInputElement | HTMLTextAreaElement;

const TEXT_TYPES = new Set(["", "text", "search", "number", "tel", "email", "url", "password"]);
const NUMERIC_INPUT_MODES = new Set(["numeric", "decimal", "tel"]);

const LAYOUTS: Record<Layout, string[][]> = {
  lower: [
    [..."qwertyuiop"],
    [..."asdfghjkl"],
    ["shift", ..."zxcvbnm", "back"],
    ["symbols", ",", "space", ".", "done"],
  ],
  upper: [
    [..."QWERTYUIOP"],
    [..."ASDFGHJKL"],
    ["shift", ..."ZXCVBNM", "back"],
    ["symbols", ",", "space", ".", "done"],
  ],
  symbols: [
    [..."1234567890"],
    ["-", "/", ":", ";", "(", ")", "&", "@", "\""],
    ["#", "+", "=", "?", "!", "'", "_", "back"],
    ["letters", ",", "space", ".", "done"],
  ],
  numeric: [
    ["1", "2", "3", "back"],
    ["4", "5", "6", "-"],
    ["7", "8", "9", "."],
    ["0", "done"],
  ],
};

const LABELS: Record<string, string> = {
  back: "⌫",
  shift: "⇧",
  space: "space",
  symbols: "123",
  letters: "ABC",
  done: "Done",
};

let keyboard: HTMLElement | null = null;
let target: Field | null = null;
let layout: Layout = "lower";
/** The first key typed into a number field replaces what it held, as typing over a selection does. */
let replaceOnNextKey = false;
/** A number field is switched to text while it is edited: "1." and "-" are not numbers yet. */
let switchedFromNumber = false;
let repeatTimer: number | null = null;

function mode(): KeyboardMode {
  const value = getAppSetting(ON_SCREEN_KEYBOARD_SETTING);
  return value === "on" || value === "off" ? value : "auto";
}

function isEnabled(): boolean {
  const current = mode();
  if (current === "off") {
    return false;
  }
  return current === "on" || isCompact() || Boolean(window.matchMedia?.("(pointer: coarse)").matches);
}

function asField(element: EventTarget | Element | null): Field | null {
  if (element instanceof HTMLTextAreaElement) {
    return element.readOnly || element.disabled ? null : element;
  }
  if (element instanceof HTMLInputElement && TEXT_TYPES.has(element.type) && !element.readOnly && !element.disabled) {
    return element.dataset.noKeyboard === undefined ? element : null;
  }
  return null;
}

function wantsNumberPad(field: Field): boolean {
  return (
    field instanceof HTMLInputElement &&
    (field.type === "number" || field.type === "tel" || NUMERIC_INPUT_MODES.has(field.inputMode))
  );
}

function notifyInput(field: Field): void {
  field.dispatchEvent(new Event("input", { bubbles: true }));
}

/** Types `text` at the caret, or over the selection. */
function insert(text: string): void {
  const field = target;
  if (!field) {
    return;
  }
  if (replaceOnNextKey) {
    field.value = "";
    replaceOnNextKey = false;
  }
  const start = field.selectionStart ?? field.value.length;
  const end = field.selectionEnd ?? field.value.length;
  field.setRangeText(text, start, end, "end");
  announce(field);
}

function backspace(): void {
  const field = target;
  if (!field) {
    return;
  }
  replaceOnNextKey = false;
  const start = field.selectionStart ?? field.value.length;
  const end = field.selectionEnd ?? field.value.length;
  if (start !== end) {
    field.setRangeText("", start, end, "end");
  } else if (start > 0) {
    field.setRangeText("", start - 1, start, "end");
  }
  announce(field);
}

/** An edit made in a number field becomes an input event only while it reads as a number. */
function announce(field: Field): void {
  if (switchedFromNumber && !Number.isFinite(Number.parseFloat(field.value))) {
    return;
  }
  notifyInput(field);
}

function done(): void {
  const field = target;
  if (!field) {
    return;
  }
  if (field instanceof HTMLTextAreaElement) {
    field.blur();
    return;
  }
  // Enter first, for fields that commit on it; leaving the field then commits the rest.
  for (const type of ["keydown", "keyup"]) {
    field.dispatchEvent(new KeyboardEvent(type, { key: "Enter", code: "Enter", bubbles: true, cancelable: true }));
  }
  field.blur();
}

function press(key: string): void {
  switch (key) {
    case "back":
      backspace();
      return;
    case "shift":
      setLayout(layout === "lower" ? "upper" : "lower");
      return;
    case "symbols":
      setLayout("symbols");
      return;
    case "letters":
      setLayout("lower");
      return;
    case "space":
      insert(" ");
      return;
    case "done":
      done();
      return;
    default:
      insert(key);
      // One capital, then back to lower case, as on a phone.
      if (layout === "upper") {
        setLayout("lower");
      }
  }
}

function stopRepeat(): void {
  if (repeatTimer !== null) {
    window.clearTimeout(repeatTimer);
    window.clearInterval(repeatTimer);
    repeatTimer = null;
  }
}

function startRepeat(key: string): void {
  stopRepeat();
  if (key !== "back") {
    return;
  }
  repeatTimer = window.setTimeout(() => {
    repeatTimer = window.setInterval(backspace, 70);
  }, 450);
}

function render(): void {
  if (!keyboard) {
    return;
  }
  keyboard.dataset.layout = layout;
  keyboard.innerHTML = LAYOUTS[layout]
    .map(
      (row) =>
        `<div class="osk-row">${row
          .map((key) => {
            const label = LABELS[key] ?? key;
            const wide = key === "space" ? " osk-key-space" : key === "done" ? " osk-key-done" : LABELS[key] ? " osk-key-fn" : "";
            const pressed = key === "shift" && layout === "upper" ? ' aria-pressed="true"' : "";
            return `<button type="button" class="osk-key${wide}" data-osk-key="${key === '"' ? "&quot;" : key}"${pressed}>${label}</button>`;
          })
          .join("")}</div>`,
    )
    .join("");
}

function setLayout(next: Layout): void {
  layout = next;
  render();
  measure();
}

function ensureKeyboard(): HTMLElement {
  if (keyboard) {
    return keyboard;
  }
  const element = document.createElement("div");
  element.className = "osk";
  element.id = "on-screen-keyboard";
  element.hidden = true;
  element.setAttribute("role", "application");
  element.setAttribute("aria-label", "On-screen keyboard");

  // The keys never take focus: the field keeps it, and with it the caret.
  const keepFocus = (event: Event): void => event.preventDefault();
  element.addEventListener("mousedown", keepFocus);
  element.addEventListener("pointerdown", (event) => {
    const key = (event.target as HTMLElement).closest<HTMLElement>("[data-osk-key]")?.dataset.oskKey;
    keepFocus(event);
    if (key) {
      press(key);
      startRepeat(key);
    }
  });
  for (const type of ["pointerup", "pointercancel", "pointerleave"]) {
    element.addEventListener(type, stopRepeat);
  }
  document.body.appendChild(element);
  keyboard = element;
  return element;
}

/** Tells the page how much of the screen the keyboard covers, and brings the field above it. */
function measure(): void {
  if (!keyboard || keyboard.hidden) {
    return;
  }
  document.documentElement.style.setProperty("--osk-height", `${keyboard.offsetHeight}px`);
  const field = target;
  if (!field) {
    return;
  }
  const rect = field.getBoundingClientRect();
  if (rect.bottom > window.innerHeight - keyboard.offsetHeight - 8) {
    field.scrollIntoView({ block: "center", inline: "nearest" });
  }
}

function open(field: Field): void {
  target = field;
  switchedFromNumber = false;
  if (field instanceof HTMLInputElement && field.type === "number") {
    // "1." and "-" are not numbers yet, so the field shows text until it is left.
    field.type = "text";
    field.inputMode = "decimal";
    switchedFromNumber = true;
  }
  replaceOnNextKey = switchedFromNumber;
  const bar = ensureKeyboard();
  layout = wantsNumberPad(field) ? "numeric" : "lower";
  bar.hidden = false;
  document.documentElement.classList.add("osk-open");
  render();
  measure();
}

function restoreField(field: Field): void {
  if (switchedFromNumber && field instanceof HTMLInputElement) {
    field.type = "number";
    switchedFromNumber = false;
  }
}

function closeAndRestore(): void {
  if (target) {
    restoreField(target);
  }
  close();
}

/** Keeps the keyboard from outliving its field: the field left the page, was hidden, or lost focus quietly. */
function watchTarget(): void {
  if (!target) {
    return;
  }
  const gone = !target.isConnected || target.offsetParent === null;
  if (gone || document.activeElement !== target || !document.hasFocus()) {
    closeAndRestore();
  }
}

function close(): void {
  stopRepeat();
  if (keyboard) {
    keyboard.hidden = true;
  }
  document.documentElement.classList.remove("osk-open");
  document.documentElement.style.removeProperty("--osk-height");
  target = null;
}

export function initOnScreenKeyboard(): void {
  document.addEventListener("focusin", (event) => {
    const field = asField(event.target);
    if (!field || !isEnabled()) {
      return;
    }
    if (target && target !== field) {
      restoreField(target);
    }
    open(field);
  });

  document.addEventListener("focusout", (event) => {
    const field = asField(event.target);
    if (!field || field !== target) {
      return;
    }
    // Focus moving straight to another field is a focusin that follows; anywhere else closes.
    window.setTimeout(() => {
      if (target === field && document.activeElement !== field) {
        restoreField(field);
        close();
      }
    }, 0);
  });

  window.addEventListener("resize", measure);

  // A tap on anything that is not a field or the keyboard leaves the field, even where the
  // browser keeps focus on it. A plugin's own window taking focus counts as leaving too.
  document.addEventListener(
    "pointerdown",
    (event) => {
      const hit = event.target as HTMLElement | null;
      if (!target || !hit || keyboard?.contains(hit) || asField(hit)) {
        return;
      }
      target.blur();
      closeAndRestore();
    },
    true,
  );
  window.addEventListener("blur", () => window.setTimeout(watchTarget, 0));
  document.addEventListener("keydown", (event) => {
    if (event.key === "Escape" && target) {
      target.blur();
      closeAndRestore();
    }
  });
  window.setInterval(watchTarget, 500);

  const select = document.getElementById("on-screen-keyboard-select") as HTMLSelectElement | null;
  if (select) {
    select.value = mode();
    // Settings can arrive after this runs: show what is stored whenever the choice is opened.
    select.addEventListener("pointerdown", () => {
      select.value = mode();
    });
    select.addEventListener("change", () => {
      updateAppSetting(ON_SCREEN_KEYBOARD_SETTING, select.value);
      if (!isEnabled() && target) {
        restoreField(target);
        close();
      }
    });
  }
}
