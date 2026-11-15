#include "StandaloneRemoteAccess.h"

#if JUCE_LINUX

    #include <sys/stat.h>

namespace
{
constexpr int kPollIntervalMs = 1000;
/// Long enough for an RSA key on a small board.
constexpr int kKeyGenerationTimeoutMs = 60000;
constexpr int kGracefulExitMs = 1500;

juce::String environmentVariable(const char* name)
{
    return juce::SystemStats::getEnvironmentVariable(name, {});
}

/// The program's full path if it is on PATH, else an empty string.
juce::String findProgram(const juce::String& name)
{
    juce::StringArray folders;
    folders.addTokens(environmentVariable("PATH"), ":", {});

    for (const auto& folder : folders)
    {
        if (folder.isEmpty() || !juce::File::isAbsolutePath(folder))
        {
            continue;
        }

        const auto candidate = juce::File(folder).getChildFile(name);

        if (candidate.existsAsFile())
        {
            return candidate.getFullPathName();
        }
    }

    return {};
}

/// Runs a short-lived helper to the end. Its output is not wanted.
bool runAndWait(const juce::StringArray& arguments, int timeoutMs)
{
    juce::ChildProcess process;

    if (!process.start(arguments, 0))
    {
        return false;
    }

    if (!process.waitForProcessToFinish(timeoutMs))
    {
        process.kill();
        return false;
    }

    return process.getExitCode() == 0;
}

void restrictToOwner(const juce::File& file)
{
    ::chmod(file.getFullPathName().toRawUTF8(), file.isDirectory() ? 0700 : 0600);
}

/// Writes a file only its owner can read. The folder it goes into is already closed to
/// everyone else, which covers the moment before the permissions are set.
bool writeOwnerOnly(const juce::File& file, const juce::String& text)
{
    // "\n" endings: the default is "\r\n", which would leave a stray character on the end
    // of every value in wayvnc's config. Replacing makes a new file, so the permissions
    // are set afterwards.
    if (!file.replaceWithText(text, false, false, "\n"))
    {
        return false;
    }

    restrictToOwner(file);
    return true;
}
} // namespace

//==============================================================================
StandaloneRemoteAccess::StandaloneRemoteAccess(juce::File settingsFolder, SendToUI sendToUI)
    : mFolder(std::move(settingsFolder)), mSendToUI(std::move(sendToUI))
{
    loadSettings();

    if (mEnabled)
    {
        mError = startServer();
    }
}

StandaloneRemoteAccess::~StandaloneRemoteAccess()
{
    stopTimer();
    stopServer();
}

void StandaloneRemoteAccess::handleRequest(const juce::var& message)
{
    const auto action = message.getProperty("action", {}).toString();
    juce::String requestError;

    if (action == "setEnabled")
    {
        requestError = setEnabled(static_cast<bool>(message.getProperty("enabled", false)));
    }
    else if (action == "setPassword")
    {
        requestError = setPassword(message.getProperty("password", {}).toString());
    }
    else if (action != "getState")
    {
        requestError = "Unknown remote access action: " + action;
    }

    if (requestError.isNotEmpty())
    {
        mError = requestError;
    }

    sendState();
}

juce::String StandaloneRemoteAccess::passwordProblem(const juce::String& password)
{
    if (password.length() < kMinPasswordLength)
    {
        return "The password needs at least " + juce::String(kMinPasswordLength) + " characters.";
    }

    if (password.length() > kMaxPasswordLength)
    {
        return "The password can have at most " + juce::String(kMaxPasswordLength) + " characters.";
    }

    // It goes into wayvnc's config file as "password=<value>", one line, with the
    // whitespace around it dropped, and has to be typed into any viewer.
    for (const auto character : password)
    {
        if (character <= 0x20 || character >= 0x7f || character == '#')
        {
            return "Use letters, digits and punctuation only: no spaces, no accented letters and no #.";
        }
    }

    return {};
}

