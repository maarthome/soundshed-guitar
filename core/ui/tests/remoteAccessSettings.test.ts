import { describe, expect, it } from "vitest";
import {
  generateRemoteAccessPassword,
  normalizeRemoteAccessState,
  remoteAccessPasswordProblem,
  remoteAccessStatus,
  remoteAccessTargets,
  remoteAccessWebAddresses,
  remoteAccessWebStatus,
  type RemoteAccessState,
} from "../ts/settings/remoteAccess.js";

function state(overrides: Partial<RemoteAccessState> = {}): RemoteAccessState {
  return {
    canRun: true,
    problem: "",
    enabled: false,
    running: false,
    hasPassword: false,
    username: "soundshed",
    port: 5900,
    addresses: ["192.168.1.20"],
    minPasswordLength: 6,
    maxPasswordLength: 64,
    error: "",
    webEnabled: false,
    webRunning: false,
    webCanRun: true,
    webProblem: "",
    webPort: 6080,
    webError: "",
    ...overrides,
  };
}

describe("remote access settings", () => {
  it("has no state where the host has no remote access", () => {
    expect(normalizeRemoteAccessState({ type: "remoteAccessState", available: false })).toBeNull();
    expect(normalizeRemoteAccessState(null)).toBeNull();
    expect(normalizeRemoteAccessState("nope")).toBeNull();
  });

  it("reads a snapshot and fills in what is missing", () => {
    expect(normalizeRemoteAccessState({ available: true })).toEqual(state({ canRun: false, username: "", addresses: [], webCanRun: false }));
    expect(
      normalizeRemoteAccessState({
        available: true,
        canRun: true,
        enabled: true,
        running: true,
        hasPassword: true,
        username: "soundshed",
        port: 5901,
        addresses: ["10.0.0.2", 7, ""],
        error: "x",
        webEnabled: true,
        webRunning: true,
        webCanRun: true,
        webPort: 6081,
        webError: "y",
      }),
    ).toEqual(
      state({ enabled: true, running: true, hasPassword: true, port: 5901, addresses: ["10.0.0.2"], error: "x", webEnabled: true, webRunning: true, webPort: 6081, webError: "y" }),
    );
  });

  it("says where a viewer connects", () => {
    expect(remoteAccessTargets(state())).toBe("192.168.1.20:5900");
    expect(remoteAccessTargets(state({ addresses: ["10.0.0.2", "192.168.1.20"] }))).toBe("10.0.0.2:5900 or 192.168.1.20:5900");
    expect(remoteAccessTargets(state({ addresses: [] }))).toBe("this device's network address, port 5900");
  });

  it("describes each state under the switch", () => {
    expect(remoteAccessStatus(state())).toBe("Off. Set a password to turn it on.");
    expect(remoteAccessStatus(state({ hasPassword: true }))).toBe("Off.");
    expect(remoteAccessStatus(state({ hasPassword: true, enabled: true, running: true }))).toBe(
      'On. Connect a VNC viewer to 192.168.1.20:5900, user name "soundshed", with the password below.',
    );
    expect(remoteAccessStatus(state({ hasPassword: true, enabled: true }))).toContain("not running");
    expect(remoteAccessStatus(state({ canRun: false, problem: "Install wayvnc" }))).toBe("Install wayvnc");
  });

  it("describes browser access under its switch", () => {
    expect(remoteAccessWebAddresses(state())).toBe("https://192.168.1.20:6080");
    expect(remoteAccessWebAddresses(state({ addresses: ["10.0.0.2", "192.168.1.20"] }))).toBe("https://10.0.0.2:6080 or https://192.168.1.20:6080");
    expect(remoteAccessWebStatus(state())).toMatch(/^Off\. /);
    expect(remoteAccessWebStatus(state({ webCanRun: false, webProblem: "Install novnc" }))).toBe("Install novnc");
    expect(remoteAccessWebStatus(state({ webEnabled: true }))).toBe("On, and starts when remote access is turned on.");
    expect(remoteAccessWebStatus(state({ enabled: true, running: true, webEnabled: true }))).toContain("not running");
    expect(remoteAccessWebStatus(state({ enabled: true, running: true, webEnabled: true, webRunning: true }))).toBe(
      'On. Open https://192.168.1.20:6080 in a web browser (https, not http), choose to continue past the certificate warning, and sign in with user name "soundshed" and the password above.',
    );
  });

  it("applies the engine's password rule", () => {
    expect(remoteAccessPasswordProblem("abcdef")).toBe("");
    expect(remoteAccessPasswordProblem("Gu1tar=Pass!")).toBe("");
    expect(remoteAccessPasswordProblem("abc")).toContain("at least 6");
    expect(remoteAccessPasswordProblem("a".repeat(65))).toContain("at most 64");
    for (const bad of ["has space1", "hash#mark1", "café12345", "tab\there1"]) {
      expect(remoteAccessPasswordProblem(bad)).toContain("no spaces");
    }
  });

  it("generates passwords that pass that rule and are easy to read", () => {
    let seed = 1;
    const bytes = (count: number): number[] =>
      Array.from({ length: count }, () => {
        seed = (seed * 1103515245 + 12345) % 2147483648;
        return (seed >> 8) % 256;
      });
    for (let index = 0; index < 50; index += 1) {
      const password = generateRemoteAccessPassword(bytes);
      expect(password).toMatch(/^[a-km-np-zA-HJ-NP-Z2-9]{12}$/);
      expect(remoteAccessPasswordProblem(password)).toBe("");
    }
    expect(generateRemoteAccessPassword((count) => new Array<number>(count).fill(0), 4)).toBe("aaaa");
  });

  it("gives up rather than hang on a source of bytes it cannot use", () => {
    expect(generateRemoteAccessPassword((count) => new Array<number>(count).fill(255))).toBe("");
  });
});
