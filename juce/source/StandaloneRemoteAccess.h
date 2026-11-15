#pragma once

#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>

#if JUCE_LINUX

    #include <functional>
    #include <memory>

//==============================================================================
/**
 * Remote access to the standalone app's screen over VNC, switched on and off from
 * Settings > Remote Access.
 *
 * For a device with only a small touch screen (a Pi in a pedal, say): a VNC viewer on
 * another computer, tablet or phone shows the app and operates it. The server is
 * wayvnc, started as a child process of the app and stopped with it, so it needs the
 * app to be running in a wlroots Wayland session (labwc or cage; run_pi_kiosk.sh
 * starts one). The app itself is an X11 client there, but it inherits WAYLAND_DISPLAY
 * from the compositor that launched it, and that is all wayvnc needs.
 *
 * Access always takes a user name and a password, and the connection is encrypted:
 * wayvnc refuses password authentication without keys, so a TLS certificate and an
 * RSA key are made the first time the server is turned on (openssl and ssh-keygen).
 * Between them they cover the common viewers: TigerVNC and others that speak VeNCrypt
 * use the certificate, RealVNC Viewer the RSA key.
 *
 * Browser access is a second switch on top of that, for when installing a viewer is
 * not wanted: websockify serves noVNC (a VNC viewer that is a web page) on kWebPort and
 * relays it to a second wayvnc that listens on this device only. A browser cannot do the
 * encrypted sign-ins above, so that second server takes Apple's Diffie-Hellman sign-in
 * instead, which noVNC speaks: the same user name and password, still protected, but the
 * picture and the input that follow are not. That is the trade the switch makes, and why
 * it is separate, off by default, and leaves the server viewers use as strict as it was.
 *
 * Everything lives in <profile>/data/v1/settings/remote-access/, readable by the user
 * only: settings.json (the switches and the password), wayvnc's config (which has to
 * hold the password in plain text, hence the permissions), the keys, the web folder
 * noVNC is served from, and the two servers' logs.
 *
 * The UI sends {"type": "remoteAccess", "action": ...} and every request is answered
 * with a whole "remoteAccessState" snapshot, as is the server stopping on its own. The
 * password is never sent back: the state only says whether one is set. See
 * docs/user-interface.md for the protocol.
 *
 * Message thread only.
 */
class StandaloneRemoteAccess final : private juce::Timer
{
  public:
    using SendToUI = std::function<void(const juce::String& json)>;

    /// The user name a viewer signs in with. Fixed, so the password is the only thing to set.
    static constexpr const char* kUserName = "soundshed";
    static constexpr int kPort = 5900;
    /// Where a browser finds the noVNC page.
    static constexpr int kWebPort = 6080;
    /// The VNC server behind that page. Reachable from this device only.
    static constexpr int kWebServerPort = 5901;
    static constexpr int kMinPasswordLength = 6;
    static constexpr int kMaxPasswordLength = 64;

    /// Starts the server straight away if it was left on.
    StandaloneRemoteAccess(juce::File settingsFolder, SendToUI sendToUI);
    /// Stops the server.
    ~StandaloneRemoteAccess() override;

    /// One "remoteAccess" message from the UI.
    void handleRequest(const juce::var& message);

    /// Empty if the password can be used, else why not, in words for the user.
    [[nodiscard]] static juce::String passwordProblem(const juce::String& password);

  private:
    void timerCallback() override;

    void sendState();

    juce::String setEnabled(bool enabled);
    juce::String setPassword(const juce::String& password);
    void setWebEnabled(bool enabled);

    /// Empty once wayvnc is running, else why it could not be started.
    juce::String startServer();
    void stopServer();
    [[nodiscard]] bool isServerRunning() const;

    /// Why the server cannot run here at all (no Wayland session, wayvnc missing); empty if it can.
    [[nodiscard]] juce::String environmentProblem() const;

    // Browser access: websockify serving noVNC, relayed to a VNC server of its own.
    /// Empty once both are running, else why they could not be started.
    juce::String startWebProxy();
    void stopWebProxy();
    [[nodiscard]] bool isWebProxyRunning() const;
    /// Why browser access cannot run here (websockify or noVNC missing); empty if it can.
    [[nodiscard]] juce::String webEnvironmentProblem() const;
    /// The folder websockify serves: noVNC's files, and a front page that opens the viewer.
    bool prepareWebFolder() const;
    /// Ends a websockify left behind by a run of the app that did not get to stop it.
    void endStaleWebProxy() const;

    /// Makes whichever of the TLS certificate and the RSA key is missing. False if neither exists afterwards.
    bool ensureKeys();
    bool writeServerConfig() const;
    bool writeWebServerConfig() const;
    /// Asks the wayvnc behind a control socket to exit; nothing happens if there is none.
    static void askServerToExit(const juce::File& socket);
    [[nodiscard]] static juce::String lastLogLines(const juce::File& log);

    void loadSettings();
    void saveSettings() const;

    [[nodiscard]] juce::File settingsFile() const;
    [[nodiscard]] juce::File configFile() const;
    [[nodiscard]] juce::File logFile() const;
    [[nodiscard]] juce::File controlSocket() const;
    [[nodiscard]] juce::File tlsKeyFile() const;
    [[nodiscard]] juce::File tlsCertificateFile() const;
    [[nodiscard]] juce::File rsaKeyFile() const;
    [[nodiscard]] juce::File webServerConfigFile() const;
    [[nodiscard]] juce::File webServerLogFile() const;
    [[nodiscard]] juce::File webServerControlSocket() const;
    [[nodiscard]] juce::File webFolder() const;
    [[nodiscard]] juce::File webLogFile() const;
    [[nodiscard]] juce::File webPidFile() const;

    juce::File mFolder;
    SendToUI mSendToUI;

    bool mEnabled = false;
    bool mWebEnabled = false;
    juce::String mPassword;

    std::unique_ptr<juce::ChildProcess> mServer;
    /// Why the server is not running although it is turned on. Cleared by the next start.
    juce::String mError;

    std::unique_ptr<juce::ChildProcess> mWebServer;
    std::unique_ptr<juce::ChildProcess> mWebProxy;
    /// The same for browser access.
    juce::String mWebError;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(StandaloneRemoteAccess)
};

#endif // JUCE_LINUX