//==============================================================================
void StandaloneRemoteAccess::timerCallback()
{
    if (mServer != nullptr && !mServer->isRunning())
    {
        // It went away by itself: the port was taken, the compositor refused it, or it crashed.
        mServer.reset();
        stopTimer();

        const auto details = lastLogLines();
        mError = "The VNC server stopped." + (details.isNotEmpty() ? " " + details : juce::String());
        sendState();
    }
}

void StandaloneRemoteAccess::sendState()
{
    if (!mSendToUI)
    {
        return;
    }

    const auto problem = environmentProblem();

    juce::Array<juce::var> addresses;

    for (const auto& address : juce::IPAddress::getAllAddresses(false))
    {
        if (address != juce::IPAddress::local(false))
        {
            addresses.add(address.toString());
        }
    }

    juce::DynamicObject::Ptr state(new juce::DynamicObject());
    state->setProperty("type", "remoteAccessState");
    state->setProperty("available", true);
    state->setProperty("canRun", problem.isEmpty());
    state->setProperty("problem", problem);
    state->setProperty("enabled", mEnabled);
    state->setProperty("running", isServerRunning());
    state->setProperty("hasPassword", mPassword.isNotEmpty());
    state->setProperty("username", kUserName);
    state->setProperty("port", kPort);
    state->setProperty("addresses", addresses);
    state->setProperty("minPasswordLength", kMinPasswordLength);
    state->setProperty("maxPasswordLength", kMaxPasswordLength);
    state->setProperty("error", mError);

    mSendToUI(juce::JSON::toString(juce::var(state.get()), true));
}

juce::String StandaloneRemoteAccess::setEnabled(bool enabled)
{
    if (!enabled)
    {
        mEnabled = false;
        mError.clear();
        saveSettings();
        stopServer();
        return {};
    }

    if (mPassword.isEmpty())
    {
        return "Set a password before turning remote access on.";
    }

    // Turned on already and still up: nothing to restart.
    if (mEnabled && isServerRunning())
    {
        return {};
    }

    auto startError = startServer();

    // Stays off if it could not start, so the switch shows what is true.
    mEnabled = startError.isEmpty();
    saveSettings();

    if (startError.isEmpty())
    {
        mError.clear();
    }

    return startError;
}

juce::String StandaloneRemoteAccess::setPassword(const juce::String& password)
{
    if (auto problem = passwordProblem(password); problem.isNotEmpty())
    {
        return problem;
    }

    mPassword = password;
    mError.clear();
    saveSettings();

    // A running server read its config when it started: start it again with the new one.
    if (mEnabled)
    {
        auto startError = startServer();

        if (startError.isNotEmpty())
        {
            mEnabled = false;
            saveSettings();
        }

        return startError;
    }

    return {};
}

//==============================================================================
juce::String StandaloneRemoteAccess::startServer()
{
    stopServer();

    if (auto problem = environmentProblem(); problem.isNotEmpty())
    {
        return problem;
    }

    if (mPassword.isEmpty())
    {
        return "Set a password before turning remote access on.";
    }

    if (!mFolder.createDirectory().wasOk())
    {
        return "Could not create " + mFolder.getFullPathName();
    }

    restrictToOwner(mFolder);

    if (!ensureKeys())
    {
        return "Could not create the encryption keys. Install openssl or openssh-client and try again.";
    }

    if (!writeServerConfig())
    {
        return "Could not write " + configFile().getFullPathName();
    }

    // One left behind by a run of the app that did not get to stop it would hold the port.
    askStaleServerToExit();

    logFile().deleteFile();

    // Through a shell only to send wayvnc's output to its log: a pipe nobody reads would
    // fill up and stall it. "exec" keeps one process, so the handle below is wayvnc itself.
    juce::StringArray command;
    command.add("/bin/sh");
    command.add("-c");
    command.add(R"(exec "$1" -C "$2" -S "$3" >"$4" 2>&1)");
    command.add("soundshed-remote-access");
    command.add(findProgram("wayvnc"));
    command.add(configFile().getFullPathName());
    command.add(controlSocket().getFullPathName());
    command.add(logFile().getFullPathName());

    auto server = std::make_unique<juce::ChildProcess>();

    if (!server->start(command, 0))
    {
        return "Could not start wayvnc.";
    }

    // A bad config or a taken port ends it within a moment; say so now rather than
    // showing "on" and correcting it a second later.
    if (server->waitForProcessToFinish(400))
    {
        const auto details = lastLogLines();
        return "The VNC server could not start." + (details.isNotEmpty() ? " " + details : juce::String());
    }

    mServer = std::move(server);
    startTimer(kPollIntervalMs);
    return {};
}

