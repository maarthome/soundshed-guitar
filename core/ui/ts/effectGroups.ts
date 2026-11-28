/**
 * Effect groups: the colour each effect box in the chain gets in Settings > Appearance's User
 * mode (my_gui). A group is one of LV2's main plugin classes (Reverb, Delay, Distortion, Dynamics,
 * Modulation, EQ, Filter, ...) or one of Soundshed's own categories (Amp, Cab, Drive), so the
 * built-in effects and hosted LV2/VST3 plugins share one set of colours.
 *
 * A built-in effect's group comes from its category, refined by name (Noise Gate is Noise
 * Reduction, Wah is Filter, ...). A hosted plugin's comes from the plugin itself, its LV2 class
 * label or VST3 sub-categories: the hosted plugin effect puts it in the node's config
 * (`pluginCategory`) each time it loads, so it is kept with the preset, and it also comes with
 * hostedPluginResourceLoadCompleted when a plugin is chosen (notePluginCategory). A chain box's group is
 * getNodeEffectGroup (signalPath/nodeTypes.ts).
 *
 * The choice is the app setting `ui.effectGroupColors`: {mode, strength, colors}. Default mode
 * leaves the boxes as the theme draws them; User mode fills each one with its group's colour
 * (css/effect-groups.css). Only colours changed from the defaults are stored.
 */

export type EffectGroupId =
  | "noise" | "drive" | "distortion" | "amp" | "cab" | "dynamics" | "eq" | "filter"
  | "modulation" | "pitch" | "delay" | "reverb" | "spatial" | "synth" | "utility";

export interface EffectGroup {
  id: EffectGroupId;
  name: string;
  /** The colour it has until the user picks another. */
  color: string;
}

/** In the order Settings lists them: roughly the order of a guitar chain. */
export const EFFECT_GROUPS: readonly EffectGroup[] = [
  { id: "noise", name: "Noise Reduction", color: "#5f9e6e" },
  { id: "dynamics", name: "Dynamics", color: "#d8b030" },
  { id: "drive", name: "Drive", color: "#e04848" },
  { id: "distortion", name: "Distortion", color: "#c0306a" },
  { id: "amp", name: "Amp", color: "#e07848" },
  { id: "cab", name: "Cab", color: "#a86830" },
  { id: "eq", name: "EQ", color: "#48a8e0" },
  { id: "filter", name: "Filter", color: "#30b8c8" },
  { id: "modulation", name: "Modulation", color: "#9048e0" },
  { id: "pitch", name: "Pitch", color: "#c040e0" },
  { id: "delay", name: "Delay", color: "#48d080" },
  { id: "reverb", name: "Reverb", color: "#4878e0" },
  { id: "spatial", name: "Spatial", color: "#7a90b8" },
  { id: "synth", name: "Synth", color: "#9aaa20" },
  { id: "utility", name: "Utility", color: "#808890" },
];

/** The colours offered when picking: every group's default and a few more. */
export const EFFECT_GROUP_PALETTE: readonly string[] = [
  ...EFFECT_GROUPS.map((group) => group.color),
  "#ff6fb0", "#ffd040", "#f0f0f0", "#404850",
];

export const EFFECT_GROUP_COLORS_SETTING = "ui.effectGroupColors";

export type EffectColorMode = "default" | "user";
export type EffectColorStrength = "subtle" | "medium" | "strong";

export interface EffectGroupColorSettings {
  mode: EffectColorMode;
  strength: EffectColorStrength;
  /** Colours changed from the defaults, by group. */
  colors: Partial<Record<EffectGroupId, string>>;
}

export const DEFAULT_EFFECT_GROUP_COLOR_SETTINGS: EffectGroupColorSettings = {
  mode: "default",
  strength: "medium",
  colors: {},
};

const GROUP_IDS = new Set<string>(EFFECT_GROUPS.map((group) => group.id));
const HEX_COLOR = /^#[0-9a-f]{6}$/i;

export function isEffectGroupId(value: unknown): value is EffectGroupId {
  return typeof value === "string" && GROUP_IDS.has(value);
}

export function defaultGroupColor(id: EffectGroupId): string {
  return EFFECT_GROUPS.find((group) => group.id === id)?.color ?? "#808890";
}

