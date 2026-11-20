#include "StandaloneSystemPower.h"

#if JUCE_LINUX

namespace
{
/// How long `systemctl poweroff` gets to answer. It returns as soon as the shutdown is under way.
constexpr int kPowerOffTimeoutMs = 20000;
} // namespace

StandaloneSystemPower::StandaloneSystemPower(Hooks hooks, SendToUI sendToUI)
    : mHooks(std::move(hooks)), mSendToUI(std::move(sendToUI))
{
}

StandaloneSystemPower::~StandaloneSystemPower()
{
    mAlive->store(false);
}

bool StandaloneSystemPower::commandExists(const juce::String& command)
{
    if (command.isEmpty())
    {
        return false;
    }

    if (command.containsChar('/'))
    {
        return juce::File(command).existsAsFile();
    }

    juce::StringArray folders;
    folders.addTokens(juce::SystemStats::getEnvironmentVariable("PATH", "/usr/bin:/bin"), ":", "");

    for (const auto& folder : folders)
    {
        if (folder.isNotEmpty() && juce::File(folder).getChildFile(command).existsAsFile())
        {
            return true;
        }
    }

    return false;
}

void StandaloneSystemPower::handleRequest(const juce::var& message)
{
    const auto action = message.getProperty("action", {}).toString();

    if (action == "exit")
    {
        if (mHooks.quitApp)
        {
            // Not from inside the UI's message: the window that sent it goes as the app quits.
            juce::MessageManager::callAsync([quit = mHooks.quitApp] { quit(); });
        }

        return;
    }

    if (action == "powerOff")
    {
        powerOff();
        return;
    }

    if (action != "getState")
    {
        mError = "Unknown power action: " + action;
    }

    sendState();
}

void StandaloneSystemPower::powerOff()
{
    if (mPoweringOff)
    {
        sendState();
        return;
    }

    if (!commandExists(mHooks.powerOffCommand[0]))
    {
        mError = "This system has no " + mHooks.powerOffCommand[0] + " to power off with.";
        sendState();
        return;
    }

    if (mHooks.saveState)
    {
        mHooks.saveState();
    }

    mPoweringOff = true;
    mError.clear();
    sendState();

    juce::Thread::launch([command = mHooks.powerOffCommand, alive = mAlive, this] {
        juce::ChildProcess process;
        juce::String error;

        if (!process.start(command, juce::ChildProcess::wantStdOut | juce::ChildProcess::wantStdErr))
        {
            error = "Could not start " + command.joinIntoString(" ") + ".";
        }
        else
        {
            const auto output = process.readAllProcessOutput().trim();

            if (!process.waitForProcessToFinish(kPowerOffTimeoutMs))
            {
                process.kill();
                error = command.joinIntoString(" ") + " did not answer.";
            }
            else if (process.getExitCode() != 0)
            {
                error = output.isNotEmpty() ? output : command.joinIntoString(" ") + " was refused.";
            }
        }

        // Success needs no answer: the computer is going down. A refusal goes back to the UI.
        if (error.isEmpty())
        {
            return;
        }

        juce::MessageManager::callAsync([alive, this, error] {
            if (!alive->load())
            {
                return;
            }

            mPoweringOff = false;
            mError = error;
            sendState();
        });
    });
}

void StandaloneSystemPower::sendState()
{
    if (!mSendToUI)
    {
        return;
    }

    juce::DynamicObject::Ptr state(new juce::DynamicObject());
    state->setProperty("type", "systemPowerState");
    state->setProperty("available", true);
    state->setProperty("canPowerOff", commandExists(mHooks.powerOffCommand[0]));
    state->setProperty("poweringOff", mPoweringOff);
    state->setProperty("error", mError);
    mSendToUI(juce::JSON::toString(juce::var(state.get()), true));
    mError.clear();
}

#endif // JUCE_LINUX
