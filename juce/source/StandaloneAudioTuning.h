#pragma once

#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>

#if JUCE_LINUX

    #include <functional>
    #include <optional>

//==============================================================================
/**
 * Two low-level audio settings of the standalone app on Linux, behind Settings > Audio & MIDI:
 * the realtime priority of the audio thread, and how many periods the ALSA buffer holds.
 * With the buffer size, which the audio device settings already have, these are the three
 * numbers a JACK user sets (-P, -n and -p).
 *
 * Realtime priority. JUCE's ALSA device runs the audio callback on an ordinary thread, which
 * takes its turn with the web view and anything else and is then late with a block now and
 * then: dropouts on a machine that is mostly idle. applyOnAudioThread(), called at the top
 * of every block, puts that thread in SCHED_FIFO at the priority chosen (or back to an
 * ordinary thread at 0) whenever the choice changes, and records what the kernel granted.
 * It needs an rtprio limit: run_pi_kiosk.sh gives one, as does the audio group on most
 * distributions. With no choice saved, the priority is SOUNDSHED_AUDIO_PRIO, else
 * kDefaultPriority.
 *
 * Periods. The device buffer is the block size times the periods: fewer is less output
 * latency, and less time for a late block to catch up. JUCE asks for 4 and has no setting,
 * so juce/patches/juce-alsa-periods.patch makes it read SOUNDSHED_ALSA_PERIODS when it opens
 * a device; this class sets that variable, at start before the first device opens and on a
 * change, and then has the device reopened. Where the patch is not applied the setting is
 * not offered (Hooks::periodsSupported).
 *
 * Both are kept in <profile>/data/v1/settings/audio-tuning.json. The UI sends
 * {"type": "audioTuning", "action": ...} and gets a whole "audioTuningState" each time; see
 * docs/user-interface.md.
 *
 * The instance is message thread only. applyOnAudioThread() is for the audio thread, and
 * touches nothing but atomics and the calling thread's own scheduling.
 */
class StandaloneAudioTuning final : private juce::Timer
{
  public:
    using SendToUI = std::function<void(const juce::String& json)>;

    static constexpr int kDefaultPriority = 70;
    /// The top few priorities are left to the kernel's own threads.
    static constexpr int kMaxPriority = 95;
    static constexpr int kDefaultPeriods = 4;
    static constexpr int kMinPeriods = 2;
    static constexpr int kMaxPeriods = 4;

    struct Hooks
    {
        /// Closes and reopens the audio device as it was set up, so a new periods count takes effect.
        std::function<void()> reopenDevice;
        /// True when JUCE's ALSA device reads SOUNDSHED_ALSA_PERIODS (the patch is applied).
        bool periodsSupported = false;
        /// Where the kernel reports the open devices' buffers. Tests point it elsewhere.
        juce::File procAsound{"/proc/asound"};
    };

    StandaloneAudioTuning(juce::File settingsFile, Hooks hooks, SendToUI sendToUI);
    ~StandaloneAudioTuning() override;

    /// One "audioTuning" message from the UI.
    void handleRequest(const juce::var& message);

    /// <profile>/data/v1/settings/audio-tuning.json
    [[nodiscard]] static juce::File defaultSettingsFile();

    /// Puts the saved choices in force before any audio device is opened: call it first thing
    /// at start, on the main thread, before other threads exist (it sets environment variables).
    static void applySavedAtStartup(const juce::File& settingsFile);

    /// Audio thread, at the top of every block. Cheap when nothing changed: one atomic load.
    static void applyOnAudioThread() noexcept;

    /// The highest realtime priority this process may ask for; 0 when it may not at all.
    [[nodiscard]] static int permittedPriority() noexcept;

    /// What the audio thread last got: its SCHED_FIFO priority, 0 for an ordinary thread,
    /// -1 before it has run a block.
    [[nodiscard]] static int appliedPriority() noexcept;

    /// The period size and buffer size, in frames, of the first open playback device; nothing when none is open.
    struct ActiveBuffer
    {
        int periodSize = 0;
        int bufferSize = 0;
    };

    [[nodiscard]] static std::optional<ActiveBuffer> readActiveBuffer(const juce::File& procAsound);

  private:
    struct Settings
    {
        /// Nothing saved: the default applies (see defaultPriority()).
        std::optional<int> priority;
        std::optional<int> periods;
    };

    void timerCallback() override;
    void sendState();

    juce::String setPriority(int priority);
    juce::String setPeriods(int periods);

    [[nodiscard]] static Settings load(const juce::File& settingsFile);
    void save() const;

    /// SOUNDSHED_AUDIO_PRIO if it holds a usable number, else kDefaultPriority.
    [[nodiscard]] static int defaultPriority();
    static void setRequestedPriority(int priority) noexcept;

    juce::File mSettingsFile;
    Hooks mHooks;
    SendToUI mSendToUI;
    Settings mSettings;
    juce::String mError;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(StandaloneAudioTuning)
};

#endif // JUCE_LINUX
