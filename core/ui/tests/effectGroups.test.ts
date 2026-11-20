import { describe, expect, it } from "vitest";
import {
  EFFECT_GROUPS,
  effectGroupForBuiltIn,
  effectGroupForPluginCategory,
  groupColor,
  normalizeEffectGroupColorSettings,
} from "../ts/effectGroups.js";
import { hexToRgb, hsvToRgb, rgbToHex, rgbToHsv } from "../ts/settings/effectColors.js";

describe("effect groups (my_gui)", () => {
  it("files hosted plugins by their LV2 class or VST3 sub-categories", () => {
    const cases: Record<string, string> = {
      "Reverb Plugin": "reverb", "Delay Plugin": "delay", "Distortion Plugin": "distortion",
      "Waveshaper Plugin": "distortion", "Amplifier Plugin": "amp", "Simulator Plugin": "amp",
      "Compressor Plugin": "dynamics", "Limiter Plugin": "dynamics", "Gate Plugin": "noise",
      "EQ Plugin": "eq", "Parametric EQ Plugin": "eq", "Multi EQ Plugin": "eq", "Filter Plugin": "filter",
      "Highpass Plugin": "filter", "Chorus Plugin": "modulation", "Flanger Plugin": "modulation",
      "Phaser Plugin": "modulation", "Pitch Shifter Plugin": "pitch", "Spatial Plugin": "spatial",
      "Instrument Plugin": "synth", "Oscillator Plugin": "synth", "Utility Plugin": "utility",
      "Analyser Plugin": "utility", "Plugin": "utility", "": "utility",
      "Fx|Reverb": "reverb", "Fx|Delay": "delay", "Fx|Restoration": "noise", "Fx|Dynamics": "dynamics",
      "Fx|EQ": "eq", "Fx|Pitch Shift": "pitch", "Fx|Tools": "utility", "Instrument|Synth": "synth",
    };
    for (const [category, group] of Object.entries(cases)) {
      expect([category, effectGroupForPluginCategory(category)]).toEqual([category, group]);
    }
  });

  it("files built-in effects by category, and a few by name", () => {
    expect(effectGroupForBuiltIn("dynamics", "Noise Gate")).toBe("noise");
    expect(effectGroupForBuiltIn("dynamics", "Noise Reduction")).toBe("noise");
    expect(effectGroupForBuiltIn("dynamics", "VCA Compressor")).toBe("dynamics");
    expect(effectGroupForBuiltIn("drive", "Overdrive")).toBe("drive");
    expect(effectGroupForBuiltIn("drive", "Multi-band Distortion")).toBe("distortion");
    expect(effectGroupForBuiltIn("modulation", "Wah")).toBe("filter");
    expect(effectGroupForBuiltIn("modulation", "Spatial 3D")).toBe("spatial");
    expect(effectGroupForBuiltIn("pedal", "Pedal")).toBe("amp");
    expect(effectGroupForBuiltIn("cab", "IR Cab")).toBe("cab");
    expect(effectGroupForBuiltIn("nonsense", "X")).toBe("utility");
  });

  it("keeps only usable, changed colours from what was stored", () => {
    const settings = normalizeEffectGroupColorSettings({
      mode: "user", strength: "loud", colors: { reverb: "#FF6FB0", amp: "#e07848", bogus: "#123456", delay: "red" },
    });
    expect(settings).toEqual({ mode: "user", strength: "medium", colors: { reverb: "#ff6fb0" } });
    expect(groupColor(settings, "reverb")).toBe("#ff6fb0");
    expect(groupColor(settings, "amp")).toBe("#e07848");
    expect(normalizeEffectGroupColorSettings(undefined)).toEqual({ mode: "default", strength: "medium", colors: {} });
  });

  it("has a distinct default colour for every group", () => {
    expect(new Set(EFFECT_GROUPS.map((group) => group.color)).size).toBe(EFFECT_GROUPS.length);
  });

  it("turns colours into hue, saturation and brightness and back without loss", () => {
    for (const hex of ["#ff0000", "#3dff64", "#808080", "#000000", "#ffffff", "#48d080", "#c0306a"]) {
      const hsv = rgbToHsv(hexToRgb(hex));
      expect(rgbToHex(hsvToRgb(hsv.h, hsv.s, hsv.v))).toBe(hex);
    }
    expect(rgbToHex({ r: 300, g: -4, b: 127.6 })).toBe("#ff0080");
    expect(rgbToHsv({ r: 0, g: 0, b: 255 })).toEqual({ h: 240, s: 1, v: 1 });
  });
});
