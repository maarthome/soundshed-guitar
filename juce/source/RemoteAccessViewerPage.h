#pragma once

/**
 * soundshed.html, the browser viewer the remote access web folder serves (my_gui). noVNC's own
 * vnc.html reads a password from the address but never a user name, so against the wayvnc server
 * here, which wants both, it always stopped at its login dialog. This page uses noVNC's RFB
 * client (core/rfb.js, linked in from the installed noVNC) and gives it both.
 *
 * - `soundshed.html?password=...` (and `&username=...`, default "soundshed") logs in at once.
 * - Without a password it shows a small form; "Remember in this browser" keeps the password in
 *   this browser's storage, so the plain address logs in next time.
 * - The screen is scaled to the window, and a dropped connection is tried again every 3 s.
 *
 * Written by StandaloneRemoteAccess::prepareWebFolder; the front page sends a browser here.
 */
constexpr const char* kRemoteAccessViewerPage = R"html(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Soundshed Guitar</title>
<style>
  html, body { margin: 0; height: 100%; background: #111; color: #ddd; font: 15px system-ui, sans-serif; overflow: hidden; }
  #screen { position: fixed; inset: 0; }
  #status { position: fixed; left: 50%; top: 12px; transform: translateX(-50%); padding: 6px 14px; border-radius: 16px;
            background: rgba(0, 0, 0, 0.7); font-size: 13px; z-index: 2; pointer-events: none; }
  #status[hidden], #login[hidden] { display: none; }
  #login { position: fixed; inset: 0; display: flex; align-items: center; justify-content: center; z-index: 3; background: #111; }
  #login form { display: grid; gap: 10px; width: min(320px, 90vw); padding: 22px; border-radius: 12px; background: #1d1e22;
                box-shadow: 0 8px 30px rgba(0, 0, 0, 0.5); }
  #login h1 { margin: 0 0 4px; font-size: 18px; color: #fff; }
  #login label { display: grid; gap: 4px; font-size: 13px; color: #aaa; }
  #login input[type=text], #login input[type=password] { padding: 9px 10px; border-radius: 6px; border: 1px solid #444;
                background: #111; color: #fff; font: inherit; }
  #login .remember { display: flex; gap: 8px; align-items: center; color: #ddd; }
  #login button { padding: 10px; border: 0; border-radius: 6px; background: #e07848; color: #fff; font: inherit; font-weight: 600; cursor: pointer; }
  #error { margin: 0; color: #f07070; font-size: 13px; }
  #error:empty { display: none; }
</style>
</head>
<body>
<div id="screen"></div>
<div id="status" hidden></div>
<div id="login" hidden>
  <form id="login-form" autocomplete="on">
    <h1>Soundshed Guitar</h1>
    <label>User name <input type="text" id="login-username" name="username" autocomplete="username" autocapitalize="off" spellcheck="false"></label>
    <label>Password <input type="password" id="login-password" name="password" autocomplete="current-password"></label>
    <label class="remember"><input type="checkbox" id="login-remember"> Remember in this browser</label>
    <p id="error" role="alert"></p>
    <button type="submit">Connect</button>
  </form>
</div>
<script type="module">
import RFB from "./core/rfb.js";

const STORAGE_KEY = "soundshed-remote-access";
const RECONNECT_MS = 3000;
const params = new URLSearchParams(location.search);
const screen = document.getElementById("screen");
const status = document.getElementById("status");
const login = document.getElementById("login");

let credentials = null;
let rfb = null;
let reconnectTimer = 0;
let failed = false;

function stored() {
  try {
    const value = JSON.parse(localStorage.getItem(STORAGE_KEY) || "null");
    return value && typeof value.password === "string" ? value : null;
  } catch {
    return null;
  }
}

function store(value) {
  try {
    if (value) {
      localStorage.setItem(STORAGE_KEY, JSON.stringify(value));
    } else {
      localStorage.removeItem(STORAGE_KEY);
    }
  } catch {
    // Storage may be off in this browser: then the form simply asks again next time.
  }
}

function showStatus(text) {
  status.textContent = text;
  status.hidden = !text;
}

function askForCredentials(error) {
  clearTimeout(reconnectTimer);
  showStatus("");
  document.getElementById("login-username").value = (credentials && credentials.username) || params.get("username") || "soundshed";
  document.getElementById("login-password").value = "";
  document.getElementById("login-remember").checked = Boolean(stored());
  document.getElementById("error").textContent = error || "";
  login.hidden = false;
  document.getElementById("login-password").focus();
}

function connect() {
  clearTimeout(reconnectTimer);
  failed = false;
  if (rfb) {
    rfb.disconnect();
  }
  const url = (location.protocol === "https:" ? "wss://" : "ws://") + location.host + "/websockify";
  showStatus("Connecting…");
  rfb = new RFB(screen, url, { credentials: { username: credentials.username, password: credentials.password }, shared: true });
  rfb.scaleViewport = params.get("resize") !== "off";
  rfb.resizeSession = false;
  rfb.addEventListener("connect", () => showStatus(""));
  rfb.addEventListener("credentialsrequired", () => rfb.sendCredentials({ username: credentials.username, password: credentials.password }));
  rfb.addEventListener("securityfailure", (event) => {
    failed = true;
    store(null);
    askForCredentials("Not accepted: " + ((event.detail && event.detail.reason) || "check the user name and password") + ".");
  });
  rfb.addEventListener("disconnect", () => {
    if (failed || params.get("reconnect") === "false") {
      if (!failed) {
        showStatus("Disconnected.");
      }
      return;
    }
    showStatus("Connection lost, trying again…");
    reconnectTimer = setTimeout(connect, RECONNECT_MS);
  });
}

document.getElementById("login-form").addEventListener("submit", (event) => {
  event.preventDefault();
  credentials = {
    username: document.getElementById("login-username").value.trim() || "soundshed",
    password: document.getElementById("login-password").value,
  };
  store(document.getElementById("login-remember").checked ? credentials : null);
  login.hidden = true;
  connect();
});

const fromAddress = params.get("password");
if (fromAddress !== null) {
  credentials = { username: params.get("username") || "soundshed", password: fromAddress };
} else {
  credentials = stored();
}

if (credentials) {
  connect();
} else {
  askForCredentials("");
}
</script>
</body>
</html>
)html";
