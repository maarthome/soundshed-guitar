#include "StandaloneSystemStats.h"

#if JUCE_LINUX

//==============================================================================
StandaloneSystemStats::StandaloneSystemStats(Sources sources, SendToUI sendToUI)
    : mSources(std::move(sources)), mSendToUI(std::move(sendToUI))
{
}

StandaloneSystemStats::~StandaloneSystemStats()
{
    stopTimer();
}

void StandaloneSystemStats::handleRequest(const juce::var& message)
{
    if (!static_cast<bool>(message.getProperty("enabled", true)))
    {
        stopTimer();
        mSamples.clear();
        return;
    }

    if (isTimerRunning())
    {
        return;
    }

    // A first reading now, so the first message a second from now already has a load.
    mSamples.clear();

    if (const auto times = readProcessorTimes())
    {
        mSamples.push_back(*times);
    }

    startTimer(kIntervalMs);
}

std::optional<StandaloneSystemStats::ProcessorTimes> StandaloneSystemStats::parseProcessorTimes(
    const juce::String& procStat)
{
    // The first line is every core together:
    //   cpu  user nice system idle iowait irq softirq steal guest guest_nice
    // Guest time is already counted in user and nice, so only the first eight are added.
    const auto line = procStat.upToFirstOccurrenceOf("\n", false, false);

    juce::StringArray fields;
    fields.addTokens(line, " ", {});
    fields.removeEmptyStrings();

    if (fields.size() < 5 || fields[0] != "cpu")
    {
        return std::nullopt;
    }

    ProcessorTimes times;
    juce::int64 idle = 0;

    for (int index = 1; index < fields.size() && index <= 8; ++index)
    {
        if (!fields[index].containsOnly("0123456789"))
        {
            return std::nullopt;
        }

        const auto ticks = fields[index].getLargeIntValue();
        times.total += ticks;

        // idle, and iowait: waiting for a disk is not work.
        if (index == 4 || index == 5)
        {
            idle += ticks;
        }
    }

    times.busy = times.total - idle;
    return times;
}

//==============================================================================
void StandaloneSystemStats::timerCallback()
{
    if (const auto times = readProcessorTimes())
    {
        mSamples.push_back(*times);

        // kAverageSeconds of change needs one more reading than that.
        while (static_cast<int>(mSamples.size()) > kAverageSeconds + 1)
        {
            mSamples.pop_front();
        }
    }

    if (!mSendToUI)
    {
        return;
    }

    juce::DynamicObject::Ptr stats(new juce::DynamicObject());
    stats->setProperty("type", "systemStats");
    stats->setProperty("available", true);

    if (const auto load = averageLoad())
    {
        stats->setProperty("cpuPercent", juce::roundToInt(*load * 10.0) / 10.0);
    }

    if (const auto temperature = readTemperature())
    {
        stats->setProperty("temperatureC", juce::roundToInt(*temperature * 10.0) / 10.0);
    }

    if (const auto dropouts = mSources.dropouts ? mSources.dropouts() : -1; dropouts >= 0)
    {
        stats->setProperty("xruns", dropouts);
    }

    // One decimal place is all the figures have.
    mSendToUI(juce::JSON::toString(juce::var(stats.get()), true, 1));
}

std::optional<StandaloneSystemStats::ProcessorTimes> StandaloneSystemStats::readProcessorTimes() const
{
    return parseProcessorTimes(mSources.processorTimes.loadFileAsString());
}

std::optional<double> StandaloneSystemStats::readTemperature() const
{
    // Thousandths of a degree Celsius, as text.
    const auto text = mSources.temperature.loadFileAsString().trim();

    if (text.isEmpty() || !text.containsOnly("-0123456789"))
    {
        return std::nullopt;
    }

    return static_cast<double>(text.getLargeIntValue()) / 1000.0;
}

std::optional<double> StandaloneSystemStats::averageLoad() const
{
    if (mSamples.size() < 2)
    {
        return std::nullopt;
    }

    const auto total = mSamples.back().total - mSamples.front().total;
    const auto busy = mSamples.back().busy - mSamples.front().busy;

    // No ticks passed, or the counters went backwards (they do not, short of a restart).
    if (total <= 0 || busy < 0)
    {
        return std::nullopt;
    }

    return juce::jlimit(0.0, 100.0, 100.0 * static_cast<double>(busy) / static_cast<double>(total));
}

#endif // JUCE_LINUX
