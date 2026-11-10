#include "friends.h"

#include <chrono>
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
            std::to_string(at) + ")";
        prJsonDestroy(doc);
        return list;
    }
    PrJsonValueIndex status = prJsonMember(doc, prJsonMember(doc, 0, pr("result")), pr("status"));
    if (prJsonKind(doc, status) == PrJsonKind_String && view(prJsonString(doc, status)) != "ok") {
        list.error = std::string(kCommand) + " answered status " + std::string(view(prJsonString(doc, status)));
        prJsonDestroy(doc);
        return list;
    }
    PrJsonValueIndex items = prJsonMember(doc, 0, pr("items"));
    if (prJsonKind(doc, items) != PrJsonKind_Array) {
        list.error = std::string(kCommand) + " printed no list of friends (no \"items\" array)";
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
        list.error = std::string(kCommand) + " listed no enabled friends";
    return list;
}

FriendList loadFriends() {
    FriendList list;
    PrProcessLauncher* launcher = prProcessLauncherCreate(PrProcessBackend_Default);
    if (launcher == nullptr) {
        list.error = std::string("cannot run ") + kCommand + ": this build has no process launcher";
        return list;
    }
    Waker waker;
    const PrStringView arguments[] = {pr("friend"), pr("list"), pr("--json")};
    PrProcessDesc desc{};
    desc.program = pr("nova-config");
    desc.arguments = arguments;
    desc.argumentCount = 3;
    desc.streams = PrProcessStreams_Pipes;
    desc.errors = PrProcessErrors_Pipe;
    desc.wake = &Waker::wake;
    desc.wakeContext = &waker;
    PrProcess* process = nullptr;
    PrStatus launched = prProcessLaunch(launcher, &desc, &process);
    if (launched != PrStatus_Ok || process == nullptr) {
        list.error = std::string("cannot run ") + kCommand + (launched == PrStatus_NotFound ?
            ": nova-config is not on the PATH" : ": the process could not start");
        prProcessLauncherDestroy(launcher);
        return list;
    }
    std::string out;
    std::string err;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    PrProcessInfo info{};
    for (;;) {
        drain(process, PrProcessStream_Output, out);
        drain(process, PrProcessStream_Error, err);
        info = prProcessInfo(process);
        if (info.exited && info.outputEnded)
            break;
        if (std::chrono::steady_clock::now() > deadline) {
            prProcessDestroy(process);
            prProcessLauncherDestroy(launcher);
            list.error = std::string(kCommand) + " did not finish within 30 seconds";
            return list;
        }
        waker.wait();
    }
    drain(process, PrProcessStream_Error, err);
    prProcessDestroy(process);
    prProcessLauncherDestroy(launcher);
    if (info.signaled || info.code != 0) {
        std::string why = trimmed(err);
        if (why.empty())
            why = info.signaled ? "ended by signal " + std::to_string(info.code) :
                "exited with status " + std::to_string(info.code);
        list.error = why;
        return list;
    }
    return parseFriendList(out);
}

} // namespace nexus
