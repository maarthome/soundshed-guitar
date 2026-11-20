#pragma once

#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>

#if JUCE_LINUX

    #include <atomic>
    #include <functional>
    #include <memory>

//==============================================================================
/**
 * The power button at the bottom of the rail (my_gui): Exit App, or Power Off the computer the
 * standalone app runs on, from a touch screen with nothing else to do it with.
 *
 * Exit asks the app to quit the way closing its window does, so the window and plugin state are
 * saved. Power Off saves the same state first, then runs `systemctl poweroff`: logind lets the
 * user at the screen (an active local session, which run_pi_kiosk.sh's kiosk is) do that without
 * a password. If it is refused, the UI is told why and the app carries on.
 *
 * The UI sends {"type": "systemPower", "action": "getState" | "exit" | "powerOff"} and gets
 * "systemPowerState" {available, canPowerOff, poweringOff, error}; see docs/user-interface.md.
 * Message thread only; the power-off command runs on a thread of its own.
 */
class StandaloneSystemPower final
{
  public:
    using SendToUI = std::function<void(const juce::String& json)>;

    struct Hooks
    {
        /// Saves what quitting would: the window and plugin state, the settings file.
        std::function<void()> saveState;
        /// Quits the app as closing its window does (and so saves as well).
        std::function<void()> quitApp;
        /// The command that powers the computer off. Tests put something harmless here.
        juce::StringArray powerOffCommand{"systemctl", "poweroff"};
    };

    StandaloneSystemPower(Hooks hooks, SendToUI sendToUI);
    ~StandaloneSystemPower();

    /// One "systemPower" message from the UI.
    void handleRequest(const juce::var& message);

    /// The first word of the power-off command, found on the PATH or as given.
    [[nodiscard]] static bool commandExists(const juce::String& command);

  private:
    void sendState();
    void powerOff();

    Hooks mHooks;
    SendToUI mSendToUI;
    bool mPoweringOff = false;
    juce::String mError;
    /// Lets the power-off thread find out, when it is done, whether this object still exists.
    std::shared_ptr<std::atomic<bool>> mAlive = std::make_shared<std::atomic<bool>>(true);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(StandaloneSystemPower)
};

#endif // JUCE_LINUX
