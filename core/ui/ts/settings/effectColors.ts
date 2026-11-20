/**
 * Settings > Appearance (my_gui): Default or User colours for the effect boxes in the chain, the
 * fill strength, and each effect group's colour. The choices are kept in the app setting
 * `ui.effectGroupColors` (ts/effectGroups.ts holds the groups and puts the colours in force).
 *
 * Tapping a group opens its colour editor across the full width: a field to drag in for
 * saturation (across) and brightness (up and down), a hue strip, a preview, R G B fields and a
 * row of quick colours. Dragging shows the colour at once and saves it when the finger lifts;
 * the R G B fields save on Enter or when left. Dragging needs no keyboard.
 */

import { uiState } from "../state.js";
import { updateAppSetting } from "../appSettingsStore.js";
import type { AppSettingValue } from "../types.js";
import {
  DEFAULT_EFFECT_GROUP_COLOR_SETTINGS,
  EFFECT_GROUPS,
  EFFECT_GROUP_COLORS_SETTING,
  EFFECT_GROUP_PALETTE,
  applyEffectGroupColorSettings,
  defaultGroupColor,
  groupColor,
  normalizeEffectGroupColorSettings,
  type EffectColorMode,
  type EffectColorStrength,
  type EffectGroupColorSettings,
  type EffectGroupId,
} from "../effectGroups.js";

let current: EffectGroupColorSettings = { ...DEFAULT_EFFECT_GROUP_COLOR_SETTINGS, colors: {} };
/** The group whose editor is open, if any: one at a time. */
let openGroup: EffectGroupId | null = null;
/**
 * The open editor's colour as hue, saturation and value (0-360, 0-1, 0-1). Kept rather than
 * worked out from the RGB each time, so the hue stays put while the colour passes through grey.
 */
let editing = { h: 0, s: 0, v: 0 };
let initialized = false;

// ── Colour arithmetic ───────────────────────────────────────────────────────────

export interface Rgb {
  r: number;
  g: number;
  b: number;
}

export function hexToRgb(hex: string): Rgb {
  const value = Number.parseInt(hex.replace("#", ""), 16);
  return { r: (value >> 16) & 255, g: (value >> 8) & 255, b: value & 255 };
}

export function rgbToHex({ r, g, b }: Rgb): string {
  const part = (n: number) => Math.round(Math.min(255, Math.max(0, n))).toString(16).padStart(2, "0");
  return `#${part(r)}${part(g)}${part(b)}`;
}

export function rgbToHsv({ r, g, b }: Rgb): { h: number; s: number; v: number } {
  const red = r / 255;
  const green = g / 255;
  const blue = b / 255;
  const max = Math.max(red, green, blue);
  const min = Math.min(red, green, blue);
  const delta = max - min;
  let h = 0;
  if (delta > 0) {
    if (max === red) h = 60 * (((green - blue) / delta) % 6);
    else if (max === green) h = 60 * ((blue - red) / delta + 2);
    else h = 60 * ((red - green) / delta + 4);
  }
  return { h: (h + 360) % 360, s: max === 0 ? 0 : delta / max, v: max };
}

export function hsvToRgb(h: number, s: number, v: number): Rgb {
  const c = v * s;
  const x = c * (1 - Math.abs(((h / 60) % 2) - 1));
  const m = v - c;
  const [r, g, b] = h < 60 ? [c, x, 0] : h < 120 ? [x, c, 0] : h < 180 ? [0, c, x]
    : h < 240 ? [0, x, c] : h < 300 ? [x, 0, c] : [c, 0, x];
  return { r: Math.round((r + m) * 255), g: Math.round((g + m) * 255), b: Math.round((b + m) * 255) };
}

// ── Saving and drawing ──────────────────────────────────────────────────────────

function el<T extends HTMLElement = HTMLElement>(id: string): T | null {
  return document.getElementById(id) as T | null;
}

function save(next: EffectGroupColorSettings): void {
  current = normalizeEffectGroupColorSettings(next);
  applyEffectGroupColorSettings(current);
  updateAppSetting(EFFECT_GROUP_COLORS_SETTING, {
    mode: current.mode,
    strength: current.strength,
    colors: { ...current.colors },
  } as unknown as AppSettingValue);
  render();
}

