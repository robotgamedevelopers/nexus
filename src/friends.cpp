#include "friends.h"

#include <chrono>
#include <cstdlib>
#include <condition_variable>
#include <mutex>

#include "pr/json.h"
#include "pr/process.h"

namespace nexus {

namespace {

constexpr const char* kCommand = "nova-config friend list --json";

std::string_view view(PrStringView s) {
    return s.data != nullptr ? std::string_view(s.data, s.size) : std::string_view();
}

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

FriendList parseFriendList(std::string_view json) {
    FriendList list;
    PrJson* doc = prJsonCreate();
    size_t at = 0;
    if (!prJsonParse(doc, pr(json), &at)) {
        list.error = std::string(kCommand) + " printed something that is not JSON (stopped at byte " +
            std::to_string(at) + "); run it in a terminal to see what it prints";
        prJsonDestroy(doc);
        return list;
    }
    PrJsonValueIndex status = prJsonMember(doc, prJsonMember(doc, 0, pr("result")), pr("status"));
    if (prJsonKind(doc, status) == PrJsonKind_String && view(prJsonString(doc, status)) != "ok") {
        list.error = std::string(kCommand) + " answered status " + std::string(view(prJsonString(doc, status))) +
            "; run it in a terminal to see why";
        prJsonDestroy(doc);
        return list;
    }
    PrJsonValueIndex items = prJsonMember(doc, 0, pr("items"));
    if (prJsonKind(doc, items) != PrJsonKind_Array) {
        list.error = std::string(kCommand) + " printed no list of friends (no \"items\" array); run it in a terminal "
            "to see what it prints";
        prJsonDestroy(doc);
        return list;
    }
    for (PrJsonValueIndex i = prJsonFirst(doc, items); i != kPrJsonMissing; i = prJsonAfter(doc, items, i)) {
        PrJsonValueIndex fields = prJsonMember(doc, i, pr("fields"));
        PrJsonValueIndex name = prJsonMember(doc, fields, pr("name"));
        PrJsonValueIndex model = prJsonMember(doc, fields, pr("model"));
        PrJsonValueIndex enabled = prJsonMember(doc, fields, pr("enabled"));
        if (prJsonKind(doc, name) != PrJsonKind_String || prJsonKind(doc, enabled) != PrJsonKind_True)
            continue;
        std::string_view n = view(prJsonString(doc, name));
        if (n.empty())
            continue;
        FriendInfo f{std::string(n), {}};
        if (prJsonKind(doc, model) == PrJsonKind_String)
            f.model = std::string(view(prJsonString(doc, model)));
        list.friends.push_back(std::move(f));
    }
    prJsonDestroy(doc);
    if (list.friends.empty())
        list.error = std::string(kCommand) + " listed no enabled friends; add one with `nova-config friend add`, or "
            "enable one with `nova-config friend set <name> --enabled true`";
    return list;
}

Command novaConfigCommand(const char* override, const char* shell, const std::vector<std::string>& arguments) {
    std::string joined;
    for (const std::string& a : arguments)
        joined += (joined.empty() ? "" : " ") + a;
    if (override != nullptr && *override != 0)
        return Command{override, arguments, std::string(override) + " " + joined};
    std::string program = shell != nullptr && *shell != 0 ? shell : "/bin/sh";
    // exec: the shell becomes nova-config, so a timeout's kill reaches it.
    std::string line = "exec nova-config " + joined;
    return Command{program, {"-l", "-c", line}, program + " -l -c '" + line + "'"};
}

Command friendListCommand(const char* override, const char* shell) {
    return novaConfigCommand(override, shell, {"friend", "list", "--json"});
}

FriendList loadFriends(const Command& command, const CommandRunner& run) {
    CommandResult r = run(command, kFriendListTimeoutSeconds);
    const std::string cmd = "`" + command.shown + "`";
    const std::string fix = "set NEXUS_NOVA_CONFIG to the full path of nova-config, or install nova-tools so that "
        "nova-config is on your login shell's PATH";
    FriendList list;
    switch (r.end) {
        case CommandResult::End::NotFound:
            list.error = "cannot run " + cmd + ": " + command.program + " was not found; " + fix;
            return list;
        case CommandResult::End::NotStarted:
            list.error = "cannot run " + cmd + ": the process could not start; " + fix;
            return list;
        case CommandResult::End::TimedOut:
            list.error = cmd + " did not finish within " + std::to_string(kFriendListTimeoutSeconds) +
                " seconds and was stopped; run it in a terminal to see where it waits (the friend store may be "
                "unreachable)";
            return list;
        case CommandResult::End::Signaled:
            list.error = cmd + " was ended by signal " + std::to_string(r.code) + "; run it in a terminal to see why";
            return list;
        case CommandResult::End::Exited:
            break;
    }
    if (r.code != 0) {
        std::string why = trimmed(r.err);
        list.error = cmd + " exited with status " + std::to_string(r.code);
        if (r.code == 127)
            list.error += " (command not found)";
        list.error += why.empty() ? " and printed nothing on standard error" : ": " + why;
        list.error += r.code == 127 ? "; " + fix : "; run it in a terminal to see more";
        return list;
    }
    return parseFriendList(r.out);
}

CommandResult runCommand(const Command& command, int timeoutSeconds) {
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
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeoutSeconds);
    PrProcessInfo info{};
    for (;;) {
        drain(process, PrProcessStream_Output, result.out);
        drain(process, PrProcessStream_Error, result.err);
        info = prProcessInfo(process);
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

FriendList loadFriends() {
    return loadFriends(friendListCommand(std::getenv("NEXUS_NOVA_CONFIG"), std::getenv("SHELL")), runCommand);
}

} // namespace nexus
