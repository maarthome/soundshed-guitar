#include "StandaloneAudioTuning.h"

#if JUCE_LINUX

    #include "ProfileFolder.h"

    #include <atomic>
    #include <cstdlib>
    #include <pthread.h>
    #include <sched.h>
    #include <sys/resource.h>
    #include <thread>

namespace
{
/// The priority the audio thread should have: 0 is an ordinary thread, -1 is "not decided yet"
/// (nothing called applySavedAtStartup), in which case the audio thread works out the default.
std::atomic<int> gRequestedPriority{-1};
/// Bumped on every change, so the audio thread applies it once and then only compares.
std::atomic<unsigned int> gGeneration{1};
std::atomic<int> gAppliedPriority{-1};

/// How long the audio thread gets to run a block and apply a change before the state is sent again.
constexpr int kFollowUpMs = 400;

bool isUsablePriority(int priority)
{
    return priority >= 0 && priority <= StandaloneAudioTuning::kMaxPriority;
}

bool isUsablePeriods(int periods)
{
    return periods >= StandaloneAudioTuning::kMinPeriods && periods <= StandaloneAudioTuning::kMaxPeriods;
}

/// The whole of a string as a number, or nothing: "70" but not "70%" or "".
std::optional<int> wholeNumber(const juce::String& text)
{
    const auto trimmed = text.trim();

    if (trimmed.isEmpty() || trimmed.length() > 6 || !trimmed.containsOnly("0123456789"))
    {
        return std::nullopt;
    }

    return trimmed.getIntValue();
}

std::optional<int> numberProperty(const juce::var& object, const char* name)
{
    const auto value = object.getProperty(name, {});

    if (value.isInt() || value.isInt64() || value.isDouble())
    {
        return juce::roundToInt(static_cast<double>(value));
    }

    return std::nullopt;
}
} // namespace

//==============================================================================
StandaloneAudioTuning::StandaloneAudioTuning(juce::File settingsFile, Hooks hooks, SendToUI sendToUI)
    : mSettingsFile(std::move(settingsFile)), mHooks(std::move(hooks)), mSendToUI(std::move(sendToUI)),
      mSettings(load(mSettingsFile))
{
}

StandaloneAudioTuning::~StandaloneAudioTuning()
{
    stopTimer();
}

juce::File StandaloneAudioTuning::defaultSettingsFile()
{
    return soundshed::profileFolder()
        .getChildFile("data")
        .getChildFile("v1")
        .getChildFile("settings")
        .getChildFile("audio-tuning.json");
}

void StandaloneAudioTuning::handleRequest(const juce::var& message)
{
    const auto action = message.getProperty("action", {}).toString();
    mError.clear();

    if (action == "setRealtimePriority")
    {
        mError = setPriority(numberProperty(message, "priority").value_or(-1));
    }
    else if (action == "setPeriods")
    {
        mError = setPeriods(numberProperty(message, "periods").value_or(-1));
    }
    else if (action != "getState")
    {
        mError = "Unknown audio tuning action: " + action;
    }

    sendState();

    // What the audio thread was granted, and the buffer the reopened device has, are only
    // known a moment later.
    if (action != "getState" && mError.isEmpty())
    {
        startTimer(kFollowUpMs);
    }
}

void StandaloneAudioTuning::timerCallback()
{
    stopTimer();
    sendState();
}

void StandaloneAudioTuning::sendState()
{
    if (!mSendToUI)
    {
        return;
    }

    juce::DynamicObject::Ptr state(new juce::DynamicObject());
    state->setProperty("type", "audioTuningState");
    state->setProperty("available", true);
    state->setProperty("realtimePriority", mSettings.priority.value_or(defaultPriority()));
    state->setProperty("realtimePriorityMax", juce::jmin(permittedPriority(), kMaxPriority));
    state->setProperty("realtimePriorityApplied", appliedPriority());
    state->setProperty("periodsSupported", mHooks.periodsSupported);
    state->setProperty("periods", mSettings.periods.value_or(kDefaultPeriods));
    state->setProperty("minPeriods", kMinPeriods);
    state->setProperty("maxPeriods", kMaxPeriods);

    if (const auto active = readActiveBuffer(mHooks.procAsound))
    {
        state->setProperty("activePeriodSize", active->periodSize);
        state->setProperty("activeBufferSize", active->bufferSize);
    }

    state->setProperty("error", mError);
    mSendToUI(juce::JSON::toString(juce::var(state.get()), true));
}