/** A stored value as settings: anything missing or unusable is the default. */
export function normalizeEffectGroupColorSettings(raw: unknown): EffectGroupColorSettings {
  const value = typeof raw === "object" && raw !== null ? (raw as Record<string, unknown>) : {};
  const colors: Partial<Record<EffectGroupId, string>> = {};
  const rawColors = typeof value.colors === "object" && value.colors !== null ? (value.colors as Record<string, unknown>) : {};
  for (const [id, color] of Object.entries(rawColors)) {
    if (isEffectGroupId(id) && typeof color === "string" && HEX_COLOR.test(color)) {
      const lower = color.toLowerCase();
      if (lower !== defaultGroupColor(id).toLowerCase()) {
        colors[id] = lower;
      }
    }
  }
  return {
    mode: value.mode === "user" ? "user" : "default",
    strength: value.strength === "subtle" || value.strength === "strong" ? value.strength : "medium",
    colors,
  };
}

export function groupColor(settings: EffectGroupColorSettings, id: EffectGroupId): string {
  return settings.colors[id] ?? defaultGroupColor(id);
}

// ── Which group ────────────────────────────────────────────────────────────────

/**
 * A hosted plugin's group from what its format calls it: an LV2 class label ("Reverb Plugin",
 * "Simulator Plugin") or VST3 sub-categories ("Fx|Reverb"). The more specific words come first:
 * "Fx|Delay|Reverb" is a reverb, an "Amplifier" an amp.
 */
export function effectGroupForPluginCategory(category: string): EffectGroupId {
  const text = category.toLowerCase();
  const rules: Array<[EffectGroupId, RegExp]> = [
    ["noise", /gate|noise|restoration|denois/],
    ["reverb", /reverb/],
    ["delay", /delay|echo/],
    ["amp", /amplifier|simulator|\bamp\b/],
    ["cab", /\bcab/],
    ["distortion", /distortion|waveshaper|overdrive|fuzz|saturat/],
    ["dynamics", /dynamics|compressor|limiter|expander|envelope/],
    ["eq", /\beq\b|equali[sz]er|parametric/],
    ["filter", /filter|lowpass|highpass|bandpass|comb|allpass|wah/],
    ["modulation", /modulat|chorus|flanger|phaser|tremolo|vibrato/],
    ["pitch", /pitch/],
    ["spatial", /spatial|surround|stereo|panner/],
    ["synth", /instrument|synth|generator|oscillator|sampler|drum/],
  ];
  for (const [group, pattern] of rules) {
    if (pattern.test(text)) {
      return group;
    }
  }
  return "utility";
}

/** A built-in effect's group: its category, and for a few its name. */
export function effectGroupForBuiltIn(category: string, displayName: string): EffectGroupId {
  const name = displayName.toLowerCase();
  if (/gate|noise/.test(name)) return "noise";
  if (category === "drive") return /distortion/.test(name) ? "distortion" : "drive";
  if (/\bwah\b/.test(name)) return "filter";
  if (/spatial/.test(name)) return "spatial";
  const byCategory: Record<string, EffectGroupId> = {
    amp: "amp", pedal: "amp", preamp: "amp", "full-rig": "amp", channel: "amp",
    cab: "cab", dynamics: "dynamics", eq: "eq", modulation: "modulation", pitch: "pitch",
    delay: "delay", reverb: "reverb", synth: "synth", utility: "utility",
  };
  return byCategory[category] ?? "utility";
}

/** Hosted plugins' kinds, by chain node, as the engine reported them on loading. */
const pluginCategoryByNode = new Map<string, string>();

/** What the engine reported for a hosted plugin's node; "" before it has. */
export function getPluginCategoryForNode(nodeId: string): string {
  return pluginCategoryByNode.get(nodeId) ?? "";
}

/** Hosted plugins' kinds, by library resource, so the plugin list can colour rows it has seen load. */
const RESOURCE_CATEGORY_KEY = "soundshed.pluginCategoryByResource";
const pluginCategoryByResource = new Map<string, string>();
try {
  const stored = JSON.parse(localStorage.getItem(RESOURCE_CATEGORY_KEY) ?? "{}") as Record<string, unknown>;
  for (const [id, category] of Object.entries(stored)) {
    if (typeof category === "string" && category) {
      pluginCategoryByResource.set(id, category);
    }
  }
} catch {
  // No storage, or unreadable: rows just stay neutral until a plugin loads.
}

/** The effect group of a library plugin once it has loaded in this browser; "" before. */
export function getPluginGroupForResource(resourceId: string | undefined): EffectGroupId | "" {
  const category = resourceId ? pluginCategoryByResource.get(resourceId) : undefined;
  return category ? effectGroupForPluginCategory(category) : "";
}