/** Shows a colour for the open group everywhere at once, without saving it. */
function preview(hex: string): void {
  if (!openGroup) {
    return;
  }
  applyEffectGroupColorSettings({ ...current, colors: { ...current.colors, [openGroup]: hex } });
  updateEditor(hex);
}

function saveOpenGroupColor(hex: string): void {
  if (openGroup) {
    save({ ...current, colors: { ...current.colors, [openGroup]: hex } });
  }
}

function renderSegments(): void {
  for (const button of document.querySelectorAll<HTMLButtonElement>("[data-effect-colors-mode]")) {
    button.setAttribute("aria-checked", String(button.dataset.effectColorsMode === current.mode));
  }
  for (const button of document.querySelectorAll<HTMLButtonElement>("[data-effect-colors-strength]")) {
    button.setAttribute("aria-checked", String(button.dataset.effectColorsStrength === current.strength));
  }
}

function editorMarkup(groupName: string): string {
  const channel = (id: string, label: string) =>
    `<label class="effect-colors-channel"><input type="number" min="0" max="255" step="1" inputmode="numeric"`
    + ` data-effect-colors-channel="${id}" aria-label="${groupName} ${label}" /><span>${label}</span></label>`;
  return `
    <div class="effect-colors-sv" data-effect-colors-sv role="slider" tabindex="0"
         aria-label="${groupName} saturation and brightness"><div class="effect-colors-sv-knob"></div></div>
    <div class="effect-colors-hue-row">
      <span class="effect-colors-preview" aria-hidden="true"></span>
      <div class="effect-colors-hue" data-effect-colors-hue role="slider" tabindex="0"
           aria-label="${groupName} hue" aria-valuemin="0" aria-valuemax="360"><div class="effect-colors-hue-knob"></div></div>
    </div>
    <div class="effect-colors-rgb">
      ${channel("r", "R")}${channel("g", "G")}${channel("b", "B")}
      <span class="effect-colors-hex" data-effect-colors-hex></span>
    </div>
    <div class="effect-colors-palette" role="radiogroup" aria-label="${groupName} quick colours"></div>`;
}

/** The open editor brought up to date with a colour, in place: nothing is rebuilt mid-drag. */
function updateEditor(hex: string): void {
  const editor = document.querySelector<HTMLElement>(".effect-colors-group[data-open] .effect-colors-editor");
  if (!editor) {
    return;
  }
  editor.style.setProperty("--editor-hue", `hsl(${editing.h} 100% 50%)`);
  editor.style.setProperty("--editor-color", hex);
  const svKnob = editor.querySelector<HTMLElement>(".effect-colors-sv-knob");
  if (svKnob) {
    svKnob.style.left = `${editing.s * 100}%`;
    svKnob.style.top = `${(1 - editing.v) * 100}%`;
  }
  const hueKnob = editor.querySelector<HTMLElement>(".effect-colors-hue-knob");
  if (hueKnob) {
    hueKnob.style.left = `${(editing.h / 360) * 100}%`;
  }
  editor.querySelector("[data-effect-colors-hue]")?.setAttribute("aria-valuenow", String(Math.round(editing.h)));
  const rgb = hexToRgb(hex);
  for (const input of editor.querySelectorAll<HTMLInputElement>("[data-effect-colors-channel]")) {
    const value = String(rgb[input.dataset.effectColorsChannel as keyof Rgb]);
    // The field being typed in keeps what is typed.
    if (document.activeElement !== input && input.value !== value) {
      input.value = value;
    }
  }
  const hexLabel = editor.querySelector<HTMLElement>("[data-effect-colors-hex]");
  if (hexLabel) {
    hexLabel.textContent = hex.toUpperCase();
  }
  for (const chip of editor.querySelectorAll<HTMLElement>("[data-effect-colors-chip]")) {
    chip.setAttribute("aria-checked", String(chip.dataset.effectColorsChip === hex.toLowerCase()));
  }
}

