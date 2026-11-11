// TRACER (tracer/claude-driver), run by hand and never by ctest or CI: it calls an outside service. A headless window
// (the window's link thread, model and mirror, without drawing) reaches a nexus-server of its own over ssh and mosh,
// and sends to the friend tab "rowan", which that server serves with drivers/claude/driver.mjs:
//   (a) a one-word reply, (b) a tool use in the driver's empty working directory, (c) a stop after the first event of a
//   long turn and then a next prompt to the same driver, (d) the driver killed (our own child, by the pid it reported)
//   and the next send resuming its session.
// Each step waits for the turn to end ("done:", "error:" or "driver exited"). The transcript the window saw is printed.
//
//   claude_tracer <nexus-server> <dir> <host>

#include <signal.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include "friends.h"
#include "link/client.h"
#include "model.h"
#include "sync/mirror.h"

namespace {

using namespace nexus;
using Clock = std::chrono::steady_clock;

const std::vector<FriendInfo> kFriends = {{"rowan", "claude"}};

const char* kindName(EntryKind k) {
    return k == EntryKind::Person ? "person" : k == EntryKind::Friend ? "friend" : "notice";
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 4) {
        std::fprintf(stderr, "usage: claude_tracer <nexus-server> <dir> <host>\n");
        return 2;
    }
    const std::string binary = argv[1];
    const std::string dir = argv[2];
    const std::string host = argv[3];
    Model model(kFriends, true);
    sync::Mirror mirror;
    link::ClientLink::Options options;
    options.bootstrap.host = host;
    options.bootstrap.command = {binary, "new", "--dir", dir};
    link::ClientLink link(options);
    link::ClientLink::Snapshot s;
    auto pump = [&] {
        if (link.poll(&s))
            mirror.apply(model, s.tabs);
    };
    auto waitFor = [&](const std::function<bool()>& done, int seconds) {
        const auto deadline = Clock::now() + std::chrono::seconds(seconds);
        while (Clock::now() < deadline) {
            pump();
            if (done())
                return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        return false;
    };
    if (!waitFor([&] { return s.phase != sync::LinkPhase::Connecting; }, 30) ||
            s.phase != sync::LinkPhase::Connected) {
        std::fprintf(stderr, "claude_tracer: not connected: %s\n", s.problem.c_str());
        return 1;
    }
    std::size_t tab = 0;
    for (std::size_t i = 0; i < model.tab_count(); ++i) {
        if (model.tab(i).name == "rowan")
            tab = i;
    }
    auto entries = [&] { return model.tab(tab).transcript.size(); };
    // The first entry at or after `from` whose text starts with one of `prefixes` (any kind when kind is null).
    auto findEntry = [&](std::size_t from, std::vector<std::string> prefixes, const EntryKind* kind) -> long {
        const auto& t = model.tab(tab).transcript;
        for (std::size_t i = from; i < t.size(); ++i) {
            if (kind != nullptr && t[i].kind != *kind)
                continue;
            for (const std::string& p : prefixes) {
                if (t[i].text.rfind(p, 0) == 0)
                    return static_cast<long>(i);
            }
        }
        return -1;
    };
    const EntryKind notice = EntryKind::Notice;
    const EntryKind friendKind = EntryKind::Friend;
    auto endsTurn = [&](std::size_t from) { return findEntry(from, {"done:", "driver exited"}, &notice) >= 0; };
    auto since = [](Clock::time_point t0) { return std::chrono::duration<double>(Clock::now() - t0).count(); };
    // Sends and waits for the turn's end; prints the time to the first event (any entry after the prompt's own) and
    // to the first reply (a friend entry, a tool use or the first streamed delta).
    auto step = [&](const char* label, const std::string& prompt, int seconds) {
        const std::size_t from = entries();
        const auto t0 = Clock::now();
        link.send(sync::ActionKind::Send, "rowan", prompt);
        double firstEvent = -1;
        double firstReply = -1;
        const bool ended = waitFor([&] {
            if (firstEvent < 0 && entries() > from + 1)
                firstEvent = since(t0);
            if (firstReply < 0 && (findEntry(from + 1, {""}, &friendKind) >= 0 ||
                    findEntry(from + 1, {"tool:", "streaming:"}, &notice) >= 0))
                firstReply = since(t0);
            return endsTurn(from + 1);
        }, seconds);
        std::printf("step %s: %s in %.2f s; first event %.2f s, first reply %.2f s\n", label,
            ended ? "turn ended" : "TIMED OUT", since(t0), firstEvent, firstReply);
        return from;
    };
    auto driverPid = [&] {
        int pid = -1;
        for (const Entry& e : model.tab(tab).transcript) {
            if (e.kind == EntryKind::Notice && e.text.rfind("driver ready, pid ", 0) == 0)
                pid = std::atoi(e.text.c_str() + 18);
        }
        return pid;
    };

    step("a", "Reply with the single word pong.", 120);
    step("b", "List the files in the current directory.", 120);

    // (c) stop while the turn runs (after its first streamed delta, friend text or tool use), then a next prompt to
    // the same process.
    const int pidBefore = driverPid();
    const std::size_t from = entries();
    link.send(sync::ActionKind::Send, "rowan", "Count slowly from 1 to 200, one number per line.");
    const bool running = waitFor([&] {
        return findEntry(from + 1, {""}, &friendKind) >= 0 || findEntry(from + 1, {"tool:", "streaming:"}, &notice) >= 0;
    }, 120);
    const bool endedEarly = endsTurn(from + 1);
    std::printf("step c: first event of the long turn %s; the turn had %s ended when stop was sent\n",
        running ? "seen" : "NOT SEEN", endedEarly ? "ALREADY" : "not");
    const std::size_t stopAt = entries();
    const auto t0 = Clock::now();
    link.send(sync::ActionKind::Send, "rowan", "/stop");
    const bool stopped = waitFor([&] { return endsTurn(stopAt); }, 60);
    std::printf("step c: stop %s in %.2f s (window's clock)\n", stopped ? "ended the turn" : "TIMED OUT", since(t0));
    waitFor([&] { return findEntry(stopAt, {"stopped:"}, &notice) >= 0; }, 10);
    step("c2", "Reply with the single word pong.", 120);
    std::printf("step c: driver pid before %d, after %d (%s process)\n", pidBefore, driverPid(),
        pidBefore == driverPid() ? "same" : "a new");

    // (d) kill our own driver (the pid it reported), wait for the server to see it exit, then resume.
    const int pid = driverPid();
    if (pid > 0) {
        const std::size_t f = entries();
        kill(pid, SIGKILL);
        const bool exited = waitFor([&] { return findEntry(f, {"driver exited"}, &notice) >= 0; }, 30);
        std::printf("step d: killed driver %d, server %s\n", pid, exited ? "saw it exit" : "DID NOT see it exit");
    }
    step("d", "What was the first thing I asked you?", 120);

    waitFor([] { return false; }, 2);
    std::printf("--- the window's transcript of rowan ---\n");
    for (const Entry& e : model.tab(tab).transcript)
        std::printf("[%s] %s\n", kindName(e.kind), e.text.c_str());
    std::printf("----------------------------------------\n");
    return 0;
}