juce::String StandaloneAudioTuning::setPriority(int priority)
{
    if (!isUsablePriority(priority))
    {
        return "The realtime priority is a number from 0 (off) to " + juce::String(kMaxPriority) + ".";
    }

    if (const auto permitted = permittedPriority(); priority > permitted)
    {
        return permitted <= 0 ? juce::String("This system does not let the app use realtime priority. Start it with "
                                             "run_pi_kiosk.sh, or add your user to the audio group.")
                              : "This system allows a realtime priority of up to " + juce::String(permitted) + ".";
    }

    mSettings.priority = priority;
    save();
    setRequestedPriority(priority);
    return {};
}

juce::String StandaloneAudioTuning::setPeriods(int periods)
{
    if (!mHooks.periodsSupported)
    {
        return "This build cannot change the number of periods.";
    }

    if (!isUsablePeriods(periods))
    {
        return "The number of periods is " + juce::String(kMinPeriods) + " to " + juce::String(kMaxPeriods) + ".";
    }

    mSettings.periods = periods;
    save();

    // Read by JUCE's ALSA device when it opens one, on this thread, in the reopen below.
    ::setenv("SOUNDSHED_ALSA_PERIODS", juce::String(periods).toRawUTF8(), 1);

    if (mHooks.reopenDevice)
    {
        mHooks.reopenDevice();
    }

    return {};
}

//==============================================================================
void StandaloneAudioTuning::applySavedAtStartup(const juce::File& settingsFile)
{
    const auto settings = load(settingsFile);

    if (settings.periods.has_value())
    {
        ::setenv("SOUNDSHED_ALSA_PERIODS", juce::String(*settings.periods).toRawUTF8(), 1);
    }

    const auto priority = settings.priority.value_or(defaultPriority());

    // The DSP helper threads read this when they start (core's RaisePriorityOfThisThreadForAudio),
    // and have to sit at the audio thread's priority. Off leaves it alone: they then ask for the
    // lowest realtime priority, as they always have without it.
    if (priority > 0)
    {
        ::setenv("SOUNDSHED_AUDIO_PRIO", juce::String(priority).toRawUTF8(), 1);
    }

    setRequestedPriority(priority);
}

void StandaloneAudioTuning::applyOnAudioThread() noexcept
{
    static thread_local unsigned int applied = 0;

    const auto generation = gGeneration.load(std::memory_order_relaxed);

    if (applied == generation)
    {
        return;
    }

    applied = generation;

    auto priority = gRequestedPriority.load(std::memory_order_relaxed);

    if (priority < 0)
    {
        priority = defaultPriority();
    }

    sched_param param{};

    if (priority > 0)
    {
        param.sched_priority =
            juce::jlimit(sched_get_priority_min(SCHED_FIFO), sched_get_priority_max(SCHED_FIFO), priority);
        // Refused without an rtprio limit: the thread then stays as it was.
        pthread_setschedparam(pthread_self(), SCHED_FIFO, &param);
    }
    else
    {
        pthread_setschedparam(pthread_self(), SCHED_OTHER, &param);
    }

    auto policy = SCHED_OTHER;
    sched_param granted{};

    if (pthread_getschedparam(pthread_self(), &policy, &granted) == 0)
    {
        const auto realtime = policy == SCHED_FIFO || policy == SCHED_RR;
        gAppliedPriority.store(realtime ? granted.sched_priority : 0, std::memory_order_relaxed);
    }
}