function renderGroups(): void {
  const list = el("effect-colors-groups");
  if (!list) {
    return;
  }
  list.innerHTML = "";
  for (const group of EFFECT_GROUPS) {
    const color = groupColor(current, group.id);
    const isOpen = openGroup === group.id;
    const item = document.createElement("div");
    item.className = "effect-colors-group";
    item.setAttribute("role", "listitem");
    item.dataset.effectGroup = group.id;
    item.toggleAttribute("data-open", isOpen);

    const head = document.createElement("button");
    head.type = "button";
    head.className = "effect-colors-group-head";
    head.dataset.effectColorsGroup = group.id;
    head.setAttribute("aria-expanded", String(isOpen));
    head.innerHTML = `<span class="effect-colors-swatch" aria-hidden="true"></span>`
      + `<span class="effect-colors-group-name"></span>`
      + `<span class="effect-colors-group-change"></span>`;
    (head.querySelector(".effect-colors-group-name") as HTMLElement).textContent = group.name;
    (head.querySelector(".effect-colors-group-change") as HTMLElement).textContent =
      isOpen ? "Close" : (color === defaultGroupColor(group.id) ? "Change" : "Changed");
    item.appendChild(head);

    if (isOpen) {
      const editor = document.createElement("div");
      editor.className = "effect-colors-editor";
      editor.innerHTML = editorMarkup(group.name);
      const palette = editor.querySelector(".effect-colors-palette") as HTMLElement;
      for (const chipColor of EFFECT_GROUP_PALETTE) {
        const chip = document.createElement("button");
        chip.type = "button";
        chip.className = "effect-colors-chip";
        chip.dataset.effectColorsChip = chipColor.toLowerCase();
        chip.style.setProperty("--chip-color", chipColor);
        chip.setAttribute("role", "radio");
        chip.setAttribute("aria-label", chipColor);
        chip.title = chipColor;
        palette.appendChild(chip);
      }
      item.appendChild(editor);
    }
    list.appendChild(item);
  }
  if (openGroup) {
    updateEditor(groupColor(current, openGroup));
  }
}

function render(): void {
  renderSegments();
  renderGroups();
  const reset = el<HTMLButtonElement>("effect-colors-reset");
  if (reset) {
    reset.disabled = Object.keys(current.colors).length === 0;
  }
}

function openEditor(group: EffectGroupId | null): void {
  openGroup = group;
  if (group) {
    editing = rgbToHsv(hexToRgb(groupColor(current, group)));
  }
  render();
}

// ── Dragging in the field and the hue strip ────────────────────────────────────────

function editorHex(): string {
  return rgbToHex(hsvToRgb(editing.h, editing.s, editing.v));
}

/** Moves the field's or the strip's knob to where the pointer is, as a fraction of the area. */
function pointAt(area: HTMLElement, event: PointerEvent): { x: number; y: number } {
  const rect = area.getBoundingClientRect();
  return {
    x: Math.min(1, Math.max(0, (event.clientX - rect.left) / rect.width)),
    y: Math.min(1, Math.max(0, (event.clientY - rect.top) / rect.height)),
  };
}

