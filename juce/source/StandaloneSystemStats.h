#pragma once

#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>

#if JUCE_LINUX

    #include <deque>
    #include <functional>
    #include <optional>

//==============================================================================
/**
 * How hard the machine is working, for the readout in the middle of the footer bar:
 * the processor's load, its temperature, and the audio device's dropouts.
 *
 * For a small board (a Pi in a pedal) these are the three things that explain
 * crackling: the processor is busy, it is hot enough to slow itself down, or blocks
 * are arriving late. The DSP load the app already shows says how long its own
 * processing takes; this is the whole machine, the UI and everything else included.
 *
 *  - Load is all cores together, from /proc/stat, averaged over the last
 *    kAverageSeconds so the figure can be read rather than chased.
 *  - Temperature is the first thermal zone's, which on a Pi is the processor.
 *  - Dropouts are the audio device's count (xruns), from the callback given.
 *
 * The UI sends {"type": "watchSystemStats", "enabled": true} once it is up, and gets a
 * "systemStats" message every second from then on. A figure this machine cannot give is
 * left out of the message. See docs/user-interface.md for the protocol.
 *
 * Message thread only.
 */
class StandaloneSystemStats final : private juce::Timer
{
  public:
    using SendToUI = std::function<void(const juce::String& json)>;

    static constexpr int kIntervalMs = 1000;
    static constexpr int kAverageSeconds = 5;

    /// Where the figures come from. The defaults are the real ones; tests point them elsewhere.
    struct Sources
    {
        juce::File processorTimes{"/proc/stat"};
        juce::File temperature{"/sys/class/thermal/thermal_zone0/temp"};
        /// The audio device's dropout count, negative when there is no device to ask.
        std::function<int()> dropouts;
    };

    StandaloneSystemStats(Sources sources, SendToUI sendToUI);
    ~StandaloneSystemStats() override;

    /// One "watchSystemStats" message from the UI.
    void handleRequest(const juce::var& message);

    /// The busy and total processor time in a /proc/stat "cpu" line, in ticks; nothing if it is not one.
    struct ProcessorTimes
    {
        juce::int64 busy = 0;
        juce::int64 total = 0;
    };

    [[nodiscard]] static std::optional<ProcessorTimes> parseProcessorTimes(const juce::String& procStat);

  private:
    void timerCallback() override;

    [[nodiscard]] std::optional<ProcessorTimes> readProcessorTimes() const;
    [[nodiscard]] std::optional<double> readTemperature() const;
    /// The load over the samples held, as a percentage; nothing until there are two.
    [[nodiscard]] std::optional<double> averageLoad() const;

    Sources mSources;
    SendToUI mSendToUI;

    /// One reading a second, oldest first: the load is the change from the first to the last.
    std::deque<ProcessorTimes> mSamples;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(StandaloneSystemStats)
};

#endif // JUCE_LINUX
