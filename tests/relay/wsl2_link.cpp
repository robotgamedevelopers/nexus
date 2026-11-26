// The native window's link across the WSL2 boundary, for real (functional, Windows only): the window's port
// (src/relay/relay_port.h) starts nexus-link-relay inside WSL2 through wsl.exe, the relay bootstraps a nexus-server
// there with a home of its own (--local, no ssh) and keeps mosh's session to it, and this program, as the window does:
// waits for the link to connect, ensures the AI's session, sends a prompt, and reads back from the server's state
// the prompt once and the AI's reply (the fake driver's); then queues six prompts at once and finds each once in
// the server's state, in its queue in the order sent or taken from it into the transcript (all within the 16 entries
// of a transcript's tail the state carries). Then the relay ends, killed as a crash ends it, just after a prompt was
// handed over: the window shows the link failed, starts the next relay after the backoff (relay/relay_life.h) and
// connects again; the prompt is either cut (shown, and in the server's state at most once) or in the server's state
// exactly once, and the next prompt, numbered after it, arrives once. Then it ends the port, as the window does at quit.
//
//   nexus_wsl2_link_tests [--state-out FILE] --relay-pid FILE <relay> -- <COMMAND...>
//
// With --state-out, the sessions it read across the boundary are written to FILE as a state file (sync/sessions.h
// stateFile), which `nexus --screenshot PNG --state FILE` draws: the window's picture of what crossed.
//
// --relay-pid names the file in WSL2 into which <relay> writes its process id (wsl2_link.sh's wrapper), so that only
// the relay this test started is killed. <relay> is that wrapper's path in WSL2, which execs nexus-link-relay, or `-`:
// the window's own default (relay/relay_words.h, `sh -c` finding nexus-link-relay on the PATH), and
// COMMAND what the relay runs in place of `nexus-server new` (the test's server: `env HOME=<dir>/home <nexus-server>
// new --dir <dir>`); the AI is ada, on the fake driver, set up in <dir>/drivers by wsl2_link.sh. Real time: each wait
// is bounded, and a wait that runs out fails, saying what the link last showed.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "model.h"
#include "relay/relay_port.h"

namespace {

using namespace nexus;
using Clock = std::chrono::steady_clock;

int failures = 0;

#define CHECK(cond)                                                                       \
    do {                                                                                  \
        if (!(cond)) {                                                                    \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                                   \
        }                                                                                 \
    } while (0)

const char* phaseName(sync::LinkPhase p) {
    switch (p) {
    case sync::LinkPhase::Connecting: return "connecting";
    case sync::LinkPhase::Connected: return "connected";
    case sync::LinkPhase::Failed: return "failed";
    }
    return "?";
}

double since(Clock::time_point t) {
    return std::chrono::duration<double>(Clock::now() - t).count();
}

const sync::SessionView* adas(const sync::Sessions& s) {
    for (const auto& v : s.sessions())
        if (v->info.aiName == "ada") return v.get();
    return nullptr;
}

std::size_t count(const sync::SessionView& v, EntryKind kind, const std::string& text) {
    std::size_t n = 0;
    for (const auto& e : v.entries)
        if (e->kind == kind && e->text == text) ++n;
    return n;
}

} // namespace