function bindDrag(panel: HTMLElement): void {
  panel.addEventListener("pointerdown", (event) => {
    const target = event.target as HTMLElement;
    const area = target.closest<HTMLElement>("[data-effect-colors-sv], [data-effect-colors-hue]");
    if (!area || !openGroup) {
      return;
    }
    event.preventDefault();
    // Keeps the drag going outside the area; a pointer the browser does not track (a synthetic
    // one) just goes without.
    try {
      area.setPointerCapture(event.pointerId);
    } catch {
      // Dragging still works while the pointer stays over the area.
    }
    const isField = area.hasAttribute("data-effect-colors-sv");
    const move = (moveEvent: PointerEvent) => {
      const point = pointAt(area, moveEvent);
      if (isField) {
        editing = { ...editing, s: point.x, v: 1 - point.y };
      } else {
        editing = { ...editing, h: point.x * 360 >= 360 ? 359.9 : point.x * 360 };
      }
      preview(editorHex());
    };
    const end = () => {
      area.removeEventListener("pointermove", move);
      area.removeEventListener("pointerup", end);
      area.removeEventListener("pointercancel", end);
      saveOpenGroupColor(editorHex());
    };
    area.addEventListener("pointermove", move);
    area.addEventListener("pointerup", end);
    area.addEventListener("pointercancel", end);
    move(event);
  });

  // Arrow keys on the field and the strip, for a keyboard.
  panel.addEventListener("keydown", (event) => {
    const area = (event.target as HTMLElement).closest<HTMLElement>("[data-effect-colors-sv], [data-effect-colors-hue]");
    if (!area || !openGroup) {
      return;
    }
    const step = event.shiftKey ? 0.1 : 0.01;
    const dx = event.key === "ArrowRight" ? step : event.key === "ArrowLeft" ? -step : 0;
    const dy = event.key === "ArrowUp" ? step : event.key === "ArrowDown" ? -step : 0;
    if (!dx && !dy) {
      return;
    }
    event.preventDefault();
    if (area.hasAttribute("data-effect-colors-sv")) {
      editing = { ...editing, s: Math.min(1, Math.max(0, editing.s + dx)), v: Math.min(1, Math.max(0, editing.v + dy)) };
    } else {
      editing = { ...editing, h: Math.min(359.9, Math.max(0, editing.h + (dx || dy) * 360)) };
    }
    saveOpenGroupColor(editorHex());
  });

  // The R G B fields: shown as typed, saved on Enter or on leaving the field.
  const fromFields = (): string | null => {
    const editor = panel.querySelector(".effect-colors-group[data-open] .effect-colors-editor");
    if (!editor) {
      return null;
    }
    const value = (id: string) => {
      const input = editor.querySelector<HTMLInputElement>(`[data-effect-colors-channel="${id}"]`);
      const n = Number.parseInt(input?.value ?? "", 10);
      return Number.isFinite(n) ? Math.min(255, Math.max(0, n)) : 0;
    };
    return rgbToHex({ r: value("r"), g: value("g"), b: value("b") });
  };
  panel.addEventListener("input", (event) => {
    if ((event.target as HTMLElement).matches("[data-effect-colors-channel]")) {
      const hex = fromFields();
      if (hex) {
        editing = rgbToHsv(hexToRgb(hex));
        preview(hex);
      }
    }
  });
  panel.addEventListener("change", (event) => {
    if ((event.target as HTMLElement).matches("[data-effect-colors-channel]")) {
      const hex = fromFields();
      if (hex) {
        saveOpenGroupColor(hex);
      }
    }
  });
}

/** The app settings as they arrived from the engine: the stored choice, or the defaults. */
export function applyEffectGroupAppSettings(appSettings: Record<string, unknown> | undefined): void {
  current = normalizeEffectGroupColorSettings(appSettings?.[EFFECT_GROUP_COLORS_SETTING]);
  applyEffectGroupColorSettings(current);
  if (openGroup) {
    editing = rgbToHsv(hexToRgb(groupColor(current, openGroup)));
  }
  render();
}

export function getEffectGroupColorSettings(): EffectGroupColorSettings {
  return current;
}

export function initEffectColorSettings(): void {
  if (initialized) {
    return;
  }
  initialized = true;

  // Before app settings arrive: whatever is there already, else the defaults.
  current = normalizeEffectGroupColorSettings(uiState.appSettings?.[EFFECT_GROUP_COLORS_SETTING]);
  applyEffectGroupColorSettings(current);

  const panel = document.querySelector<HTMLElement>("#equipment-tab-appearance");
  if (!panel) {
    return;
  }
  bindDrag(panel);

  panel.addEventListener("click", (event) => {
    const target = event.target as HTMLElement;

    const mode = target.closest<HTMLElement>("[data-effect-colors-mode]")?.dataset.effectColorsMode;
    if (mode === "default" || mode === "user") {
      save({ ...current, mode: mode as EffectColorMode });
      return;
    }

    const strength = target.closest<HTMLElement>("[data-effect-colors-strength]")?.dataset.effectColorsStrength;
    if (strength === "subtle" || strength === "medium" || strength === "strong") {
      save({ ...current, strength: strength as EffectColorStrength });
      return;
    }

    const chip = target.closest<HTMLElement>("[data-effect-colors-chip]");
    if (chip?.dataset.effectColorsChip) {
      editing = rgbToHsv(hexToRgb(chip.dataset.effectColorsChip));
      saveOpenGroupColor(chip.dataset.effectColorsChip);
      return;
    }

    const head = target.closest<HTMLElement>("[data-effect-colors-group]");
    if (head) {
      const group = head.dataset.effectColorsGroup as EffectGroupId;
      openEditor(openGroup === group ? null : group);
      return;
    }

    if (target.closest("#effect-colors-reset")) {
      openGroup = null;
      save({ ...current, colors: {} });
    }
  });

  render();
}
