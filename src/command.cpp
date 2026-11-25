#include "command.h"

#include <chrono>
#include <condition_variable>
#include <mutex>

#include "pr/process.h"

namespace nexus {

namespace {

PrStringView pr(std::string_view s) {
    return PrStringView{s.data(), s.size()};
}

std::string trimmed(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' '))
        s.pop_back();
    return s;
}

// Wakes the reader when the process has output or has exited.
struct Waker {
    std::mutex mutex;
    std::condition_variable woken;
    bool signaled = false;

    static void wake(void* context) {
        Waker& w = *static_cast<Waker*>(context);
        {
            std::lock_guard<std::mutex> lock(w.mutex);
            w.signaled = true;
        }
        w.woken.notify_one();
    }

    void wait() {
        std::unique_lock<std::mutex> lock(mutex);
        woken.wait_for(lock, std::chrono::milliseconds(100), [this] { return signaled; });
        signaled = false;
    }
};

void drain(PrProcess* process, PrProcessStream stream, std::string& into) {
    char buffer[4096];
    size_t got = 0;
    while ((got = prProcessRead(process, stream, reinterpret_cast<uint8_t*>(buffer), sizeof buffer)) > 0)
        into.append(buffer, got);
}

} // namespace
Command novaConfigCommand(const char* override, const char* shell, const std::vector<std::string>& arguments) {
    std::string joined;
    for (const std::string& a : arguments)
        joined += (joined.empty() ? "" : " ") + a;
    if (override != nullptr && *override != 0)
        return Command{override, arguments, std::string(override) + " " + joined};
#if defined(_WIN32)
    // Windows has no login shell to find nova-config through, and a GUI program there gets the person's whole PATH:
    // nova-config is run directly, from PATH.
    (void)shell;
    return Command{"nova-config", arguments, "nova-config " + joined};
#else
    std::string program = shell != nullptr && *shell != 0 ? shell : "/bin/sh";
    // exec: the shell becomes nova-config, so a timeout's kill reaches it.
    std::string line = "exec nova-config " + joined;
    return Command{program, {"-l", "-c", line}, program + " -l -c '" + line + "'"};
#endif
}

std::string commandProblem(const Command& command, const CommandResult& r, int timeoutSeconds) {
    const std::string cmd = "`" + command.shown + "`";
    const std::string fix = "put the full path of nova-config in nexus's client config (a line `nova-config <path>` in "
        "~/.nexus/client/config), or install nova-tools so that nova-config is on your login shell's PATH";
    switch (r.end) {
        case CommandResult::End::NotFound:
            return "cannot run " + cmd + ": " + command.program + " was not found; " + fix;
        case CommandResult::End::NotStarted:
            return "cannot run " + cmd + ": the process could not start; " + fix;
        case CommandResult::End::TimedOut:
            return cmd + " did not finish within " + std::to_string(timeoutSeconds) + " seconds and was stopped; run "
                "it in a terminal to see where it waits (the store may be unreachable)";
        case CommandResult::End::Signaled:
            return cmd + " was ended by signal " + std::to_string(r.code) + "; run it in a terminal to see why";
        case CommandResult::End::Exited:
            break;
    }
    if (r.code == 0)
        return {};
    std::string why = trimmed(r.err);
    std::string problem = cmd + " exited with status " + std::to_string(r.code);
    if (r.code == 127)
        problem += " (command not found)";
    problem += why.empty() ? " and printed nothing on standard error" : ": " + why;
    problem += r.code == 127 ? "; " + fix : "; run it in a terminal to see more";
    return problem;
}

CommandResult runCommand(const Command& command, int timeoutSeconds) {
    return runCommandWithInput(command, timeoutSeconds, {});
}

CommandResult runCommandWithInput(const Command& command, int timeoutSeconds, std::string_view input) {
    CommandResult result;
    PrProcessLauncher* launcher = prProcessLauncherCreate(PrProcessBackend_Default);
    if (launcher == nullptr)
        return result;
    Waker waker;
    std::vector<PrStringView> arguments;
    for (const std::string& a : command.arguments)
        arguments.push_back(pr(a));
    PrProcessDesc desc{};
    desc.program = pr(command.program);
    desc.arguments = arguments.data();
    desc.argumentCount = uint32_t(arguments.size());
    desc.streams = PrProcessStreams_Pipes;
    desc.errors = PrProcessErrors_Pipe;
    desc.wake = &Waker::wake;
    desc.wakeContext = &waker;
    PrProcess* process = nullptr;
    PrStatus launched = prProcessLaunch(launcher, &desc, &process);
    if (launched != PrStatus_Ok || process == nullptr) {
        result.end = launched == PrStatus_NotFound ? CommandResult::End::NotFound : CommandResult::End::NotStarted;
        prProcessLauncherDestroy(launcher);
        return result;
    }
    if (!input.empty()) prProcessWrite(process, {reinterpret_cast<const uint8_t*>(input.data()), input.size()});
    prProcessCloseInput(process);
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeoutSeconds);
    PrProcessState info{};
    for (;;) {
        drain(process, PrProcessStream_Output, result.out);
        drain(process, PrProcessStream_Error, result.err);
        info = prProcessState(process);
        if (info.exited && info.outputEnded)
            break;
        if (std::chrono::steady_clock::now() > deadline) {
            prProcessDestroy(process);
            prProcessLauncherDestroy(launcher);
            result.end = CommandResult::End::TimedOut;
            return result;
        }
        waker.wait();
    }
    drain(process, PrProcessStream_Error, result.err);
    prProcessDestroy(process);
    prProcessLauncherDestroy(launcher);
    result.end = info.signaled ? CommandResult::End::Signaled : CommandResult::End::Exited;
    result.code = info.code;
    return result;
}

} // namespace nexus