void StandaloneRemoteAccess::stopServer()
{
    stopTimer();

    if (mServer == nullptr)
    {
        return;
    }

    if (mServer->isRunning())
    {
        // Ask first, so viewers are told the session has ended.
        askStaleServerToExit();

        if (!mServer->waitForProcessToFinish(kGracefulExitMs))
        {
            mServer->kill();
            mServer->waitForProcessToFinish(kGracefulExitMs);
        }
    }

    mServer.reset();
    controlSocket().deleteFile();
}

bool StandaloneRemoteAccess::isServerRunning() const
{
    return mServer != nullptr && mServer->isRunning();
}

juce::String StandaloneRemoteAccess::environmentProblem() const
{
    if (environmentVariable("WAYLAND_DISPLAY").isEmpty())
    {
        return "Remote access needs the app to run in a Wayland session (labwc or cage), "
               "as run_pi_kiosk.sh starts it. It cannot share the screen of an X11 desktop.";
    }

    if (findProgram("wayvnc").isEmpty())
    {
        return "The VNC server is not installed. Install it with: sudo apt install wayvnc";
    }

    return {};
}

bool StandaloneRemoteAccess::ensureKeys()
{
    const auto haveTls = [this] { return tlsKeyFile().existsAsFile() && tlsCertificateFile().existsAsFile(); };
    const auto haveRsa = [this] { return rsaKeyFile().existsAsFile(); };

    if (!haveTls())
    {
        // Half a pair is no use, and openssl would stop to ask before overwriting.
        tlsKeyFile().deleteFile();
        tlsCertificateFile().deleteFile();

        if (const auto openssl = findProgram("openssl"); openssl.isNotEmpty())
        {
            runAndWait({openssl, "req", "-x509", "-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:secp384r1", "-sha384",
                        "-days", "3650", "-nodes", "-subj", "/CN=soundshed", "-keyout", tlsKeyFile().getFullPathName(),
                        "-out", tlsCertificateFile().getFullPathName()},
                       kKeyGenerationTimeoutMs);
        }

        if (!haveTls())
        {
            tlsKeyFile().deleteFile();
            tlsCertificateFile().deleteFile();
        }
    }

    if (!haveRsa())
    {
        if (const auto sshKeygen = findProgram("ssh-keygen"); sshKeygen.isNotEmpty())
        {
            // Through a shell for the empty passphrase: ChildProcess drops empty arguments.
            runAndWait({"/bin/sh", "-c", R"(exec "$1" -q -m pem -t rsa -N "" -f "$2")", "soundshed-remote-access",
                        sshKeygen, rsaKeyFile().getFullPathName()},
                       kKeyGenerationTimeoutMs);
        }
    }

    for (const auto& key : {tlsKeyFile(), rsaKeyFile()})
    {
        if (key.existsAsFile())
        {
            restrictToOwner(key);
        }
    }

    return haveTls() || haveRsa();
}

