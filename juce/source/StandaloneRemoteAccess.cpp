#include "StandaloneRemoteAccess.h"

#if JUCE_LINUX

    #include <csignal>
    #include <sys/stat.h>
    #include <sys/types.h>

namespace
{
constexpr int kPollIntervalMs = 1000;
/// Long enough for an RSA key on a small board.
constexpr int kKeyGenerationTimeoutMs = 60000;
constexpr int kGracefulExitMs = 1500;
/// How long a server gets to fail at once (a taken port, a bad config) before it counts as up.
constexpr int kEarlyExitMs = 400;
/// Where Debian's noVNC package puts the viewer page and its scripts.
constexpr const char* kDefaultNoVncFolder = "/usr/share/novnc";
/// What the front page sends a browser on to: connect at once, fit the window, and come back after a drop.
constexpr const char* kNoVncPage = "vnc.html?autoconnect=true&resize=scale&reconnect=true";

juce::String environmentVariable(const char* name)
{
    return juce::SystemStats::getEnvironmentVariable(name, {});
}

/// noVNC's folder: SOUNDSHED_NOVNC_FOLDER for a copy kept somewhere else, else the package's.
juce::File noVncFolder()
{
    const auto chosen = environmentVariable("SOUNDSHED_NOVNC_FOLDER");
    return juce::File(chosen.isNotEmpty() && juce::File::isAbsolutePath(chosen) ? chosen
                                                                                : juce::String(kDefaultNoVncFolder));
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
    else if (action == "setWebEnabled")
    {
        setWebEnabled(static_cast<bool>(message.getProperty("enabled", false)));
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
    auto changed = false;

    if (mServer != nullptr && !mServer->isRunning())
    {
        // It went away by itself: the port was taken, the compositor refused it, or it crashed.
        mServer.reset();
        // Nothing left to relay to.
        stopWebProxy();

        const auto details = lastLogLines(logFile());
        mError = "The VNC server stopped." + (details.isNotEmpty() ? " " + details : juce::String());
        changed = true;
    }

    // Browser access needs both of its servers; either going takes it down.
    const auto webServerDied = mWebServer != nullptr && !mWebServer->isRunning();
    const auto webProxyDied = mWebProxy != nullptr && !mWebProxy->isRunning();

    if (webServerDied || webProxyDied)
    {
        const auto details = lastLogLines(webServerDied ? webServerLogFile() : webLogFile());
        stopWebProxy();
        mWebError = "Browser access stopped." + (details.isNotEmpty() ? " " + details : juce::String());
        changed = true;
    }

    if (mServer == nullptr && mWebServer == nullptr && mWebProxy == nullptr)
    {
        stopTimer();
    }

    if (changed)
    {
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

    const auto webProblem = webEnvironmentProblem();
    state->setProperty("webEnabled", mWebEnabled);
    state->setProperty("webRunning", isWebProxyRunning());
    state->setProperty("webCanRun", webProblem.isEmpty());
    state->setProperty("webProblem", webProblem);
    state->setProperty("webPort", kWebPort);
    state->setProperty("webError", mWebError);

    mSendToUI(juce::JSON::toString(juce::var(state.get()), true));
}

juce::String StandaloneRemoteAccess::setEnabled(bool enabled)
{
    if (!enabled)
    {
        mEnabled = false;
        mError.clear();
        mWebError.clear();
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

void StandaloneRemoteAccess::setWebEnabled(bool enabled)
{
    mWebEnabled = enabled;
    mWebError.clear();
    saveSettings();

    // Browser access has servers of its own, so viewers that are connected stay connected.
    if (!enabled)
    {
        stopWebProxy();
    }
    else if (isServerRunning())
    {
        mWebError = startWebProxy();
    }
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
    askServerToExit(controlSocket());

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
    if (server->waitForProcessToFinish(kEarlyExitMs))
    {
        const auto details = lastLogLines(logFile());
        return "The VNC server could not start." + (details.isNotEmpty() ? " " + details : juce::String());
    }

    mServer = std::move(server);
    startTimer(kPollIntervalMs);

    // Browser access rides on the server. Failing to start it is its own error: a viewer
    // can still connect.
    mWebError = mWebEnabled ? startWebProxy() : juce::String();
    return {};
}

void StandaloneRemoteAccess::stopServer()
{
    stopTimer();
    stopWebProxy();

    if (mServer == nullptr)
    {
        return;
    }

    if (mServer->isRunning())
    {
        // Ask first, so viewers are told the session has ended.
        askServerToExit(controlSocket());

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

//==============================================================================
juce::String StandaloneRemoteAccess::startWebProxy()
{
    stopWebProxy();

    if (auto problem = webEnvironmentProblem(); problem.isNotEmpty())
    {
        return problem;
    }

    // noVNC signs in to a VNC server of its own, reachable from this device only; see
    // writeWebServerConfig() for why it cannot use the one viewers connect to.
    if (!rsaKeyFile().existsAsFile())
    {
        return "Browser access needs an RSA key. Install openssh-client, then turn remote access off and on.";
    }

    if (!writeWebServerConfig())
    {
        return "Could not write " + webServerConfigFile().getFullPathName();
    }

    if (!prepareWebFolder())
    {
        return "Could not prepare " + webFolder().getFullPathName();
    }

    // Ones left behind by a run of the app that did not get to stop them would hold the ports.
    askServerToExit(webServerControlSocket());
    endStaleWebProxy();

    webServerLogFile().deleteFile();
    webLogFile().deleteFile();

    juce::StringArray serverCommand;
    serverCommand.add("/bin/sh");
    serverCommand.add("-c");
    serverCommand.add(R"(exec "$1" -C "$2" -S "$3" >"$4" 2>&1)");
    serverCommand.add("soundshed-remote-access");
    serverCommand.add(findProgram("wayvnc"));
    serverCommand.add(webServerConfigFile().getFullPathName());
    serverCommand.add(webServerControlSocket().getFullPathName());
    serverCommand.add(webServerLogFile().getFullPathName());

    auto server = std::make_unique<juce::ChildProcess>();

    if (!server->start(serverCommand, 0))
    {
        return "Could not start wayvnc for browser access.";
    }

    if (server->waitForProcessToFinish(kEarlyExitMs))
    {
        const auto details = lastLogLines(webServerLogFile());
        return "Browser access could not start." + (details.isNotEmpty() ? " " + details : juce::String());
    }

    mWebServer = std::move(server);

    // Through a shell for the same reason as wayvnc, and to leave the process id behind
    // for endStaleWebProxy(): websockify has no control socket to ask it to exit through.
    juce::StringArray proxyCommand;
    proxyCommand.add("/bin/sh");
    proxyCommand.add("-c");
    proxyCommand.add(R"(echo $$ >"$5"; exec "$1" --web "$2" "$3" "$4" >"$6" 2>&1)");
    proxyCommand.add("soundshed-remote-access");
    proxyCommand.add(findProgram("websockify"));
    proxyCommand.add(webFolder().getFullPathName());
    proxyCommand.add("0.0.0.0:" + juce::String(kWebPort));
    proxyCommand.add("127.0.0.1:" + juce::String(kWebServerPort));
    proxyCommand.add(webPidFile().getFullPathName());
    proxyCommand.add(webLogFile().getFullPathName());

    auto proxy = std::make_unique<juce::ChildProcess>();

    if (!proxy->start(proxyCommand, 0))
    {
        stopWebProxy();
        return "Could not start websockify.";
    }

    if (proxy->waitForProcessToFinish(kEarlyExitMs))
    {
        const auto details = lastLogLines(webLogFile());
        stopWebProxy();
        return "Browser access could not start." + (details.isNotEmpty() ? " " + details : juce::String());
    }

    mWebProxy = std::move(proxy);

    // Its own watch: browser access can be turned on while the main server is already up.
    startTimer(kPollIntervalMs);
    return {};
}

void StandaloneRemoteAccess::stopWebProxy()
{
    if (mWebProxy != nullptr)
    {
        if (mWebProxy->isRunning())
        {
            mWebProxy->kill();
            mWebProxy->waitForProcessToFinish(kGracefulExitMs);
        }

        mWebProxy.reset();
        webPidFile().deleteFile();
    }

    if (mWebServer != nullptr)
    {
        if (mWebServer->isRunning())
        {
            askServerToExit(webServerControlSocket());

            if (!mWebServer->waitForProcessToFinish(kGracefulExitMs))
            {
                mWebServer->kill();
                mWebServer->waitForProcessToFinish(kGracefulExitMs);
            }
        }

        mWebServer.reset();
        webServerControlSocket().deleteFile();
    }
}

bool StandaloneRemoteAccess::isWebProxyRunning() const
{
    return mWebProxy != nullptr && mWebProxy->isRunning() && mWebServer != nullptr && mWebServer->isRunning();
}

juce::String StandaloneRemoteAccess::webEnvironmentProblem() const
{
    if (findProgram("websockify").isEmpty() || !noVncFolder().getChildFile("vnc.html").existsAsFile())
    {
        return "Browser access is not installed. Install it with: sudo apt install novnc websockify";
    }

    return {};
}

bool StandaloneRemoteAccess::prepareWebFolder() const
{
    const auto folder = webFolder();

    // Made afresh each time, so it follows the installed noVNC. The links inside are not
    // followed: only the links themselves go.
    folder.deleteRecursively(false);

    if (!folder.createDirectory().wasOk())
    {
        return false;
    }

    for (const auto& entry : noVncFolder().findChildFiles(juce::File::findFilesAndDirectories, false))
    {
        // The front page is ours.
        if (entry.getFileName() != "index.html")
        {
            entry.createSymbolicLink(folder.getChildFile(entry.getFileName()), true);
        }
    }

    const juce::String page(kNoVncPage);
    const auto escapedPage = page.replace("&", "&amp;");

    juce::StringArray html;
    html.add("<!DOCTYPE html>");
    html.add("<html lang=\"en\">");
    html.add("<head>");
    html.add("<meta charset=\"utf-8\">");
    html.add("<title>Soundshed Guitar</title>");
    html.add("<meta http-equiv=\"refresh\" content=\"0; url=" + escapedPage + "\">");
    html.add("</head>");
    html.add("<body><a href=\"" + escapedPage + "\">Open Soundshed Guitar</a></body>");
    html.add("</html>");

    return folder.getChildFile("index.html").replaceWithText(html.joinIntoString("\n") + "\n", false, false, "\n") &&
           folder.getChildFile("vnc.html").exists();
}

void StandaloneRemoteAccess::endStaleWebProxy() const
{
    const auto pidFile = webPidFile();
    const auto pid = pidFile.loadFileAsString().trim().getIntValue();
    pidFile.deleteFile();

    // 0 and 1 would signal a whole process group or init.
    if (pid <= 1)
    {
        return;
    }

    // Only if that process is still websockify: the id may have gone to something else since.
    const juce::File process("/proc/" + juce::String(pid));

    // The arguments are separated by zero bytes, which would end a string at the first one
    // (the interpreter, for a script such as websockify).
    juce::MemoryBlock commandLine;
    process.getChildFile("cmdline").loadFileAsData(commandLine);

    for (auto& byte : commandLine)
    {
        if (byte == 0)
        {
            byte = ' ';
        }
    }

    if (!commandLine.toString().contains("websockify"))
    {
        return;
    }

    ::kill(static_cast<pid_t>(pid), SIGTERM);

    for (auto waited = 0; waited < kGracefulExitMs && process.exists(); waited += 50)
    {
        juce::Thread::sleep(50);
    }
}

//==============================================================================
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

bool StandaloneRemoteAccess::writeWebServerConfig() const
{
    // A browser cannot do TLS or RSA-AES inside a VNC connection, so noVNC cannot sign in
    // to the server viewers use. It gets one of its own that:
    //  - listens on this device only (websockify relays the browser to it);
    //  - has no TLS certificate, since a server that has one offers VeNCrypt first, which
    //    noVNC accepts and then cannot complete;
    //  - allows Apple's Diffie-Hellman sign-in (relax_encryption), which noVNC speaks. That
    //    protects the user name and password, and nothing after them.
    juce::StringArray lines;
    lines.add("use_relative_paths=true");
    lines.add("address=127.0.0.1");
    lines.add("port=" + juce::String(kWebServerPort));
    lines.add("enable_auth=true");
    lines.add("username=" + juce::String(kUserName));
    lines.add("password=" + mPassword);
    lines.add("rsa_private_key_file=" + rsaKeyFile().getFileName());
    lines.add("relax_encryption=true");

    return writeOwnerOnly(webServerConfigFile(), lines.joinIntoString("\n") + "\n");
}

void StandaloneRemoteAccess::askServerToExit(const juce::File& socket)
{
    if (!socket.exists())
    {
        return;
    }

    if (const auto wayvncctl = findProgram("wayvncctl"); wayvncctl.isNotEmpty())
    {
        runAndWait({wayvncctl, "-S", socket.getFullPathName(), "wayvnc-exit"}, kGracefulExitMs);
    }
}

juce::String StandaloneRemoteAccess::lastLogLines(const juce::File& log)
{
    juce::StringArray lines;
    lines.addLines(log.loadFileAsString());
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
    mWebEnabled = static_cast<bool>(parsed.getProperty("webEnabled", false));
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
    settings->setProperty("webEnabled", mWebEnabled);
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

juce::File StandaloneRemoteAccess::webServerConfigFile() const
{
    return mFolder.getChildFile("wayvnc-web.config");
}

juce::File StandaloneRemoteAccess::webServerLogFile() const
{
    return mFolder.getChildFile("wayvnc-web.log");
}

juce::File StandaloneRemoteAccess::webServerControlSocket() const
{
    return juce::File(controlSocket().getFullPathName() + "-web");
}

juce::File StandaloneRemoteAccess::webFolder() const
{
    return mFolder.getChildFile("web");
}

juce::File StandaloneRemoteAccess::webLogFile() const
{
    return mFolder.getChildFile("websockify.log");
}

juce::File StandaloneRemoteAccess::webPidFile() const
{
    return mFolder.getChildFile("websockify.pid");
}

#endif // JUCE_LINUX