/** A plugin already loaded by a preset reports its kind in its node's config: the list learns it from there. */
export function rememberResourceCategory(resourceId: string, category: string): void {
  if (pluginCategoryByResource.get(resourceId) === category) {
    return;
  }
  pluginCategoryByResource.set(resourceId, category);
  try {
    localStorage.setItem(RESOURCE_CATEGORY_KEY, JSON.stringify(Object.fromEntries(pluginCategoryByResource)));
  } catch {
    // Storage unavailable: the colour lasts until the page is reloaded.
  }
}

/**
 * hostedPluginResourceLoadCompleted's pluginCategory. The box is already drawn by then, so its
 * group is put right in place rather than the chain drawn again; the same goes for the plugin's
 * row in the plugin list and for the editor panel when it is that node's.
 */
export function notePluginCategory(nodeId: string, category: string, resourceId?: string): void {
  if (!nodeId || !category) {
    return;
  }
  pluginCategoryByNode.set(nodeId, category);
  const group = effectGroupForPluginCategory(category);
  for (const element of Array.from(document.querySelectorAll<HTMLElement>(".signal-node[data-node-id]"))) {
    if (element.dataset.nodeId === nodeId) {
      element.dataset.effectGroup = group;
    }
  }
  for (const element of Array.from(document.querySelectorAll<HTMLElement>(".default-effect-shell[data-host-node-id]"))) {
    if (element.dataset.hostNodeId === nodeId) {
      element.dataset.effectGroup = group;
    }
  }
  if (resourceId) {
    rememberResourceCategory(resourceId, category);
    for (const element of Array.from(document.querySelectorAll<HTMLElement>(".plugin-host-item[data-resource-id]"))) {
      if (element.dataset.resourceId === resourceId) {
        element.dataset.effectGroup = group;
      }
    }
  }
}

// ── Applying ───────────────────────────────────────────────────────────────────

const STYLE_ELEMENT_ID = "effect-group-colors";

/**
 * Puts settings in force: the mode and strength on the root (css/effect-groups.css reads them)
 * and each group's colour as a rule of its own.
 */
export function applyEffectGroupColorSettings(settings: EffectGroupColorSettings): void {
  const root = document.documentElement;
  root.dataset.effectColors = settings.mode;
  root.dataset.effectColorStrength = settings.strength;

  let style = document.getElementById(STYLE_ELEMENT_ID) as HTMLStyleElement | null;
  if (!style) {
    style = document.createElement("style");
    style.id = STYLE_ELEMENT_ID;
    document.head.appendChild(style);
  }
  const fill = FILL_FRACTION[settings.strength];
  style.textContent = EFFECT_GROUPS.map((group) => {
    const color = groupColor(settings, group.id);
    return `[data-effect-group="${group.id}"]{--effect-group-color:${color};--effect-group-ink:${inkFor(color, fill)}}`;
  }).join("\n");
}

/** How much of a box's face is the group colour, per strength (css/effect-groups.css). */
const FILL_FRACTION: Record<EffectColorStrength, number> = { subtle: 0.22, medium: 0.42, strong: 0.68 };

/** The card colour the group colour is mixed into: dark in every theme the boxes are drawn on. */
const CARD_RGB: readonly [number, number, number] = [38, 38, 46];

/**
 * The text colour for a box filled with `hex` at `fill`: white, or near-black on a face light
 * enough that white would wash out (a yellow Dynamics box at full strength).
 */
function inkFor(hex: string, fill: number): string {
  const match = /^#?([0-9a-f]{6})$/i.exec(hex.trim());
  if (!match) {
    return "#ffffff";
  }
  const value = parseInt(match[1], 16);
  const channels = [(value >> 16) & 255, (value >> 8) & 255, value & 255].map((channel, index) => {
    const mixed = (channel * fill + CARD_RGB[index] * (1 - fill)) / 255;
    return mixed <= 0.03928 ? mixed / 12.92 : ((mixed + 0.055) / 1.055) ** 2.4;
  });
  const luminance = 0.2126 * channels[0] + 0.7152 * channels[1] + 0.0722 * channels[2];
  // The contrast with white (1.05 / (L + 0.05)) against that with near-black ((L + 0.05) / 0.06).
  return 1.05 / (luminance + 0.05) >= (luminance + 0.05) / 0.06 ? "#ffffff" : "#16161c";
}
