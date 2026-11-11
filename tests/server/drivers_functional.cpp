// Functional tests of nexus-server's friends layer (src/server/drivers.h): real driver processes (the fake driver,
// tests/driver) over real pipes, behind the hub, on a real store in a directory of the test's own, with short deadlines
// and the real clock. Each case starts from an empty directory, and checks that every process it started is gone at
// its end: no child of this process is left, collected or not.
//
//   drivers_functional <nexus-fake-driver> <dir> <case>
//
// The cases: normal, stop, silent, exit_mid_turn, malformed, ignores_stop, kill (ignores EOF and SIGTERM, so only
// SIGKILL ends it), flood, fails_first_start, resume, restart (the server killed mid-turn and started again),
// orphan (what a driver started is ended with it), escaped_output (a driver whose output another process holds open),
// queue (the open entry while the friend writes, and a queued prompt taken at the turn's end).

#include <libproc.h>
#include <poll.h>
#include <signal.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "server/drivers.h"
#include "server/durable.h"
#include "store/fs.h"

namespace {

using namespace nexus;
using namespace nexus::server;

int failures = 0;

#define CHECK(cond)                                                                       \
    do {                                                                                  \
        if (!(cond)) {                                                                    \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                                   \
        }                                                                                 \
    } while (0)

// Short deadlines, so that every timeout is reached within the case: hello, stop, the grace after closing a driver's
// input, the grace after SIGTERM.
const Timing kTiming{1000, 300, 200, 200};

const char* const kHello = "emit {\"kind\":\"hello\",\"protocol\":1,\"harness\":\"fake\"}\n"
                           "emit {\"kind\":\"credential\",\"billing\":\"subscription\",\"plan\":\"max\"}\n";

Millis now() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

bool has(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

void writeFile(const std::string& path, const std::string& text) {
    std::ofstream out(path);
    out << text;
    CHECK(out.flush());
}

std::string readFile(const std::string& path) {
    std::ifstream in(path);
    std::ostringstream all;
    all << in.rdbuf();
    return all.str();
}

// The children of this process not yet collected, and those still running.
int children() {
    pid_t pids[256] = {};
    proc_listchildpids(getpid(), pids, sizeof pids);
    return static_cast<int>(std::count_if(std::begin(pids), std::end(pids), [](pid_t p) { return p > 0; }));
}

// nexus-server's friends over a store in `dir`: the store, the hub and the drivers, as the server makes them.
struct Server {
    std::unique_ptr<store::Fs> fs = store::make_posix_fs();
    DurableStore store;
    std::unique_ptr<Drivers> drivers;
    std::unique_ptr<sync::Hub> hub;
    std::set<pid_t> pids;               // every driver seen running
    std::uint64_t num = 1;

    explicit Server(const std::string& dir) {
        std::string why;
        if (!store.open(*fs, dir + "/store", &why)) {
            std::fprintf(stderr, "drivers_functional: %s\n", why.c_str());
            std::exit(1);
        }
        drivers = std::make_unique<Drivers>(store, Drivers::Options{dir + "/drivers", dir, kTiming, now});
        hub = std::make_unique<sync::Hub>(store, sync::Limits{}, drivers.get());
        drivers->attach(*hub);
        CHECK(drivers->start(now()));
        see();
    }

    void see() {
        const pid_t p = drivers->pid("ada");
        if (p > 0) {
            pids.insert(p);
        }
    }

    void send(const std::string& text) {
        CHECK(hub->apply(sync::Action{num++, sync::ActionKind::Send, "ada", text}));
        see();
    }

    // The loop, as the server runs it, until `done` or `seconds` pass.
    bool serveUntil(const std::function<bool()>& done, double seconds) {
        const Millis end = now() + static_cast<Millis>(seconds * 1000);
        while (!done() && now() < end) {
            std::vector<pollfd> fds = drivers->fds();
            const int wait = std::min<int>(drivers->waitMs(now()), 20);
            ::poll(fds.data(), static_cast<nfds_t>(fds.size()), wait);
            bool changed = false;
            CHECK(drivers->serve(now(), &changed));
            see();
        }
        return done();
    }

    const Supervisor& sup() { return *drivers->supervisor("ada"); }

    // ada's whole transcript, an entry a line: "P " the person's, "F " the friend's, "N " a notice.
    std::vector<std::string> transcript() const {
        std::vector<std::string> out;
        if (const sync::StoredTab* t = store.find("ada")) {
            for (const sync::EntryRef& e : t->transcript) {
                out.push_back(std::string(e->kind == EntryKind::Person ? "P " : e->kind == EntryKind::Friend ? "F " :
                    "N ") + e->text);
            }
        }
        return out;
    }
    sync::DriverState driver() const {
        const sync::StoredTab* t = store.find("ada");
        return t != nullptr ? t->driver : sync::DriverState{};
    }

    // Shutdown: every driver ended and collected, and none left running.
    void end() {
        CHECK(drivers->shutdown(now()));
        CHECK(serveUntil([&] { return !drivers->alive(); }, 5));
        for (pid_t p : pids) {
            CHECK(::kill(p, 0) != 0 && errno == ESRCH);
        }
    }
};

void dump(const Server& s) {
    for (const std::string& line : s.transcript()) {
        std::fprintf(stderr, "  | %s\n", line.c_str());
    }
}

// ada's drivers file and script.
void setUp(const std::string& dir, const std::string& fake, const std::string& script) {
    writeFile(dir + "/ada.script", script);
    writeFile(dir + "/drivers", "driver fake " + fake + " --script " + dir + "/ada.script\nfriend ada fake " + dir +
        "\n");
}

bool idle(Server& s) {
    return s.sup().phase() == Phase::Idle && s.driver().openTurn == 0;
}

bool none(Server& s) {
    return s.sup().phase() == Phase::None;
}

// A turn answered: the prompt, the text gathered into one entry, the tool line; the session id and the next turn kept.
void normal(Server& s, const std::string& dir, const std::string& fake) {
    setUp(dir, fake, std::string(kHello) + "send\nemit {\"kind\":\"session\",\"id\":\"s-1\"}\n"
        "emit {\"kind\":\"text\",\"delta\":\"Hel\"}\nemit {\"kind\":\"text\",\"delta\":\"lo.\"}\n"
        "emit {\"kind\":\"text-end\"}\nemit {\"kind\":\"tool\",\"name\":\"Read\",\"summary\":\"a file\"}\n"
        "emit {\"kind\":\"done\",\"outcome\":\"ok\",\"session_cost_usd\":0.02}\n");
    s.send("hi");
    CHECK(s.serveUntil([&] { return s.transcript().size() == 3 && idle(s); }, 10));
    CHECK((s.transcript() == std::vector<std::string>{"P hi", "F Hello.", "F Read: a file"}));
    CHECK(s.driver().session == "s-1");
    CHECK(s.driver().nextTurn == 2);
    CHECK(s.sup().alive());
}

// Stopped mid-turn: the text so far, then the stopped notice; the driver stays up for the next prompt.
void stop(Server& s, const std::string& dir, const std::string& fake) {
    setUp(dir, fake, std::string(kHello) + "send\nemit {\"kind\":\"text\",\"delta\":\"Work\"}\nawait-stop\n"
        "emit {\"kind\":\"text\",\"delta\":\"ing\"}\nemit {\"kind\":\"text-end\"}\n"
        "emit {\"kind\":\"done\",\"outcome\":\"stopped\"}\nsend\nemit {\"kind\":\"done\",\"outcome\":\"ok\"}\n");
    s.send("go");
    CHECK(s.serveUntil([&] { return s.sup().gathering(); }, 10));
    CHECK(s.hub->apply(sync::Action{s.num++, sync::ActionKind::Stop, "ada", ""}));
    CHECK(s.serveUntil([&] { return idle(s); }, 10));
    CHECK((s.transcript() == std::vector<std::string>{"P go", "F Working", "N Stopped."}));
    const pid_t first = s.drivers->pid("ada");
    s.send("again");
    CHECK(s.serveUntil([&] { return s.driver().nextTurn == 3 && idle(s); }, 10));
    CHECK(s.drivers->pid("ada") == first);
}

// The friend's text growing shows as the tab's open entry, in memory only; a prompt queued meanwhile waits for the
// turn's end (here a stop), then goes to the driver as the next turn; one queued with the friend idle goes at once.
void queue(Server& s, const std::string& dir, const std::string& fake) {
    setUp(dir, fake, std::string(kHello) + "send\nemit {\"kind\":\"text\",\"delta\":\"Work\"}\nawait-stop\n"
        "emit {\"kind\":\"text-end\"}\nemit {\"kind\":\"done\",\"outcome\":\"stopped\"}\n"
        "send\nemit {\"kind\":\"text\",\"delta\":\"Second\"}\nemit {\"kind\":\"text-end\"}\n"
        "emit {\"kind\":\"done\",\"outcome\":\"ok\"}\nsend\nemit {\"kind\":\"done\",\"outcome\":\"ok\"}\n");
    s.send("go");
    CHECK(s.serveUntil([&] { return s.sup().gathering(); }, 10));
    const sync::TabView* v = s.hub->state().find("ada");
    CHECK(v != nullptr && !v->entries.empty() && !v->entries.back()->closed && v->entries.back()->text == "Work");
    CHECK((s.transcript() == std::vector<std::string>{"P go"}));
    CHECK(s.hub->apply(sync::Action{s.num++, sync::ActionKind::Queue, "ada", "next"}));
    CHECK(s.store.find("ada")->queue == std::vector<std::string>{"next"});
    CHECK((s.transcript() == std::vector<std::string>{"P go"}));
    CHECK(s.hub->apply(sync::Action{s.num++, sync::ActionKind::Stop, "ada", ""}));
    CHECK(s.serveUntil([&] { return s.driver().nextTurn == 3 && idle(s); }, 10));
    CHECK((s.transcript() == std::vector<std::string>{"P go", "F Work", "N Stopped.", "P next", "F Second"}));
    CHECK(s.store.find("ada")->queue.empty());
    v = s.hub->state().find("ada");
    CHECK(v != nullptr && v->entries.back()->closed);
    CHECK(s.hub->apply(sync::Action{s.num++, sync::ActionKind::Queue, "ada", "idle"}));
    CHECK(s.serveUntil([&] { return s.driver().nextTurn == 4 && idle(s); }, 10));
    CHECK(s.transcript().back() == "P idle");
    CHECK(s.store.find("ada")->queue.empty());
}

// No hello: ended after the hello deadline, counted as a failed start, and started again for the prompt still waiting.
void silent(Server& s, const std::string& dir, const std::string& fake) {
    setUp(dir, fake, "hang\n");
    const Millis sent = now();
    s.send("anyone?");
    CHECK(s.serveUntil([&] { return s.sup().failedStarts() >= 1; }, 10));
    CHECK(now() - sent >= kTiming.hello);
    const std::vector<std::string> t = s.transcript();
    CHECK(t.size() >= 2 && has(t[1], "did not say hello within 1 s"));
    CHECK(s.driver().waiting == std::vector<std::string>{"anyone?"});
    CHECK(s.driver().openTurn == 0);
}

// An exit with a turn open: the text so far, why, and the turn cut; the prompt is never written again.
void exitMidTurn(Server& s, const std::string& dir, const std::string& fake) {
    setUp(dir, fake, std::string(kHello) + "send\nemit {\"kind\":\"text\",\"delta\":\"par\"}\nexit 5\n");
    s.send("q");
    CHECK(s.serveUntil([&] { return s.transcript().size() == 4 && none(s); }, 10));
    const std::vector<std::string> t = s.transcript();
    CHECK(t.size() == 4);
    if (t.size() == 4) {
        CHECK(t[1] == "F par");
        CHECK(has(t[2], "ada's driver exited with status 5."));
        CHECK(has(t[3], "cut short. Whether ada got the prompt is unknown"));
    }
    CHECK(s.driver().openTurn == 0 && s.driver().waiting.empty() && s.driver().nextTurn == 2);
}

// A line that is not a message: the driver is ended and the turn cut.
void malformed(Server& s, const std::string& dir, const std::string& fake) {
    setUp(dir, fake, std::string(kHello) + "send\nraw {not json\nhang\n");
    s.send("q");
    CHECK(s.serveUntil([&] { return none(s) && s.driver().openTurn == 0; }, 10));
    const std::vector<std::string> t = s.transcript();
    CHECK(t.size() == 3);
    if (t.size() == 3) {
        CHECK(has(t[1], "wrote a line the server refused"));
        CHECK(has(t[2], "cut short"));
    }
}

// A stop the driver ignores: ended after the stop deadline, the turn cut.
void ignoresStop(Server& s, const std::string& dir, const std::string& fake) {
    setUp(dir, fake, std::string(kHello) + "send\nemit {\"kind\":\"text\",\"delta\":\"a\"}\nhang\n");
    s.send("q");
    CHECK(s.serveUntil([&] { return s.sup().gathering(); }, 10));
    const Millis stopped = now();
    CHECK(s.hub->apply(sync::Action{s.num++, sync::ActionKind::Stop, "ada", ""}));
    CHECK(s.serveUntil([&] { return none(s); }, 10));
    CHECK(now() - stopped >= kTiming.stop);
    const std::vector<std::string> t = s.transcript();
    CHECK(t.size() == 4);
    if (t.size() == 4) {
        CHECK(t[1] == "F a");
        CHECK(has(t[2], "did not end the stopped turn within"));
        CHECK(has(t[3], "cut short"));
    }
}

// A driver that ignores the end of its input and SIGTERM: only SIGKILL ends it, after both graces.
void kill(Server& s, const std::string& dir, const std::string& fake) {
    setUp(dir, fake, std::string(kHello) + "option ignore-eof\noption ignore-sigterm\nsend\nhang\n");
    s.send("q");
    CHECK(s.serveUntil([&] { return s.sup().phase() == Phase::Working; }, 10));
    const Millis stopped = now();
    CHECK(s.hub->apply(sync::Action{s.num++, sync::ActionKind::Stop, "ada", ""}));
    bool killed = false;
    CHECK(s.serveUntil([&] {
        killed = killed || s.sup().stage() == Stage::Killed;
        return none(s);
    }, 10));
    CHECK(killed);
    CHECK(now() - stopped >= kTiming.stop + kTiming.endGrace + kTiming.termGrace);
}

// A driver writing as fast as it can: all of its text arrives, in entries within the hub's limit, while the loop keeps
// its turns short.
void flood(Server& s, const std::string& dir, const std::string& fake) {
    setUp(dir, fake, std::string(kHello) + "send\nflood 100000 {\"kind\":\"text\",\"delta\":\"x\"}\n"
        "emit {\"kind\":\"text-end\"}\nemit {\"kind\":\"done\",\"outcome\":\"ok\"}\n");
    s.send("q");
    Millis longest = 0;
    Millis last = now();
    CHECK(s.serveUntil([&] {
        longest = std::max(longest, now() - last);
        last = now();
        return idle(s);
    }, 30));
    std::size_t total = 0;
    const std::vector<std::string> t = s.transcript();
    for (std::size_t i = 1; i < t.size(); ++i) {
        CHECK(t[i].rfind("F ", 0) == 0);
        CHECK(t[i].size() - 2 <= s.hub->limits().text);
        total += t[i].size() - 2;
    }
    CHECK(total == 100000);
    std::printf("flood: %zu entries; the longest round of the loop took %lld ms\n", t.size() - 1,
        static_cast<long long>(longest));
}

// A driver that fails its first start: the failure is told and counted, the next start answers.
void failsFirstStart(Server& s, const std::string& dir, const std::string& fake) {
    setUp(dir, fake, "count-starts " + dir + "/starts\non-start 1 exit 1\n" + kHello +
        "send\nemit {\"kind\":\"text\",\"delta\":\"ok\"}\nemit {\"kind\":\"text-end\"}\n"
        "emit {\"kind\":\"done\",\"outcome\":\"ok\"}\n");
    s.send("q");
    CHECK(s.serveUntil([&] { return idle(s) && s.transcript().size() == 3; }, 10));
    const std::vector<std::string> t = s.transcript();
    CHECK(t.size() == 3);
    if (t.size() == 3) {
        CHECK(has(t[1], "ada's driver exited with status 1."));
        CHECK(t[2] == "F ok");
    }
    CHECK(s.sup().failedStarts() == 0);     // reset by the answered turn
    CHECK(s.pids.size() == 2);
}

// The session id a driver reports is the next start's --resume, and the directory is its --cwd.
void resume(Server& s, const std::string& dir, const std::string& fake) {
    setUp(dir, fake, "record " + dir + "/starts.log\n" + kHello + "send\nemit {\"kind\":\"session\",\"id\":\"s-9\"}\n"
        "emit {\"kind\":\"done\",\"outcome\":\"ok\"}\nexit 0\n");
    s.send("one");
    CHECK(s.serveUntil([&] { return none(s) && s.driver().session == "s-9"; }, 10));
    s.send("two");
    CHECK(s.serveUntil([&] { return none(s) && s.driver().nextTurn == 3; }, 10));
    const std::string log = readFile(dir + "/starts.log");
    const std::size_t second = log.find("resume=s-9");
    CHECK(has(log, "resume=\n") || has(log, "resume= "));
    CHECK(second != std::string::npos);
    CHECK(has(log, "cwd=" + dir));
}

// ada's driver as a shell script, for what the fake driver does not do: start processes of its own.
void setUpShell(const std::string& dir, const std::string& script) {
    writeFile(dir + "/ada.sh", script);
    writeFile(dir + "/drivers", "driver sh /bin/sh " + dir + "/ada.sh\nfriend ada sh " + dir + "\n");
}

pid_t readPid(const std::string& path) {
    const std::string text = readFile(path);
    return text.empty() ? -1 : static_cast<pid_t>(std::stol(text));
}

// Whether process `pid` is gone within `seconds`.
bool goneWithin(pid_t pid, double seconds) {
    const Millis end = now() + static_cast<Millis>(seconds * 1000);
    while (::kill(pid, 0) == 0 && now() < end) {
        ::usleep(10'000);
    }
    return ::kill(pid, 0) != 0 && errno == ESRCH;
}

const char* const kShellHello = "printf '%s\\n' '{\"kind\":\"hello\",\"protocol\":1,\"harness\":\"sh\"}'\n";

// A driver that exits on its own: what it started in its process group (its harness) is ended with it, so that the
// next driver for the tab never works beside it.
void orphan(Server& s, const std::string& dir, const std::string&) {
    setUpShell(dir, "sleep 30 </dev/null >/dev/null 2>&1 &\necho $! > " + dir + "/child.pid\n" + kShellHello +
        "exit 3\n");
    s.send("q");
    CHECK(s.serveUntil([&] { return none(s) && s.sup().failedStarts() == 1; }, 10));
    const pid_t child = readPid(dir + "/child.pid");
    CHECK(child > 0);
    if (child > 0) {
        CHECK(goneWithin(child, 2));
        ::kill(child, SIGKILL);
    }
}

// A driver ended at shutdown that ignores the end of its input and SIGTERM, while a process it started outside its
// process group holds its output open: the loop as nexus-server ends its drivers (polling as long as waitMs() says)
// collects it soon after SIGKILL, not when that process lets go.
void escapedOutput(Server& s, const std::string& dir, const std::string&) {
    setUpShell(dir, "trap '' TERM\nperl -e 'setpgrp(0, 0); sleep 5' </dev/null &\necho $! > " + dir +
        "/holder.pid\n" + kShellHello + "while :; do sleep 1; done\n");
    s.send("q");
    CHECK(s.serveUntil([&] { return s.sup().phase() == Phase::Working; }, 10));
    const Millis start = now();
    CHECK(s.drivers->shutdown(start));
    while (s.drivers->alive() && now() - start < 10'000) {
        std::vector<pollfd> fds = s.drivers->fds();
        ::poll(fds.data(), static_cast<nfds_t>(fds.size()), s.drivers->waitMs(now()));
        bool changed = false;
        CHECK(s.drivers->serve(now(), &changed));
    }
    const Millis took = now() - start;
    CHECK(!s.drivers->alive());
    CHECK(took < kTiming.endGrace + kTiming.termGrace + 1500);
    std::printf("escaped_output: the driver was collected %lld ms after the shutdown\n", static_cast<long long>(took));
    const pid_t holder = readPid(dir + "/holder.pid");
    if (holder > 0) {
        ::kill(holder, SIGKILL);
    }
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 4) {
        std::fprintf(stderr, "usage: drivers_functional <nexus-fake-driver> <dir> <case>\n");
        return 2;
    }
    const std::string fake = argv[1];
    const std::string dir = argv[2];
    const std::string name = argv[3];
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    ::signal(SIGPIPE, SIG_IGN);     // as the server does: a driver gone mid-write is an error from write
    const int before = children();
    CHECK(before == 0);
    {
        if (name == "restart") {
            // The server dies mid-turn (its drivers killed, nothing ended in order) and starts again on the store.
            {
                Server s(dir);
                setUp(dir, fake, "count-starts " + dir + "/starts\n" + kHello + "send\non-start 1 emit {\"kind\":\"text\",\"delta\":\"a\"}\n"
                    "on-start 1 hang\nemit {\"kind\":\"done\",\"outcome\":\"ok\"}\n");
                s.send("first");
                CHECK(s.serveUntil([&] { return s.sup().gathering(); }, 10));
                CHECK(s.driver().openTurn == 1);
            }
            Server s(dir);
            CHECK(s.driver().openTurn == 0);
            const std::vector<std::string> t = s.transcript();
            CHECK(t.size() == 2 && has(t.back(), "nexus-server restarted while ada was answering"));
            s.send("second");
            CHECK(s.serveUntil([&] { return idle(s) && s.driver().nextTurn == 3; }, 10));
            CHECK(s.driver().nextTurn == 3);    // the second prompt was turn 2: the first's number is never reused
            s.end();
            if (failures != 0) {
                dump(s);
            }
        } else {
            Server s(dir);
            using Case = void (*)(Server&, const std::string&, const std::string&);
            const std::vector<std::pair<std::string, Case>> cases = {{"normal", normal}, {"stop", stop},
                {"silent", silent}, {"exit_mid_turn", exitMidTurn}, {"malformed", malformed},
                {"ignores_stop", ignoresStop}, {"kill", kill}, {"flood", flood},
                {"fails_first_start", failsFirstStart}, {"resume", resume}, {"orphan", orphan},
                {"escaped_output", escapedOutput}, {"queue", queue}};
            auto it = std::find_if(cases.begin(), cases.end(), [&](const auto& c) { return c.first == name; });
            if (it == cases.end()) {
                std::fprintf(stderr, "drivers_functional: no case %s\n", name.c_str());
                return 2;
            }
            it->second(s, dir, fake);
            s.end();
            if (failures != 0) {
                dump(s);
            }
        }
    }
    CHECK(children() == before);
    std::filesystem::remove_all(dir);
    if (failures != 0) {
        std::fprintf(stderr, "drivers_functional %s: %d check(s) failed\n", name.c_str(), failures);
        return 1;
    }
    std::printf("drivers_functional %s: ok\n", name.c_str());
    return 0;
}