int StandaloneAudioTuning::permittedPriority() noexcept
{
    const auto highest = sched_get_priority_max(SCHED_FIFO);
    rlimit limit{};

    if (getrlimit(RLIMIT_RTPRIO, &limit) == 0 && limit.rlim_cur > 0)
    {
        if (limit.rlim_cur == RLIM_INFINITY || limit.rlim_cur > static_cast<rlim_t>(highest))
        {
            return highest;
        }

        return static_cast<int>(limit.rlim_cur);
    }

    // No limit to go by, but a process with the capability (root, or CAP_SYS_NICE) may ask
    // for any priority regardless. The only way to know is to try, on a thread of its own
    // so nothing that matters changes class. Once: the answer does not change while running.
    static const bool capable = [] {
        auto granted = false;

        try
        {
            std::thread probe([&granted] {
                sched_param param{};
                param.sched_priority = sched_get_priority_min(SCHED_FIFO);
                granted = pthread_setschedparam(pthread_self(), SCHED_FIFO, &param) == 0;
            });
            probe.join();
        }
        catch (...)
        {
        }

        return granted;
    }();

    return capable ? highest : 0;
}

int StandaloneAudioTuning::appliedPriority() noexcept
{
    return gAppliedPriority.load(std::memory_order_relaxed);
}

std::optional<StandaloneAudioTuning::ActiveBuffer> StandaloneAudioTuning::readActiveBuffer(const juce::File& procAsound)
{
    // /proc/asound/card0/pcm0p/sub0/hw_params: "closed" for a device nobody has open, else
    // lines such as "period_size: 64" and "buffer_size: 256". Playback devices end in "p".
    for (const auto& card : procAsound.findChildFiles(juce::File::findDirectories, false, "card*"))
    {
        for (const auto& pcm : card.findChildFiles(juce::File::findDirectories, false, "pcm*p"))
        {
            for (const auto& sub : pcm.findChildFiles(juce::File::findDirectories, false, "sub*"))
            {
                juce::StringArray lines;
                lines.addLines(sub.getChildFile("hw_params").loadFileAsString());

                ActiveBuffer buffer;

                for (const auto& line : lines)
                {
                    const auto value = wholeNumber(line.fromFirstOccurrenceOf(":", false, false));

                    if (line.startsWith("period_size:"))
                    {
                        buffer.periodSize = value.value_or(0);
                    }
                    else if (line.startsWith("buffer_size:"))
                    {
                        buffer.bufferSize = value.value_or(0);
                    }
                }

                if (buffer.periodSize > 0 && buffer.bufferSize > 0)
                {
                    return buffer;
                }
            }
        }
    }

    return std::nullopt;
}

//==============================================================================
StandaloneAudioTuning::Settings StandaloneAudioTuning::load(const juce::File& settingsFile)
{
    Settings settings;
    const auto parsed = juce::JSON::parse(settingsFile.loadFileAsString());

    if (parsed.getDynamicObject() == nullptr)
    {
        return settings;
    }

    // A value edited by hand into something unusable counts as not saved.
    if (const auto priority = numberProperty(parsed, "realtimePriority"); priority && isUsablePriority(*priority))
    {
        settings.priority = priority;
    }

    if (const auto periods = numberProperty(parsed, "alsaPeriods"); periods && isUsablePeriods(*periods))
    {
        settings.periods = periods;
    }

    return settings;
}

void StandaloneAudioTuning::save() const
{
    if (!mSettingsFile.getParentDirectory().createDirectory().wasOk())
    {
        return;
    }

    juce::DynamicObject::Ptr settings(new juce::DynamicObject());

    if (mSettings.priority.has_value())
    {
        settings->setProperty("realtimePriority", *mSettings.priority);
    }

    if (mSettings.periods.has_value())
    {
        settings->setProperty("alsaPeriods", *mSettings.periods);
    }

    mSettingsFile.replaceWithText(juce::JSON::toString(juce::var(settings.get())), false, false, "\n");
}

int StandaloneAudioTuning::defaultPriority()
{
    if (const char* fromEnvironment = std::getenv("SOUNDSHED_AUDIO_PRIO"))
    {
        if (const auto priority = wholeNumber(fromEnvironment); priority && *priority >= 1 && *priority <= kMaxPriority)
        {
            return *priority;
        }
    }

    return kDefaultPriority;
}

void StandaloneAudioTuning::setRequestedPriority(int priority) noexcept
{
    gRequestedPriority.store(priority, std::memory_order_relaxed);
    gGeneration.fetch_add(1, std::memory_order_relaxed);
}

#endif // JUCE_LINUX