bool StandaloneRemoteAccess::writeServerConfig() const
{
    // Key-value lines only: wayvnc's manual promises nothing about comments. The file is
    // rewritten on every start, so changes made to it by hand do not last.
    juce::StringArray lines;
    lines.add("use_relative_paths=true");
    lines.add("address=0.0.0.0");
    lines.add("port=" + juce::String(kPort));
    lines.add("enable_auth=true");
    lines.add("username=" + juce::String(kUserName));
    lines.add("password=" + mPassword);

    if (tlsKeyFile().existsAsFile() && tlsCertificateFile().existsAsFile())
    {
        lines.add("private_key_file=" + tlsKeyFile().getFileName());
        lines.add("certificate_file=" + tlsCertificateFile().getFileName());
    }

    if (rsaKeyFile().existsAsFile())
    {
        lines.add("rsa_private_key_file=" + rsaKeyFile().getFileName());
    }

    return writeOwnerOnly(configFile(), lines.joinIntoString("\n") + "\n");
}

void StandaloneRemoteAccess::askStaleServerToExit() const
{
    const auto socket = controlSocket();

    if (!socket.exists())
    {
        return;
    }

    if (const auto wayvncctl = findProgram("wayvncctl"); wayvncctl.isNotEmpty())
    {
        runAndWait({wayvncctl, "-S", socket.getFullPathName(), "wayvnc-exit"}, kGracefulExitMs);
    }
}

juce::String StandaloneRemoteAccess::lastLogLines() const
{
    juce::StringArray lines;
    lines.addLines(logFile().loadFileAsString());
    lines.removeEmptyStrings();

    while (lines.size() > 2)
    {
        lines.remove(0);
    }

    return lines.joinIntoString(" ").substring(0, 300);
}

//==============================================================================
void StandaloneRemoteAccess::loadSettings()
{
    const auto parsed = juce::JSON::parse(settingsFile().loadFileAsString());

    if (parsed.getDynamicObject() == nullptr)
    {
        return;
    }

    const auto password = parsed.getProperty("password", {}).toString();

    // One edited by hand into something wayvnc's config cannot hold is treated as not set.
    if (passwordProblem(password).isEmpty())
    {
        mPassword = password;
    }

    mEnabled = static_cast<bool>(parsed.getProperty("enabled", false)) && mPassword.isNotEmpty();
}

void StandaloneRemoteAccess::saveSettings() const
{
    if (!mFolder.createDirectory().wasOk())
    {
        return;
    }

    restrictToOwner(mFolder);

    juce::DynamicObject::Ptr settings(new juce::DynamicObject());
    settings->setProperty("enabled", mEnabled);
    settings->setProperty("password", mPassword);

    writeOwnerOnly(settingsFile(), juce::JSON::toString(juce::var(settings.get())));
}

juce::File StandaloneRemoteAccess::settingsFile() const
{
    return mFolder.getChildFile("settings.json");
}

juce::File StandaloneRemoteAccess::configFile() const
{
    return mFolder.getChildFile("wayvnc.config");
}

juce::File StandaloneRemoteAccess::logFile() const
{
    return mFolder.getChildFile("wayvnc.log");
}

juce::File StandaloneRemoteAccess::controlSocket() const
{
    // A unix socket's path is limited to about a hundred characters, which the profile
    // folder can exceed; the session's runtime folder is short, and gone after a reboot.
    const auto runtimeFolder = environmentVariable("XDG_RUNTIME_DIR");

    if (runtimeFolder.isNotEmpty() && juce::File::isAbsolutePath(runtimeFolder))
    {
        return juce::File(runtimeFolder).getChildFile("soundshed-wayvncctl");
    }

    return mFolder.getChildFile("ctl");
}

juce::File StandaloneRemoteAccess::tlsKeyFile() const
{
    return mFolder.getChildFile("tls_key.pem");
}

juce::File StandaloneRemoteAccess::tlsCertificateFile() const
{
    return mFolder.getChildFile("tls_cert.pem");
}

juce::File StandaloneRemoteAccess::rsaKeyFile() const
{
    return mFolder.getChildFile("rsa_key.pem");
}

#endif // JUCE_LINUX
