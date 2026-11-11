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
    auto endsTurn = [&](std::size_t from) {
        const auto& t = model.tab(tab).transcript;
        for (std::size_t i = from; i < t.size(); ++i) {
            const std::string& x = t[i].text;
            if (t[i].kind == EntryKind::Notice &&
                    (x.rfind("done:", 0) == 0 || x.rfind("error:", 0) == 0 || x.rfind("driver exited", 0) == 0))
                return true;
        }
        return false;
    };
    auto step = [&](const char* label, const std::string& prompt, int seconds) {
        const std::size_t from = entries();
        const auto t0 = Clock::now();
        link.send(sync::ActionKind::Send, "rowan", prompt);
        const bool ended = waitFor([&] { return endsTurn(from + 1); }, seconds);
        std::printf("step %s: %s in %.1f s\n", label, ended ? "turn ended" : "TIMED OUT",
            std::chrono::duration<double>(Clock::now() - t0).count());
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

    // (c) stop after the first event of a long turn, then a next prompt to the same process.
    const int pidBefore = driverPid();
    const std::size_t from = entries();
    link.send(sync::ActionKind::Send, "rowan", "Count slowly from 1 to 200, one number per line.");
    waitFor([&] { return entries() > from + 1; }, 120);
    link.send(sync::ActionKind::Send, "rowan", "/stop");
    const bool stopped = waitFor([&] { return endsTurn(from + 1); }, 60);
    std::printf("step c: stop %s\n", stopped ? "ended the turn" : "TIMED OUT");
    step("c2", "Reply with the single word pong.", 120);
    std::printf("step c: driver pid before %d, after %d (%s process)\n", pidBefore, driverPid(),
        pidBefore == driverPid() ? "same" : "a new");

    // (d) kill our own driver (the pid it reported), then resume.
    const int pid = driverPid();
    if (pid > 0) {
        kill(pid, SIGKILL);
        const std::size_t f = entries();
        waitFor([&] { return endsTurn(f); }, 30);
    }
    step("d", "What was the first thing I asked you?", 120);

    waitFor([] { return false; }, 2);
    std::printf("--- the window's transcript of rowan ---\n");
    for (const Entry& e : model.tab(tab).transcript)
        std::printf("[%s] %s\n", kindName(e.kind), e.text.c_str());
    std::printf("----------------------------------------\n");
    return 0;
}