int main(int argc, char** argv) {
    const char* stateOut = nullptr;
    const char* pidFile = nullptr;
    int at = 1;
    while (argc > at + 1 && (std::strcmp(argv[at], "--state-out") == 0 || std::strcmp(argv[at], "--relay-pid") == 0)) {
        (std::strcmp(argv[at], "--state-out") == 0 ? stateOut : pidFile) = argv[at + 1];
        at += 2;
    }
    if (pidFile == nullptr || argc < at + 3 || std::strcmp(argv[at + 1], "--") != 0) {
        std::fprintf(stderr, "usage: nexus_wsl2_link_tests [--state-out FILE] --relay-pid FILE <relay> -- <COMMAND...>\n");
        return 2;
    }
    relay::RelayPort::Options options;
    options.host = "localhost";
    // "-": the program the window starts by default.
    options.relay = {std::strcmp(argv[at], "-") == 0 ? std::string() : std::string(argv[at]), "--"};
    for (int i = at + 2; i < argc; ++i) options.relay.push_back(argv[i]);

    LinkState s;
    std::vector<std::uint64_t> cut;     // every action the port said it cut
    const auto start = Clock::now();
    auto port = std::make_unique<relay::RelayPort>(options);
    auto poll = [&] {
        if (port->poll(&s)) cut.insert(cut.end(), s.cut.begin(), s.cut.end());
    };
    // Polls until `done`, or `seconds` pass; the state stays the latest polled.
    auto until = [&](const std::function<bool()>& done, int seconds, const char* what) {
        const auto deadline = Clock::now() + std::chrono::seconds(seconds);
        while (Clock::now() < deadline) {
            poll();
            if (done()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        poll();
        if (done()) return true;
        std::fprintf(stderr, "wsl2_link: no %s within %d s: the link is %s%s%s\n", what, seconds, phaseName(s.phase),
            s.problem.empty() ? "" : ": ", s.problem.c_str());
        return false;
    };

    // The relay's start through wsl.exe, its bootstrap of the server, and mosh's first state.
    if (!until([&] { return s.phase == sync::LinkPhase::Connected; }, 60, "connection")) return 1;
    std::printf("wsl2_link: connected through nexus-link-relay in WSL2 in %.2f s (loopback %s)\n", since(start),
        s.loopback ? "yes" : "no");
    CHECK(s.loopback);

    sync::Action ensure;
    ensure.kind = sync::ActionKind::Ensure;
    ensure.aiName = "ada";
    CHECK(port->send(ensure) == 1);
    if (!until([&] { return adas(s.sessions) != nullptr; }, 30, "session with ada")) return 1;
    const sync::SessionId ada = adas(s.sessions)->id;

    const std::string prompt = "hello across the WSL2 boundary";
    sync::Action send;
    send.kind = sync::ActionKind::Send;
    send.session = ada;
    send.text = prompt;
    const auto sent = Clock::now();
    CHECK(port->send(send) == 2);
    if (!until([&] {
            const auto* v = adas(s.sessions);
            return v != nullptr && count(*v, EntryKind::Person, prompt) >= 1 &&
                count(*v, EntryKind::AI, "hello back") >= 1;
        }, 30, "prompt and reply in the server's state")) return 1;
    std::printf("wsl2_link: the prompt and ada's reply in the server's state in %.3f s\n", since(sent));
    CHECK(count(*adas(s.sessions), EntryKind::Person, prompt) == 1);

    // Six queued at once: each in the server's state once.
    std::vector<std::string> want;
    for (int i = 1; i <= 6; ++i) {
        sync::Action queue;
        queue.kind = sync::ActionKind::Queue;
        queue.session = ada;
        queue.text = "queued " + std::to_string(i);
        want.push_back(queue.text);
        CHECK(port->send(queue) == static_cast<std::uint64_t>(2 + i));
    }
    // Each is in the queue, or, once the AI took it from the queue, in the transcript as the person's: once.
    auto seen = [&](const sync::SessionView& v, const std::string& text) {
        return count(v, EntryKind::Person, text) + static_cast<std::size_t>(
            std::count(v.queue.begin(), v.queue.end(), text));
    };
    if (!until([&] {
            const auto* v = adas(s.sessions);
            if (v == nullptr) return false;
            for (const std::string& w : want)
                if (seen(*v, w) == 0) return false;
            return true;
        }, 30, "six queued prompts in the server's state")) return 1;
    for (const std::string& w : want) CHECK(seen(*adas(s.sessions), w) == 1);
    // In the order sent: those still queued are the last ones, in order.
    const auto& queued = adas(s.sessions)->queue;
    CHECK(std::equal(queued.begin(), queued.end(), want.end() - static_cast<std::ptrdiff_t>(queued.size())));
    if (stateOut != nullptr) {
        std::ofstream out(stateOut, std::ios::binary);
        out << sync::stateFile(s.sessions);
        CHECK(out.flush());
        std::printf("wsl2_link: the state read across the boundary is in %s\n", stateOut);
    }

    // The relay ends, killed as a crash would end it, with a prompt handed over just before: the window shows the link
    // failed, starts the next relay after the backoff, and connects again. The prompt is applied once or not at all:
    // cut (it may have arrived), or else in the server's state exactly once; the window's numbers go on.
    const std::string raced = "raced with the relay's end";
    sync::Action race;
    race.kind = sync::ActionKind::Send;
    race.session = ada;
    race.text = raced;
    CHECK(port->send(race) == 9);
    const std::string kill = relay::windowsCommandLine({"wsl.exe", "-e", "/bin/sh", "-c", "kill -9 \"$(cat \"$1\")\"",
        "kill", pidFile});
    const auto killed = Clock::now();
    CHECK(std::system(kill.c_str()) == 0);
    if (!until([&] { return s.phase == sync::LinkPhase::Failed; }, 30, "failed link after the relay's end")) return 1;
    std::printf("wsl2_link: the relay's end shown in %.2f s: %s\n", since(killed), s.problem.c_str());
    if (!until([&] { return s.phase == sync::LinkPhase::Connected; }, 60, "connection through the next relay")) return 1;
    std::printf("wsl2_link: connected through the next relay %.2f s after the end\n", since(killed));
    const bool racedCut = std::count(cut.begin(), cut.end(), std::uint64_t(9)) == 1;
    if (!racedCut && !until([&] {
            const auto* v = adas(s.sessions);
            return v != nullptr && count(*v, EntryKind::Person, raced) >= 1;
        }, 30, "prompt raced with the relay's end, not cut, in the server's state")) return 1;
    CHECK(count(*adas(s.sessions), EntryKind::Person, raced) <= 1);
    std::printf("wsl2_link: the prompt raced with the relay's end: %s, %zu time(s) in the server's state\n",
        racedCut ? "cut, shown" : "not cut", count(*adas(s.sessions), EntryKind::Person, raced));
    for (const std::uint64_t n : cut) CHECK(n == 9);
    const std::string after = "after the relay's end";
    sync::Action next = race;
    next.text = after;
    CHECK(port->send(next) == 10);
    if (!until([&] {
            const auto* v = adas(s.sessions);
            return v != nullptr && count(*v, EntryKind::Person, after) >= 1;
        }, 30, "prompt sent after the relay's end in the server's state")) return 1;
    CHECK(count(*adas(s.sessions), EntryKind::Person, after) == 1);

    const auto quit = Clock::now();
    port.reset();   // the relay's input closes: mosh's shutdown, then it exits
    std::printf("wsl2_link: the port ended in %.2f s\n", since(quit));
    if (failures != 0) {
        std::fprintf(stderr, "wsl2_link: %d check(s) failed\n", failures);
        return 1;
    }
    std::printf("wsl2_link: all passed\n");
    return 0;
}
