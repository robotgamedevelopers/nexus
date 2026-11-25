// Functional tests of nexus-server's AIs layer (src/server/drivers.h): real driver processes (the fake driver,
// tests/driver) over real pipes, behind the hub, on a real store in a directory of the test's own, with short deadlines
// and the real clock. Each case starts from an empty directory, and checks that every process it started is gone at
// its end: no child of this process is left, collected or not.
//
//   drivers_functional <nexus-fake-driver> <dir> <case> [<state dir>]
//
// The cases: normal, stop, silent, exit_mid_turn, malformed, ignores_stop, kill (ignores EOF and SIGTERM, so only
// SIGKILL ends it), flood, fails_first_start, resume, restart (the server killed mid-turn and started again),
// orphan (what a driver started is ended with it), escaped_output (a driver whose output another process holds open),
// queue (the open entry while the AI writes, and a queued prompt taken at the turn's end), account (an account
// change restarts the session's driver), sign_in and no_credential (what a driver says of its account's login reaches
// the hub), usage_limit (a usage limit moves the session to its AI's next signed-in account), no_account (a session
// on no account starts no driver that needs one), ai_removed (a reload that takes the AI out mid-turn stops
// the turn and ends the driver, keeps the queue, starts nothing, and one that puts it back lets the queue go on),
// ai_back_limited (an AI taken out unlimited and put back limited starts its next driver limited),
// ai_removed_starting, ai_back_before_stop, ai_back_limited_queue, ai_back_moved and
// ai_back_account (taken out while the driver starts, put back before the stopped turn ends, put back limited with
// prompts queued, put back with its instance moved to another machine or directory, and put back with its account
// unlinked: each prompt goes once, under the new place, limit and account), no_registry
// and no_registry_listed (a restart holding no registry sends nothing and starts nothing, and the first registry applied
// says from itself alone whether the AI is in it),
// instance_dir, instance_elsewhere, instance_moved and
// instance_default (a session's instance: its directory, a machine other than this one, a move of its directory or
// machine while its driver runs, and a session naming none, which takes its AI's default), window (busy with a
// reply streaming after tool lines, idle, then down; with a state dir it writes each state there for `nexus --state`),
// held (a prompt sent while the drivers are held for a batch is written only once they are released), secret_ask (a
// driver's ask for a secret, refused with no window to take it, then pasted, stored and answered), secret_sign_in
// (an account with no login asked for by nexus itself, and the waiting prompt answered once it is pasted), and
// secret_leak_witness (the reversed witness of the leak scan: the value set in this process's environment must be found).

#if defined(__APPLE__)
#include <libproc.h>
#endif
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
#include <iomanip>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "leak_scan.h"
#include "server/drivers.h"
#include "server/durable.h"
#include "server/secret_files.h"
#include "server/secret_lines.h"
#include "store/fs.h"

// The process's environment, which withoutInherited() takes from a recorded one (POSIX declares it; not every header
// does).
extern char** environ;

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

const char* const kHello = "emit {\"kind\":\"hello\",\"protocol\":1,\"harness\":\"fake\",\"features\":[\"limit\"]}\n"
                           "emit {\"kind\":\"credential\",\"billing\":\"subscription\",\"plan\":\"max\"}\n";

Millis now() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

bool has(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

// Any of `lines` holds `part`.
bool has(const std::vector<std::string>& lines, const std::string& part) {
    return std::any_of(lines.begin(), lines.end(), [&](const std::string& l) { return has(l, part); });
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

// The children of this process not yet collected, and those still running: on macOS from libproc, on Linux from each
// process's parent in /proc (a child not yet collected is still there, a zombie).
#if defined(__APPLE__)
int children() {
    pid_t pids[256] = {};
    proc_listchildpids(getpid(), pids, sizeof pids);
    return static_cast<int>(std::count_if(std::begin(pids), std::end(pids), [](pid_t p) { return p > 0; }));
}
#else
int children() {
    const pid_t self = getpid();
    int count = 0;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator("/proc", ec)) {
        const std::string name = entry.path().filename().string();
        if (name.empty() || name.find_first_not_of("0123456789") != std::string::npos) {
            continue;
        }
        // /proc/<pid>/stat: pid (comm) state ppid ...; comm may hold spaces and parentheses, so read after the last ')'.
        const std::string stat = readFile(entry.path().string() + "/stat");
        const std::size_t close = stat.rfind(')');
        if (close == std::string::npos) {
            continue;
        }
        std::istringstream rest(stat.substr(close + 1));
        std::string state;
        long ppid = 0;
        if (rest >> state >> ppid && ppid == self) {
            ++count;
        }
    }
    return count;
}
#endif

// The machine this server runs on, by its name in the fleet (server/home.h serverMachine).
const char* const kHost = "testhost";

// ada's session: the first the server makes.
constexpr sync::SessionId kAda = 1;

// The accounts and AIs a server holds unless a case sets its own: ada, linked to the account work (of the fake
// driver's provider), with no default and no instance. A server holds a registry before its drivers start, as
// nexus-server does once its committed files read; with none no AI is listed (Drivers::listed).
accounts::Registry adaRegistry() {
    accounts::Registry r;
    r.accounts = {{"work", "fake"}};
    r.ais = {{"ada", {"work"}, ""}};
    return r;
}

// nexus-server's AIs over a store in `dir`: the store, the hub and the drivers, as the server makes them; with
// `secrets`, the secret dialog too, its files under `dir`/accounts, offered to a window while `takers` holds. With
// `registry` false, the hub holds no accounts and AIs, as when the committed files do not read at the start.
struct Server {
    std::unique_ptr<store::Fs> fs = store::make_posix_fs();
    DurableStore store;
    std::unique_ptr<Drivers> drivers;
    std::unique_ptr<sync::Hub> hub;
    std::unique_ptr<AccountFiles> files;
    // The secret dialog's effects through the server's own SecretLines: each answer to its asker's driver (and kept,
    // with each log line, for the case to read).
    std::vector<std::string> log;
    std::unique_ptr<SecretLines> lines;
    bool takers = false;
    std::set<pid_t> pids;               // every driver seen running
    std::uint64_t num = 1;

    explicit Server(const std::string& dir, bool secrets = false, bool registry = true) {
        std::string why;
        if (!store.open(*fs, dir + "/store", &why)) {
            std::fprintf(stderr, "drivers_functional: %s\n", why.c_str());
            std::exit(1);
        }
        drivers = std::make_unique<Drivers>(store, Drivers::Options{dir + "/drivers", dir, kTiming, now, kHost, {},
                                                                    secrets});
        hub = std::make_unique<sync::Hub>(store, sync::Limits{}, drivers.get());
        drivers->attach(*hub);
        if (secrets) {
            files = std::make_unique<AccountFiles>(*fs, dir + "/accounts");
            lines = std::make_unique<SecretLines>(
                [this](sync::SessionId id, const std::string& line) {
                    log.push_back(line);
                    drivers->tellSecret(id, line);
                },
                [this](const std::string& line) { log.push_back(line); });
            hub->secretsWith(files.get(), lines.get(), [this] { return takers; },
                             [] { return static_cast<std::uint64_t>(now()); });
        }
        if (registry) {
            CHECK(hub->useAccounts(adaRegistry(), nullptr).empty());
        }
        CHECK(drivers->start(now()));
        // ada's session, as a window's ensure makes it (none on a restart: the store holds it)
        sync::Action a;
        a.kind = sync::ActionKind::Ensure;
        a.aiName = "ada";
        CHECK(hub->apply(a));
        CHECK(store.find(kAda) != nullptr && store.find(kAda)->info.aiName == "ada");
        see();
    }

    // An action of a window's in ada's session.
    sync::Action action(sync::ActionKind kind, const std::string& text) {
        sync::Action a;
        a.num = num++;
        a.kind = kind;
        a.session = kAda;
        a.text = text;
        return a;
    }

    void see() {
        const pid_t p = drivers->pid(kAda);
        if (p > 0) {
            pids.insert(p);
        }
    }

    void send(const std::string& text) {
        CHECK(hub->apply(action(sync::ActionKind::Send, text)));
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

    const Supervisor& sup() { return *drivers->supervisor(kAda); }

    // ada's whole transcript, an entry a line: "P " the person's, "F " the AI's, "T " a tool line, "N " a notice.
    std::vector<std::string> transcript() const {
        std::vector<std::string> out;
        if (const sync::StoredSession* t = store.find(kAda)) {
            for (const sync::EntryRef& e : t->transcript) {
                out.push_back(std::string(e->kind == EntryKind::Person ? "P " : e->kind == EntryKind::AI ? "F " :
                    e->kind == EntryKind::Tool ? "T " :
                    "N ") + e->text);
            }
        }
        return out;
    }
    sync::DriverState driver() const {
        const sync::StoredSession* t = store.find(kAda);
        return t != nullptr ? t->driver : sync::DriverState{};
    }

    // What the windows are sent of ada's activity (the state cursor).
    sync::Activity activity() const {
        const sync::SessionView* v = hub->state().find(kAda);
        return v != nullptr ? v->activity : sync::Activity::Idle;
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
    writeFile(dir + "/drivers", "driver fake " + fake + " --script " + dir + "/ada.script\nai ada fake " + dir +
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
    CHECK((s.transcript() == std::vector<std::string>{"P hi", "F Hello.", "T Read: a file"}));
    CHECK(s.driver().session == "s-1");
    CHECK(s.driver().nextTurn == 2);
    CHECK(s.sup().alive());
}

// Stopped mid-turn: the text so far, then the stopped notice; the driver stays up for the next prompt.
void stop(Server& s, const std::string& dir, const std::string& fake) {
    setUp(dir, fake, std::string(kHello) + "send\nemit {\"kind\":\"text\",\"delta\":\"Work\"}\nawait-stop\n"
        "emit {\"kind\":\"text\",\"delta\":\"ing\"}\nemit {\"kind\":\"text-end\"}\n"
        "emit {\"kind\":\"done\",\"outcome\":\"stopped\"}\nsend\nemit {\"kind\":\"done\",\"outcome\":\"ok\"}\n");
    // ada's session was made before her driver was configured: down until the drivers file is read again.
    CHECK(s.serveUntil([&] { return s.activity() == sync::Activity::Idle; }, 10));
    s.send("go");
    CHECK(s.activity() == sync::Activity::Busy);     // the driver starts for the prompt
    CHECK(s.serveUntil([&] { return s.sup().gathering(); }, 10));
    CHECK(s.activity() == sync::Activity::Busy);
    CHECK(s.hub->apply(s.action(sync::ActionKind::Stop, "")));
    CHECK(s.activity() == sync::Activity::Busy);     // stopping
    CHECK(s.serveUntil([&] { return idle(s); }, 10));
    CHECK(s.activity() == sync::Activity::Idle);
    CHECK((s.transcript() == std::vector<std::string>{"P go", "F Working", "N Stopped."}));
    const pid_t first = s.drivers->pid(kAda);
    s.send("again");
    CHECK(s.serveUntil([&] { return s.driver().nextTurn == 3 && idle(s); }, 10));
    CHECK(s.drivers->pid(kAda) == first);
}

// The AI's text growing shows as the tab's open entry, in memory only; a prompt queued meanwhile waits for the
// turn's end (here a stop), then goes to the driver as the next turn; one queued with the AI idle goes at once.
void queue(Server& s, const std::string& dir, const std::string& fake) {
    setUp(dir, fake, std::string(kHello) + "send\nemit {\"kind\":\"text\",\"delta\":\"Work\"}\nawait-stop\n"
        "emit {\"kind\":\"text-end\"}\nemit {\"kind\":\"done\",\"outcome\":\"stopped\"}\n"
        "send\nemit {\"kind\":\"text\",\"delta\":\"Second\"}\nemit {\"kind\":\"text-end\"}\n"
        "emit {\"kind\":\"done\",\"outcome\":\"ok\"}\nsend\nemit {\"kind\":\"done\",\"outcome\":\"ok\"}\n");
    s.send("go");
    CHECK(s.serveUntil([&] { return s.sup().gathering(); }, 10));
    const sync::SessionView* v = s.hub->state().find(kAda);
    CHECK(v != nullptr && !v->entries.empty() && !v->entries.back()->closed && v->entries.back()->text == "Work");
    CHECK((s.transcript() == std::vector<std::string>{"P go"}));
    CHECK(s.hub->apply(s.action(sync::ActionKind::Queue, "next")));
    CHECK(s.store.find(kAda)->queue == std::vector<std::string>{"next"});
    CHECK((s.transcript() == std::vector<std::string>{"P go"}));
    CHECK(s.hub->apply(s.action(sync::ActionKind::Stop, "")));
    CHECK(s.serveUntil([&] { return s.driver().nextTurn == 3 && idle(s); }, 10));
    CHECK((s.transcript() == std::vector<std::string>{"P go", "F Work", "N Stopped.", "P next", "F Second"}));
    CHECK(s.store.find(kAda)->queue.empty());
    v = s.hub->state().find(kAda);
    CHECK(v != nullptr && v->entries.back()->closed);
    CHECK(s.hub->apply(s.action(sync::ActionKind::Queue, "idle")));
    CHECK(s.serveUntil([&] { return s.driver().nextTurn == 4 && idle(s); }, 10));
    CHECK(s.transcript().back() == "P idle");
    CHECK(s.store.find(kAda)->queue.empty());
}

// Command-Escape hammered, stops applied in batches with prompts queued: a stop while the driver starts cancels the
// turn about to begin, each stop ends the turn running when it arrives, and the rest (the turn already stopping, or
// nothing running) vanish: none is kept for a later turn, and nothing queued is lost.
void stopHammered(Server& s, const std::string& dir, const std::string& fake) {
    setUp(dir, fake, std::string(kHello) + "send\nemit {\"kind\":\"text\",\"delta\":\"Work\"}\nawait-stop\n"
        "emit {\"kind\":\"text-end\"}\nemit {\"kind\":\"done\",\"outcome\":\"stopped\"}\n"
        "send\nemit {\"kind\":\"text\",\"delta\":\"A\"}\nawait-stop\nemit {\"kind\":\"text-end\"}\n"
        "emit {\"kind\":\"done\",\"outcome\":\"stopped\"}\n"
        "send\nemit {\"kind\":\"text\",\"delta\":\"B\"}\nemit {\"kind\":\"text-end\"}\n"
        "emit {\"kind\":\"done\",\"outcome\":\"ok\"}\nsend\nemit {\"kind\":\"done\",\"outcome\":\"ok\"}\n");
    const auto stops = [&](int n) {
        for (int i = 0; i < n; ++i) {
            CHECK(s.hub->apply(s.action(sync::ActionKind::Stop, "")));
        }
    };
    s.send("never");
    CHECK(s.sup().phase() == Phase::Starting);
    stops(3);
    CHECK((s.transcript() == std::vector<std::string>{"P never", "N Stopped."}));
    CHECK(s.driver().waiting.empty() && s.driver().nextTurn == 1);
    s.send("go");
    CHECK(s.serveUntil([&] { return s.sup().gathering(); }, 10));
    for (const char* q : {"q1", "q2"}) {
        CHECK(s.hub->apply(s.action(sync::ActionKind::Queue, q)));
    }
    stops(4);
    CHECK(s.serveUntil([&] { return s.driver().openTurn == 2 && s.sup().gathering(); }, 10));
    stops(3);
    CHECK(s.serveUntil([&] { return s.driver().nextTurn == 4 && idle(s); }, 10));
    stops(5);
    CHECK(s.store.find(kAda)->queue.empty() && idle(s));
    s.send("last");
    CHECK(s.serveUntil([&] { return s.driver().nextTurn == 5 && idle(s); }, 10));
    CHECK((s.transcript() == std::vector<std::string>{"P never", "N Stopped.", "P go", "F Work", "N Stopped.",
        "P q1", "F A", "N Stopped.", "P q2", "F B", "P last"}));
}

// The drivers file loses ada mid-turn: a prompt queued then stays queued at the turn's end, since there is no driver
// to give it to; once ada is configured again, the next action takes the oldest, one per turn.
void queueUnconfigured(Server& s, const std::string& dir, const std::string& fake) {
    setUp(dir, fake, std::string(kHello) + "send\nemit {\"kind\":\"text\",\"delta\":\"Work\"}\nawait-stop\n"
        "emit {\"kind\":\"text-end\"}\nemit {\"kind\":\"done\",\"outcome\":\"stopped\"}\n"
        "send\nemit {\"kind\":\"text\",\"delta\":\"Second\"}\nemit {\"kind\":\"text-end\"}\n"
        "emit {\"kind\":\"done\",\"outcome\":\"ok\"}\nsend\nemit {\"kind\":\"done\",\"outcome\":\"ok\"}\n");
    s.send("go");
    CHECK(s.serveUntil([&] { return s.sup().gathering(); }, 10));
    const std::string table = dir + "/drivers";
    std::filesystem::rename(table, table + ".away");
    CHECK(s.hub->apply(s.action(sync::ActionKind::Queue, "next")));
    CHECK(s.hub->apply(s.action(sync::ActionKind::Stop, "")));
    CHECK(s.serveUntil([&] { return idle(s); }, 10));
    CHECK((s.transcript() == std::vector<std::string>{"P go", "F Work", "N Stopped."}));
    CHECK(s.store.find(kAda)->queue == std::vector<std::string>{"next"});
    CHECK(s.driver().waiting.empty());
    CHECK(s.activity() == sync::Activity::Down);    // a driver idle, but none configured: nothing queued is taken
    std::filesystem::rename(table + ".away", table);
    CHECK(s.hub->apply(s.action(sync::ActionKind::Queue, "later")));
    CHECK(s.serveUntil([&] { return s.driver().nextTurn == 4 && idle(s); }, 10));
    CHECK((s.transcript() == std::vector<std::string>{"P go", "F Work", "N Stopped.", "P next", "F Second",
        "P later"}));
    CHECK(s.store.find(kAda)->queue.empty());
}

// An AI with no driver configured shows down (steady red), not idle: from the session's making, through a prompt
// refused for want of a driver and one queued, which stays queued. Once the drivers file gives one, the server reads
// it again within kResolveMs: the AI shows idle, and the queued prompt goes to the new driver without another
// action of the person's.
void unconfigured(Server& s, const std::string& dir, const std::string& fake) {
    CHECK(s.activity() == sync::Activity::Down);
    s.send("anyone?");
    CHECK(s.transcript().size() == 2 && has(s.transcript().back(), "ada"));
    CHECK(s.activity() == sync::Activity::Down);
    CHECK(s.hub->apply(s.action(sync::ActionKind::Queue, "later")));
    CHECK(s.store.find(kAda)->queue == std::vector<std::string>{"later"});
    s.serveUntil([] { return false; }, 1.5);    // read again past kResolveMs: still none
    CHECK(s.activity() == sync::Activity::Down && s.drivers->pid(kAda) < 0);
    setUp(dir, fake, std::string(kHello) + "send\nemit {\"kind\":\"text\",\"delta\":\"Here.\"}\n"
        "emit {\"kind\":\"text-end\"}\nemit {\"kind\":\"done\",\"outcome\":\"ok\"}\n");
    CHECK(s.serveUntil([&] { return s.driver().nextTurn == 2 && idle(s); }, 10));
    CHECK(s.store.find(kAda)->queue.empty());
    CHECK(s.transcript().size() == 4 && s.transcript()[2] == "P later" && s.transcript()[3] == "F Here.");
    CHECK(s.activity() == sync::Activity::Idle);
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
// Where the window case writes the states it reaches, if anywhere (the optional fourth argument).
std::string stateDir;

// The hub's state now, as a window's first diff, into stateDir/<name>.state for `nexus --state` to draw.
void writeState(Server& s, const std::string& name) {
    if (!stateDir.empty()) {
        writeFile(stateDir + "/" + name + ".state", sync::stateFile(s.hub->state()));
    }
}

// What the window draws, from the server's state through the fake driver: a reply streaming after two tool lines
// (busy), the turn ended (idle), then the driver crashing on the next prompt (down). Each state is written for the
// window's screenshots when a state directory is given.
void window(Server& s, const std::string& dir, const std::string& fake) {
    setUp(dir, fake, std::string(kHello) + "send\nemit {\"kind\":\"session\",\"id\":\"s-1\"}\n"
        "emit {\"kind\":\"text\",\"delta\":\"I'll start with the failed job's log.\"}\nemit {\"kind\":\"text-end\"}\n"
        "emit {\"kind\":\"tool\",\"name\":\"Bash\",\"summary\":\"gh run view 4211 --log-failed\"}\n"
        "emit {\"kind\":\"tool\",\"name\":\"Read\",\"summary\":\"tests/sync/link_walk.cpp\"}\n"
        "emit {\"kind\":\"text\",\"delta\":\"The nightly went red in the link walk: seed 17 \"}\n"
        "emit {\"kind\":\"text\",\"delta\":\"loses a diff when the\"}\nawait-stop\n"
        "emit {\"kind\":\"text\",\"delta\":\" window reconnects mid-fragment.\"}\nemit {\"kind\":\"text-end\"}\n"
        "emit {\"kind\":\"done\",\"outcome\":\"ok\"}\n"
        "send\nemit {\"kind\":\"text\",\"delta\":\"On it.\"}\nemit {\"kind\":\"text-end\"}\nexit 1\n");
    s.send("Why did the nightly build go red?");
    const std::string growing = "The nightly went red in the link walk: seed 17 loses a diff when the";
    CHECK(s.serveUntil([&] {
        const sync::SessionView* v = s.hub->state().find(kAda);
        return v != nullptr && !v->entries.empty() && !v->entries.back()->closed && v->entries.back()->text == growing;
    }, 10));
    CHECK((s.transcript() == std::vector<std::string>{"P Why did the nightly build go red?",
        "F I'll start with the failed job's log.", "T Bash: gh run view 4211 --log-failed",
        "T Read: tests/sync/link_walk.cpp"}));
    CHECK(s.activity() == sync::Activity::Busy);
    writeState(s, "busy");
    CHECK(s.hub->apply(s.action(sync::ActionKind::Stop, "")));
    CHECK(s.serveUntil([&] { return idle(s) && s.activity() == sync::Activity::Idle; }, 10));
    CHECK(s.activity() == sync::Activity::Idle);
    writeState(s, "idle");
    s.send("Can you fix it?");
    CHECK(s.serveUntil([&] { return none(s) && s.activity() == sync::Activity::Down; }, 10));
    CHECK(s.activity() == sync::Activity::Down);
    writeState(s, "down");
}

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
    CHECK(s.activity() == sync::Activity::Down);     // the driver crashed: down until the next prompt
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
    CHECK(s.hub->apply(s.action(sync::ActionKind::Stop, "")));
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
    CHECK(s.hub->apply(s.action(sync::ActionKind::Stop, "")));
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
        "on-start 2 emit {\"kind\":\"done\",\"outcome\":\"ok\"}\n");
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

// The primary conversation uses the installation's neutral driver, account and workspace.
void primaryRoute(Server& s, const std::string& dir, const std::string& fake) {
    const std::string workspace = dir + "/installer workspace";
    std::filesystem::create_directory(workspace);
    const std::string script = dir + "/primary.script";
    writeFile(script, "count-starts " + dir + "/starts\nrecord " + dir + "/starts.log\n" + kHello +
        "send\non-start 1 emit {\"kind\":\"session\",\"id\":\"reported-1\"}\n"
        "on-start 1 emit {\"kind\":\"child-agents\",\"count\":2}\non-start 1 await-stop\n"
        "on-start 1 emit {\"kind\":\"done\",\"outcome\":\"stopped\"}\n"
        "on-start 2 emit {\"kind\":\"done\",\"outcome\":\"ok\"}\nexit 0\n");
    std::ostringstream driver;
    driver << "driver codex /unconfigured-named-AI-driver\nprimary-driver codex ["
           << std::quoted(fake) << ",\"--script\"," << std::quoted(script) << "]\n";
    const auto route = [&](const std::string& cwd) { return driver.str() + "primary codex personal " + cwd + "\n"; };
    writeFile(dir + "/drivers", route(workspace));
    const DriverTable table = parseDriverTable(route(workspace), dir);
    CHECK(table.errors.empty() && table.primary.has_value());
    if (!table.primary) return;

    auto registry = adaRegistry();
    registry.accounts.push_back({"personal", "codex"});
    CHECK(s.hub->useAccounts(registry, nullptr).empty());
    s.hub->reportSignedIn("personal", true);
    const sync::SessionInfo binding{std::string(kPrimaryChat), "codex", "personal", {}, {}, table.primary->instance()};
    std::string why;
    CHECK(s.hub->ensurePrimary(binding, &why));
    const auto id = s.hub->primarySession();
    CHECK(id.has_value());
    if (!id) return;
    const auto act = [&](sync::ActionKind kind, const std::string& text) {
        sync::Action a = s.action(kind, text);
        a.session = *id;
        CHECK(s.hub->apply(a));
    };
    act(sync::ActionKind::Send, "first");
    CHECK(s.serveUntil([&] {
        const auto* view = s.hub->state().find(*id);
        return view && view->activeAgents == 3;
    }, 10));
    act(sync::ActionKind::Stop, {});
    CHECK(s.serveUntil([&] {
        return s.drivers->pid(*id) < 0 && s.store.find(*id)->driver.session == "reported-1";
    }, 10));
    act(sync::ActionKind::Send, "second");
    CHECK(s.serveUntil([&] {
        return s.drivers->pid(*id) < 0 && s.store.find(*id)->driver.nextTurn == 3;
    }, 10));
    const std::string starts = readFile(dir + "/starts.log");
    CHECK(has(starts, "cwd=" + workspace + "\n"));
    CHECK(has(starts, "resume=\naccount=personal\nlimit=cwd\n"));
    CHECK(has(starts, "resume=reported-1\naccount=personal\nlimit=cwd\n"));
    CHECK(readFile(dir + "/starts") == "2\n");
    CHECK(s.hub->accounts()->aiNamed(kPrimaryChat) == nullptr);

    // A session still held by a window cannot move to the next installation's workspace.
    writeFile(dir + "/drivers", route(dir));
    act(sync::ActionKind::Send, "from the old workspace");
    CHECK(s.drivers->pid(*id) < 0);
    CHECK(readFile(dir + "/starts") == "2\n");
    const auto* retained = s.store.find(*id);
    CHECK(retained && retained->driver.nextTurn == 3 && !retained->transcript.empty() &&
          retained->transcript.back()->kind == EntryKind::Notice);
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

// The session's account changed: its driver is ended once the turn open has ended (never counted as a failed start),
// and the next prompt starts one again in the same session, its id, transcript and turn numbers kept. A change of
// model leaves the driver running.
// Group commit's hold (Drivers::hold, release): a prompt sent while the drivers are held opens its turn and is kept,
// its bytes unwritten, for as long as the hold lasts; the AI cannot answer what it was never sent. Released (the
// batch's flush has returned), it is written once, and the turn is answered as ever. Then a turn whose driver hangs on
// a stop: held, the stop's line, the end of its input, SIGTERM and SIGKILL are each kept as their deadlines pass, and
// the driver, which would end at any one of the last three, runs on (were one carried out, serve() would collect the
// exit); released, they go out in order and the driver ends.
void held(Server& s, const std::string& dir, const std::string& fake) {
    setUp(dir, fake, std::string(kHello) + "send\nemit {\"kind\":\"text\",\"delta\":\"Back.\"}\n"
        "emit {\"kind\":\"text-end\"}\nemit {\"kind\":\"done\",\"outcome\":\"ok\"}\n"
        "send\nemit {\"kind\":\"text\",\"delta\":\"a\"}\nhang\n");
    s.drivers->hold();
    s.send("hi");
    CHECK(s.serveUntil([&] { return s.sup().phase() == Phase::Working && s.drivers->unwritten(kAda) > 0; }, 10));
    CHECK((s.transcript() == std::vector<std::string>{"P hi"}));
    CHECK(s.drivers->holding());
    s.drivers->release(now());
    CHECK(s.drivers->unwritten(kAda) == 0);
    CHECK(s.serveUntil([&] { return s.transcript().size() == 2 && idle(s); }, 10));
    CHECK((s.transcript() == std::vector<std::string>{"P hi", "F Back."}));

    s.send("again");
    CHECK(s.serveUntil([&] { return s.sup().gathering(); }, 10));
    const pid_t running = s.drivers->pid(kAda);
    CHECK(running > 0);
    s.drivers->hold();
    CHECK(s.hub->apply(s.action(sync::ActionKind::Stop, "")));
    bool closed = false;
    bool termed = false;
    CHECK(s.serveUntil([&] {
        closed = closed || s.sup().stage() == Stage::Closed;
        termed = termed || s.sup().stage() == Stage::Termed;
        return s.sup().stage() == Stage::Killed;
    }, 10));
    CHECK(closed && termed);
    // A while longer, held: an exit would be collected by now.
    CHECK(!s.serveUntil([&] { return s.drivers->pid(kAda) != running; }, 0.3));
    CHECK(s.drivers->pid(kAda) == running);
    CHECK(::kill(running, 0) == 0);
    CHECK(s.drivers->holding());
    s.drivers->release(now());
    CHECK(s.serveUntil([&] { return none(s); }, 10));
    CHECK(s.drivers->pid(kAda) != running);
    s.end();
}

void account(Server& s, const std::string& dir, const std::string& fake) {
    // each start runs the script from the top: the first answers with text until stopped, the second at once
    setUp(dir, fake, "count-starts " + dir + "/starts\nrecord " + dir + "/starts.log\n" + kHello +
        "send\non-start 1 emit {\"kind\":\"session\",\"id\":\"s-1\"}\n"
        "on-start 1 emit {\"kind\":\"text\",\"delta\":\"one\"}\non-start 1 await-stop\n"
        "on-start 1 emit {\"kind\":\"text-end\"}\non-start 1 emit {\"kind\":\"done\",\"outcome\":\"stopped\"}\n"
        "on-start 2 emit {\"kind\":\"done\",\"outcome\":\"ok\"}\n");
    s.send("first");
    CHECK(s.serveUntil([&] { return s.sup().gathering(); }, 10));
    const pid_t before = s.drivers->pid(kAda);
    CHECK(s.hub->apply(s.action(sync::ActionKind::SetModel, "fast")));
    CHECK(s.store.find(kAda)->info.model == "fast");
    CHECK(s.hub->apply(s.action(sync::ActionKind::SetAccount, "work")));
    CHECK(s.store.find(kAda)->info.account == "work");
    const std::string actual = s.sup().driverAccount();
    CHECK(!s.drivers->retireForSignOut(actual).empty());
    CHECK(s.drivers->retireForSignOut("unrelated").empty());
    // the turn open runs on under the account it started with
    CHECK(s.drivers->pid(kAda) == before && s.sup().gathering());
    CHECK(s.hub->apply(s.action(sync::ActionKind::Stop, "")));
    CHECK(s.serveUntil([&] { return none(s) && s.driver().openTurn == 0; }, 10));
    CHECK(s.sup().failedStarts() == 0);
    CHECK((s.transcript() == std::vector<std::string>{"P first", "F one", "N Stopped."}));
    s.send("second");
    CHECK(s.serveUntil([&] { return idle(s) && s.driver().nextTurn == 3; }, 10));
    CHECK(s.drivers->pid(kAda) != before && s.drivers->pid(kAda) > 0);
    CHECK(s.transcript().size() == 5 && has(s.transcript().back(), "belongs to a different account"));
    CHECK(s.store.size() == 1 && s.store.find(kAda)->info.account == "work");
    CHECK(!has(readFile(dir + "/starts.log"), "resume=s-1"));
    CHECK(s.drivers->retireForSignOut("unrelated").empty() && idle(s));
    CHECK(s.drivers->retireForSignOut("work").empty());
    CHECK(s.sup().phase() == Phase::Ending);
    CHECK(s.serveUntil([&] { return none(s); }, 10));
    CHECK(s.sup().failedStarts() == 0);
}

// ada's drivers file and script for a driver that takes an account (`--account`, as the Claude driver's line does), and
// the server's accounts: a1 and a2, both ada's, a1 her default.
void setUpAccounts(Server& s, const std::string& dir, const std::string& fake, const std::string& script,
                   const std::string& provider = "fake") {
    writeFile(dir + "/ada.script", script);
    writeFile(dir + "/drivers", "driver " + provider + " " + fake + " --script " + dir + "/ada.script --account from-the-file\n"
        "AI ada " + provider + " " + dir + "\n");
    accounts::Registry r;
    r.accounts = {{"a1", provider}, {"a2", provider}};
    r.ais = {{"ada", {"a1", "a2"}, "a1"}};
    CHECK(s.hub->useAccounts(r, nullptr).empty());
}

bool signedIn(Server& s, const std::string& account) {
    return s.hub->signedIn().count(account) == 1;
}

// A bus prompt takes the real driver's bounded admission route, with its own transcript attribution.
void busAdmission(Server& s, const std::string& dir, const std::string& fake) {
    const std::string sends = dir + "/bus-sends";
    const std::string record = "send\nrecord-send " + sends + "\n";
    const std::string done = "emit {\"kind\":\"done\",\"outcome\":\"ok\"}\n";
    setUpAccounts(s, dir, fake, std::string(kHello) + record + "await-stop\n" + done +
        record + done + record + done + record + done);
    CHECK(s.hub->apply(s.action(sync::ActionKind::SetAccount, "a1")));
    CHECK(s.hub->apply(sync::Action{0, sync::ActionKind::Create, 0, "ada", {}, {}}));
    std::string why;
    auto receive = [&](const std::string& id, const std::string& text) {
        return s.hub->receiveBus("ada", id, "bea", text, text, &why);
    };
    CHECK(!receive("100-0", "/clear") && !s.hub->hasBusReceipt("ada", "100-0"));
    s.hub->reportSignedIn("a1", true);
    CHECK(!receive("0100-0", "/clear") && !receive("18446744073709551616-0", "/clear"));
    CHECK(!s.hub->receiveBus("ada", "100-0", "bea\nspoof", "/clear", "/clear", &why));
    CHECK(!s.hub->receiveBus("ada", "100-0", "bea", std::string("x\0y", 3), "/clear", &why));
    CHECK(!s.hub->hasBusReceipt("ada", "100-0"));
    CHECK(receive("100-0", "/clear"));
    CHECK(s.serveUntil([&] { return has(readFile(sends), "/clear\n"); }, 10));
    const auto* primary = s.store.find(kAda);
    CHECK(primary->info.contextEpoch == 0);
    CHECK(primary->transcript.back()->kind == EntryKind::Bus && primary->transcript.back()->text == "bea\n/clear");

    // The open turn stays open, the next incoming turn waits. Older explicitly queued work has priority.
    CHECK(receive("99-0", "/model literal"));
    CHECK(s.driver().waiting == std::vector<std::string>{"/model literal"});
    CHECK(s.hub->apply(s.action(sync::ActionKind::Queue, "human queued")));
    CHECK(!receive("101-0", "later") && !s.hub->hasBusReceipt("ada", "101-0"));
    CHECK(s.hub->apply(s.action(sync::ActionKind::Stop, "")));
    CHECK(s.serveUntil([&] { return has(readFile(sends), "human queued\n") && idle(s); }, 10));
    CHECK(receive("101-0", "later"));
    CHECK(s.serveUntil([&] { return has(readFile(sends), "later\n") && idle(s); }, 10));
    const std::string observed = readFile(sends);
    CHECK(observed == "send 1:\n/clear\nsend 2:\n/model literal\nsend 3:\nhuman queued\nsend 4:\nlater\n");
    CHECK(s.store.find(2)->driver.nextTurn == 1);
    for (const auto& e : primary->transcript)
        CHECK(e->kind != EntryKind::Person || e->text == "human queued");

    // The receipt outlives the visible message and a login change. A replay cannot affect another recipient.
    CHECK(s.hub->apply(s.action(sync::ActionKind::Send, "/clear")));
    s.hub->reportSignedIn("a1", false);
    const auto afterClear = primary->transcript.size();
    CHECK(receive("100-0", "different replay body") && why.empty());
    CHECK(primary->transcript.size() == afterClear && primary->driver.nextTurn == 5);
    CHECK(!s.hub->hasBusReceipt("bea", "100-0") && !s.hub->hasBusReceipt("ada:100-0", "101-0"));
}

// A driver's credential event reports its account signed in, and the driver runs on the session's account: the
// drivers file's --account value is replaced by it.
void signIn(Server& s, const std::string& dir, const std::string& fake) {
    setUpAccounts(s, dir, fake, "count-starts " + dir + "/starts\nrecord " + dir + "/starts.log\n" + kHello +
        "send\nrecord-send " + dir + "/sends\n"
        "on-start 1 emit {\"kind\":\"session\",\"id\":\"first-id\"}\n"
        "on-start 2 emit {\"kind\":\"session\",\"id\":\"second-id\"}\n"
        "emit {\"kind\":\"done\",\"outcome\":\"ok\"}\nexit 0\n");
    CHECK(s.hub->signedIn().empty());
    CHECK(s.hub->apply(s.action(sync::ActionKind::SetAccount, "a1")));
    s.send("first turn");
    CHECK(s.serveUntil([&] { return none(s) && s.driver().session == "first-id"; }, 10));
    CHECK(signedIn(s, "a1") && !signedIn(s, "a2"));
    CHECK(s.hub->apply(s.action(sync::ActionKind::SetAccount, "a2")));
    s.send("second turn");
    CHECK(s.serveUntil([&] { return none(s) && s.driver().session == "second-id"; }, 10));
    CHECK(signedIn(s, "a1") && signedIn(s, "a2"));
    s.send("third turn");
    CHECK(s.serveUntil([&] { return none(s) && s.driver().nextTurn == 4; }, 10));
    CHECK(s.hub->apply(s.action(sync::ActionKind::SetAccount, "a1")));
    s.send("last turn");
    CHECK(s.serveUntil([&] { return none(s) && s.driver().nextTurn == 5; }, 10));
    const std::string log = readFile(dir + "/starts.log");
    CHECK(!has(log, "from-the-file") && !has(log, "resume=first-id"));
    CHECK(has(log, "resume=\naccount=a1\n") && has(log, "resume=\naccount=a2\n"));
    CHECK(has(log, "resume=second-id\naccount=a2\n") && !has(log, "resume=second-id\naccount=a1\n"));
    const std::string sends = readFile(dir + "/sends");
    CHECK(has(sends, "The person: first turn\n") && has(sends, "send 3:\nthird turn\n"));
    CHECK(has(sends, "The person: third turn\n") && has(sends, "prompt follows.]\n\nlast turn\n"));
}

// A driver that has no credential for its account reports it as needing sign-in: the account is signed in no more.
void noCredential(Server& s, const std::string& dir, const std::string& fake) {
    setUpAccounts(s, dir, fake, "emit {\"kind\":\"hello\",\"protocol\":1,\"harness\":\"fake\",\"features\":[\"limit\"]}\n"
        "emit {\"kind\":\"error\",\"cause\":\"a1 has no token\",\"fix\":\"make one\",\"code\":\"no-credential\"}\n"
        "exit 0\n");
    s.hub->reportSignedIn("a1", true);
    CHECK(s.hub->apply(s.action(sync::ActionKind::SetAccount, "a1")));
    s.send("q");
    CHECK(s.serveUntil([&] { return !signedIn(s, "a1"); }, 10));
    CHECK(!signedIn(s, "a1"));
    CHECK(s.serveUntil([&] { return none(s); }, 10));
}

// A limited AI (ada: no "limit" in her entry) on a driver whose hello does not list "limit": the driver is ended
// before any prompt is written to it, and the session is told why. It never runs the prompt unlimited.
void limitUnsupported(Server& s, const std::string& dir, const std::string& fake) {
    setUpAccounts(s, dir, fake, "record " + dir + "/starts.log\n"
        "emit {\"kind\":\"hello\",\"protocol\":1,\"harness\":\"fake\"}\nsend\nrecord-send " + dir + "/sends.log\n"
        "emit {\"kind\":\"done\",\"outcome\":\"ok\"}\n");
    s.hub->reportSignedIn("a1", true);
    CHECK(s.hub->apply(s.action(sync::ActionKind::SetAccount, "a1")));
    s.send("q");
    const auto told = [&] {
        for (const std::string& line : s.transcript()) {
            if (has(line, "does not support the limit to its directory")) {
                return true;
            }
        }
        return false;
    };
    CHECK(s.serveUntil(told, 10));
    CHECK(told());
    CHECK(has(readFile(dir + "/starts.log"), "limit=cwd\n"));
    CHECK(readFile(dir + "/sends.log").empty());     // the prompt never reached it
}

// A usage limit on the session's account: the session moves to ada's next signed-in account, told, and its next
// prompt starts a driver on that account.
void usageLimit(Server& s, const std::string& dir, const std::string& fake) {
    setUpAccounts(s, dir, fake, "count-starts " + dir + "/starts\nrecord " + dir + "/starts.log\n" + kHello + "send\n"
        "on-start 1 emit {\"kind\":\"error\",\"cause\":\"the limit\",\"fix\":\"wait\",\"code\":\"usage-limit\"}\n"
        "on-start 1 emit {\"kind\":\"done\",\"outcome\":\"error\"}\n"
        "on-start 2 emit {\"kind\":\"done\",\"outcome\":\"ok\"}\n");
    s.hub->reportSignedIn("a1", true);
    s.hub->reportSignedIn("a2", true);
    CHECK(s.hub->apply(s.action(sync::ActionKind::SetAccount, sync::kAutoAccountTarget)));
    s.send("first");
    CHECK(s.serveUntil([&] { return s.store.find(kAda)->info.account == "a2"; }, 10));
    CHECK(s.store.find(kAda)->info.account == "a2");
    bool told = false;
    for (const std::string& line : s.transcript()) {
        told = told || has(line, "Switched to a2");
    }
    CHECK(told);
    CHECK(s.serveUntil([&] { return none(s) || idle(s); }, 10));
    s.send("second");
    CHECK(s.serveUntil([&] { return idle(s) && has(readFile(dir + "/starts.log"), "account=a2\n"); }, 10));
    CHECK(has(readFile(dir + "/starts.log"), "account=a2\n"));
}

// Exercise both acknowledged choices through a responding supervisor and two real child processes. The AI
// links a third subscription which is signed in but belongs to a different driver's provider.
// Observe the real pipe writes across two account-owned processes. A tool already ran and the interrupted reply
// was still streaming when the first process reported quota. One visible request yields two distinct sends.
void continuation(Server& s, const std::string& dir, const std::string& fake) {
    setUpAccounts(s, dir, fake, "count-starts " + dir + "/starts\nrecord " + dir + "/starts.log\n" + kHello +
        "send\nrecord-send " + dir + "/sends\n"
        "on-start 1 emit {\"kind\":\"session\",\"id\":\"private-first-id\"}\n"
        "on-start 1 emit {\"kind\":\"tool\",\"name\":\"Write\",\"summary\":\"created draft.txt\"}\n"
        "on-start 1 emit {\"kind\":\"text\",\"delta\":\"" + std::string(20000, 'x') + " I finished the first section.\"}\n"
        "on-start 1 emit {\"kind\":\"error\",\"cause\":\"subscription limit\",\"fix\":\"wait\",\"code\":\"usage-limit\"}\n"
        "on-start 2 emit {\"kind\":\"text\",\"delta\":\"Finished the remainder.\"}\n"
        "emit {\"kind\":\"done\",\"outcome\":\"ok\"}\nexit 0\n");
    s.hub->reportSignedIn("a1", true);
    s.hub->reportSignedIn("a2", true);
    CHECK(s.hub->apply(s.action(sync::ActionKind::SetAccount, sync::kAutoAccountTarget)));
    s.send("finish my draft");
    CHECK(s.serveUntil([&] { return none(s) && s.driver().nextTurn == 3; }, 10));
    const std::string sends = readFile(dir + "/sends");
    CHECK(has(sends, "send 1:\nfinish my draft\n") && has(sends, "send 2:\n[nexus:"));
    CHECK(has(sends, "Tool activity: Write: created draft.txt") && has(sends, " I finished the first section."));
    CHECK(has(sends, "Do not repeat completed work.]\n\nfinish my draft\n"));
    const std::string starts = readFile(dir + "/starts.log");
    CHECK(has(starts, "account=a1\n") && has(starts, "account=a2\n") && !has(starts, "resume=private-first-id"));
    const auto transcript = s.transcript();
    CHECK(std::count(transcript.begin(), transcript.end(), "P finish my draft") == 1);
    CHECK(std::count(transcript.begin(), transcript.end(), "F Finished the remainder.") == 1);
    CHECK(s.store.size() == 1 && s.store.find(kAda)->info.account == "a2" && s.driver().continuationPrompt.empty());
}

// A quota handoff replays one attributed bus request. Earlier bus attachments keep their readable
// paths in the handoff, and messages already waiting keep their own later turns.
void busContinuation(Server& s, const std::string& dir, const std::string& fake) {
    const std::string record = "send\nrecord-send " + dir + "/sends\n";
    const std::string done = "emit {\"kind\":\"done\",\"outcome\":\"ok\"}\n";
    setUpAccounts(s, dir, fake, "count-starts " + dir + "/starts\n" + kHello + record + done + record +
        "on-start 1 emit {\"kind\":\"error\",\"cause\":\"quota\",\"fix\":\"wait\",\"code\":\"usage-limit\"}\n"
        "on-start 1 emit {\"kind\":\"done\",\"outcome\":\"error\"}\n"
        "on-start 2 " + done + "on-start 3 " + done + "exit 0\n");
    s.hub->reportSignedIn("a1", true);
    s.hub->reportSignedIn("a2", true);
    CHECK(s.hub->apply(s.action(sync::ActionKind::SetAccount, sync::kAutoAccountTarget)));
    const std::string path = "incoming/bus-123/message.txt";
    const std::string past = "Message from bea on the message bus (id 10-0)\nSubject: reference\n\nText attachment: " + path;
    const std::string current = "Message from bo on the message bus (id 11-0)\nSubject: editing\n\nedit the reference";
    const std::string later = "Message from cy on the message bus (id 12-0)\nSubject: publishing\n\npublish afterwards";
    std::string why;
    CHECK(s.hub->receiveBus("ada", "10-0", "bea", "[message.txt (4097 bytes)]", past, &why));
    CHECK(s.serveUntil([&] { return idle(s); }, 10));
    CHECK(s.hub->receiveBus("ada", "11-0", "bo", "edit the reference", current, &why));
    CHECK(s.hub->receiveBus("ada", "12-0", "cy", "publish afterwards", later, &why));
    CHECK(s.serveUntil([&] { return none(s) && s.driver().nextTurn == 5; }, 10));
    CHECK(s.store.find(kAda)->info.account == "a2");
    const std::string sends = readFile(dir + "/sends");
    const std::string prefix = "send 1:\n" + past + "\nsend 2:\n" + current + "\nsend 3:\n";
    const std::string suffix = "Do not repeat completed work.]\n\n" + current + "\nsend 4:\n" + later + "\n";
    CHECK(sends.starts_with(prefix) && sends.ends_with(suffix));
    if (!sends.starts_with(prefix) || !sends.ends_with(suffix)) return;
    const std::string history = sends.substr(prefix.size(), sends.size() - prefix.size() - suffix.size());
    CHECK(has(history, "Message bus: " + past));
    CHECK(!has(history, "edit the reference") && !has(history, "publish afterwards"));

    // An ordinary fresh context after /compact builds a summary through a separate driver path.
    CHECK(s.hub->apply(s.action(sync::ActionKind::Send, "/compact")));
    const std::string fresh = "Message from bo on the message bus (id 13-0)\n\nreview final draft";
    const std::string queued = "Message from cy on the message bus (id 14-0)\n\nnotify the editor";
    CHECK(s.hub->receiveBus("ada", "13-0", "bo", "review final draft", fresh, &why));
    CHECK(s.hub->receiveBus("ada", "14-0", "cy", "notify the editor", queued, &why));
    CHECK(s.serveUntil([&] { return none(s) && s.driver().nextTurn == 7; }, 10));
    const std::string all = readFile(dir + "/sends");
    const std::string start = sends + "send 5:\n";
    const std::string finish = "\n\n" + fresh + "\nsend 6:\n" + queued + "\n";
    CHECK(all.starts_with(start) && all.ends_with(finish));
    if (!all.starts_with(start) || !all.ends_with(finish)) return;
    const std::string compacted = all.substr(start.size(), all.size() - start.size() - finish.size());
    CHECK(has(compacted, "Message bus: " + past));
    CHECK(!has(compacted, "review final draft") && !has(compacted, "notify the editor"));
}

// Exercise the same decision a window sees across the wire. The first process has already done work;
// either choice continues it under the explicitly selected account and model in a second process.
void fableChoice(Server& s, const std::string& dir, const std::string& fake, bool switchAccount) {
    setUpAccounts(s, dir, fake, "count-starts " + dir + "/starts\nrecord " + dir + "/launches\n"
        "emit {\"kind\":\"hello\",\"protocol\":1,\"harness\":\"claude\",\"features\":[\"limit\"]}\n"
        "emit {\"kind\":\"credential\",\"billing\":\"subscription\",\"plan\":\"max\"}\n"
        "send\nrecord-send " + dir + "/sends\n"
        "on-start 1 emit {\"kind\":\"session\",\"id\":\"first-fable-session\"}\n"
        "on-start 1 emit {\"kind\":\"tool\",\"name\":\"Write\",\"summary\":\"draft saved\"}\n"
        "on-start 1 emit {\"kind\":\"text\",\"delta\":\"First section done.\"}\n"
        "on-start 1 emit {\"kind\":\"error\",\"cause\":\"Fable limit reached\",\"fix\":\"choose\",\"code\":\"model-limit\"}\n"
        "on-start 2 emit {\"kind\":\"text\",\"delta\":\"Second section done.\"}\n"
        "emit {\"kind\":\"done\",\"outcome\":\"ok\"}\nexit 0\n", "claude");
    accounts::Registry r;
    r.accounts = {{"a1", "claude"}, {"foreign", "codex"}, {"a2", "claude"}};
    r.ais = {{"ada", {"foreign", "a1"}, "a1"}};
    CHECK(s.hub->useAccounts(r, nullptr).empty());
    CHECK(!s.hub->state().ais().front().canAutoAccount);
    CHECK(!s.hub->apply(s.action(sync::ActionKind::SetAccount, sync::kAutoAccountTarget)));
    r.ais[0].accounts.push_back("a2");
    CHECK(s.hub->useAccounts(r, nullptr).empty());
    for (const auto& a : r.accounts) s.hub->reportSignedIn(a.name, true);
    CHECK(s.hub->state().ais().front().canAutoAccount);
    CHECK(s.hub->apply(s.action(sync::ActionKind::SetAccount, sync::kAutoAccountTarget)));
    CHECK(s.store.find(kAda)->info.account == "a1");
    std::string why;
    CHECK(s.hub->nextSubscription(kAda, {}, why) == "a2");
    CHECK(s.hub->apply(s.action(sync::ActionKind::SetModel, "fable")));
    s.send("finish both sections");

    CHECK(s.serveUntil([&] { return none(s) && s.driver().modelLimitPending; }, 10));
    CHECK(s.driver().continuationPrompt == "finish both sections" && !has(readFile(dir + "/sends"), "send 2:"));
    sync::Sessions mirror;
    mirror.apply_string(s.hub->state().init_diff());
    const auto& offer = mirror.find(kAda)->info;
    CHECK(offer.limitRequest != 0 && offer.account == "a1" && offer.model == "fable");
    CHECK(offer.limitAccount == "a2" && offer.limitModel == "opus");

    auto choice = s.action(sync::ActionKind::ResolveLimit, switchAccount ? "account" : "model");
    choice.account = offer.account;
    choice.request = offer.limitRequest;
    choice.aiName = switchAccount ? offer.limitAccount : offer.limitModel;
    CHECK(s.hub->apply(choice));
    CHECK(s.serveUntil([&] { return none(s) && s.driver().nextTurn == 3; }, 10));

    const auto& info = s.store.find(kAda)->info;
    CHECK(info.account == (switchAccount ? "a2" : "a1") && info.model == (switchAccount ? "fable" : "opus"));
    CHECK(s.pids.size() == 2 && s.sup().model() == info.model);
    CHECK(!s.driver().modelLimitPending && s.driver().continuationPrompt.empty());
    const auto launches = readFile(dir + "/launches");
    CHECK(has(launches, "resume=\naccount=a1\n"));
    if (switchAccount) {
        CHECK(has(launches, "resume=\naccount=a2\n") && !has(launches, "resume=first-fable-session"));
    } else {
        CHECK(has(launches, "resume=first-fable-session\naccount=a1\n"));
    }
    const auto sends = readFile(dir + "/sends");
    CHECK(has(sends, "send 1:\nfinish both sections\n") && has(sends, "send 2:\n[nexus:"));
    CHECK(has(sends, "Tool activity: Write: draft saved") && has(sends, "First section done."));
    CHECK(has(sends, "Do not repeat completed work.]\n\nfinish both sections\n"));
    const auto transcript = s.transcript();
    CHECK(std::count(transcript.begin(), transcript.end(), "P finish both sections") == 1);
    CHECK(std::count(transcript.begin(), transcript.end(), "F Second section done.") == 1);
}

// A session on no account, whose AI's driver takes one: no driver starts, and the session is told how to link an
// account.
void noAccount(Server& s, const std::string& dir, const std::string& fake) {
    setUpAccounts(s, dir, fake, "count-starts " + dir + "/starts\n" + std::string(kHello) + "send\n"
        "emit {\"kind\":\"done\",\"outcome\":\"ok\"}\n");
    CHECK(s.store.find(kAda)->info.account.empty());
    s.send("q");
    const auto told = [&] {
        for (const std::string& line : s.transcript()) {
            if (has(line, "runs on no account, and ada's driver needs one, so the prompt was not sent. Link an "
                          "account to ada in ais.json")) {
                return true;
            }
        }
        return false;
    };
    CHECK(s.serveUntil(told, 10));
    s.serveUntil([] { return false; }, 0.3);
    CHECK(s.drivers->pid(kAda) < 0);
    CHECK(!std::filesystem::exists(dir + "/starts"));
}

// ada taken out of ais.json mid-turn, by a reload: her turn is stopped (the stop line, then its done) and her driver
// ended, the session told once; a prompt queued is kept and nothing starts while she is out, and a prompt sent is
// refused. Put back, the queued prompt goes to a new driver.
void aiRemoved(Server& s, const std::string& dir, const std::string& fake) {
    setUpAccounts(s, dir, fake, "count-starts " + dir + "/starts\n" + std::string(kHello) +
        "send\non-start 1 emit {\"kind\":\"text\",\"delta\":\"Work\"}\non-start 1 await-stop\n"
        "on-start 1 emit {\"kind\":\"text-end\"}\non-start 1 emit {\"kind\":\"done\",\"outcome\":\"stopped\"}\n"
        "on-start 2 emit {\"kind\":\"done\",\"outcome\":\"ok\"}\n");
    s.hub->reportSignedIn("a1", true);
    CHECK(s.hub->apply(s.action(sync::ActionKind::SetAccount, "a1")));
    s.send("go");
    CHECK(s.serveUntil([&] { return s.sup().gathering(); }, 10));
    CHECK(s.hub->apply(s.action(sync::ActionKind::Queue, "next")));
    accounts::Registry withAda = *s.hub->accounts();
    accounts::Registry without = withAda;
    without.ais.clear();
    std::vector<std::string> report;
    CHECK(s.hub->applyAccounts(without, "reload", &report));
    CHECK(s.serveUntil([&] { return none(s); }, 10));
    std::vector<std::string> t = s.transcript();
    // The notice is committed at the reload; the text gathered so far is committed when the stopped turn ends.
    CHECK(t.size() == 4 && t[0] == "P go" && has(t[1], "ada was taken out of ais.json") && t[2] == "F Work" &&
          t[3] == "N Stopped.");
    CHECK(s.store.find(kAda)->queue == std::vector<std::string>{"next"} && s.store.find(kAda)->info.account == "a1");
    s.send("now?");
    CHECK(has(s.transcript().back(), "ada is not in ais.json, so the prompt was not sent"));
    s.serveUntil([] { return false; }, 1.5);    // past kResolveMs: nothing starts
    CHECK(s.drivers->pid(kAda) < 0 && s.activity() == sync::Activity::Down);
    CHECK(s.store.find(kAda)->queue == std::vector<std::string>{"next"});
    CHECK(s.hub->applyAccounts(withAda, "reload", &report));
    CHECK(s.serveUntil([&] { return s.store.find(kAda)->queue.empty() && idle(s) && s.driver().nextTurn == 3; }, 10));
    CHECK(s.transcript().back() == "P next");
}

// ada unlimited, taken out of ais.json, then put back limited: her next driver starts limited, as her record now
// says. While she is out her session reads limited (accounts::Registry::limitedFor); putting her back sets the limit
// from her record again, so the limit she had when she was taken out is never the one she comes back with.
void aiBackLimited(Server& s, const std::string& dir, const std::string& fake) {
    setUpAccounts(s, dir, fake, "record " + dir + "/starts.log\n" + std::string(kHello) +
        "send\nemit {\"kind\":\"done\",\"outcome\":\"ok\"}\n");
    accounts::Registry unlimited = *s.hub->accounts();
    unlimited.ais[0].limit = false;
    std::vector<std::string> report;
    CHECK(s.hub->applyAccounts(unlimited, "reload", &report));
    s.hub->reportSignedIn("a1", true);
    CHECK(s.hub->apply(s.action(sync::ActionKind::SetAccount, "a1")));
    s.send("first");
    CHECK(s.serveUntil([&] { return idle(s) && s.driver().nextTurn == 2; }, 10));
    CHECK(!has(readFile(dir + "/starts.log"), "limit="));      // unlimited: no --limit
    accounts::Registry without = unlimited;
    without.ais.clear();
    CHECK(s.hub->applyAccounts(without, "reload", &report));
    CHECK(s.serveUntil([&] { return none(s); }, 10));
    accounts::Registry limited = unlimited;
    limited.ais[0].limit = true;
    CHECK(s.hub->applyAccounts(limited, "reload", &report));
    s.send("second");
    CHECK(s.serveUntil([&] { return has(readFile(dir + "/starts.log"), "limit=cwd\n"); }, 10));
    CHECK(has(readFile(dir + "/starts.log"), "limit=cwd\n"));
    CHECK(s.serveUntil([&] { return idle(s) || none(s); }, 10));
}

// How many times `prompt` was written to any driver (the fake driver's record-send lines).
int sendsOf(const std::string& path, const std::string& prompt) {
    const std::string text = readFile(path);
    int n = 0;
    for (std::size_t at = text.find(":\n" + prompt + "\n"); at != std::string::npos;
         at = text.find(":\n" + prompt + "\n", at + 1)) {
        ++n;
    }
    return n;
}

// Taken out while its driver starts (the first start never says hello), put back: the waiting prompt goes once.
void aiRemovedStarting(Server& s, const std::string& dir, const std::string& fake) {
    const std::string hello = kHello;
    std::string second;     // the hello, said by the second start only
    std::size_t from = 0;
    while (from < hello.size()) {
        const std::size_t nl = hello.find('\n', from);
        second += "on-start 2 " + hello.substr(from, nl - from + 1);
        from = nl + 1;
    }
    setUpAccounts(s, dir, fake, "count-starts " + dir + "/starts\non-start 1 hang\n" + second + "send\nrecord-send " +
        dir + "/sends\nemit {\"kind\":\"done\",\"outcome\":\"ok\"}\n");
    s.hub->reportSignedIn("a1", true);
    CHECK(s.hub->apply(s.action(sync::ActionKind::SetAccount, "a1")));
    s.send("p1");
    CHECK(s.serveUntil([&] { return s.sup().phase() == Phase::Starting; }, 10));
    const accounts::Registry with = *s.hub->accounts();
    accounts::Registry without = with;
    without.ais.clear();
    std::vector<std::string> report;
    CHECK(s.hub->applyAccounts(without, "reload", &report));
    CHECK(s.serveUntil([&] { return none(s); }, 10));
    s.serveUntil([] { return false; }, 1.5);
    CHECK(none(s));
    CHECK(s.hub->applyAccounts(with, "reload", &report));
    CHECK(s.serveUntil([&] { return idle(s) && s.driver().nextTurn == 2; }, 10));
    CHECK(sendsOf(dir + "/sends", "p1") == 1);
}

// Taken out mid-turn and put back by the next reload before the stopped turn's done is read; then a prompt queued
// before and one queued while out each go once.
void aiBackBeforeStop(Server& s, const std::string& dir, const std::string& fake) {
    const std::string record = "send\nrecord-send " + dir + "/sends\n";
    setUpAccounts(s, dir, fake, std::string(kHello) + record + "emit {\"kind\":\"text\",\"delta\":\"Work\"}\n"
        "await-stop\nemit {\"kind\":\"text-end\"}\nemit {\"kind\":\"done\",\"outcome\":\"stopped\"}\n" + record +
        "emit {\"kind\":\"done\",\"outcome\":\"ok\"}\n" + record + "emit {\"kind\":\"done\",\"outcome\":\"ok\"}\n");
    s.hub->reportSignedIn("a1", true);
    CHECK(s.hub->apply(s.action(sync::ActionKind::SetAccount, "a1")));
    s.send("go");
    CHECK(s.serveUntil([&] { return s.sup().gathering(); }, 10));
    CHECK(s.hub->apply(s.action(sync::ActionKind::Queue, "next")));
    const accounts::Registry with = *s.hub->accounts();
    accounts::Registry without = with;
    without.ais.clear();
    std::vector<std::string> report;
    CHECK(s.hub->applyAccounts(without, "reload", &report));
    CHECK(s.hub->apply(s.action(sync::ActionKind::Queue, "late")));
    CHECK(s.hub->applyAccounts(with, "reload", &report));
    CHECK(s.serveUntil([&] { return s.store.find(kAda)->queue.empty() && idle(s) && s.driver().nextTurn == 4; }, 10));
    CHECK(sendsOf(dir + "/sends", "go") == 1);
    CHECK(sendsOf(dir + "/sends", "next") == 1 && sendsOf(dir + "/sends", "late") == 1);
}

// Unlimited, a turn open and a prompt queued; taken out; a prompt queued while out; put back limited in one reload: the
// queued prompts go once each, and the driver they go to started limited.
void aiBackLimitedQueue(Server& s, const std::string& dir, const std::string& fake) {
    const std::string record = "send\nrecord-send " + dir + "/sends\n";
    setUpAccounts(s, dir, fake, "count-starts " + dir + "/starts\nrecord " + dir + "/starts.log\n" +
        std::string(kHello) + record + "on-start 1 emit {\"kind\":\"text\",\"delta\":\"W\"}\non-start 1 await-stop\n"
        "on-start 1 emit {\"kind\":\"text-end\"}\non-start 1 emit {\"kind\":\"done\",\"outcome\":\"stopped\"}\n"
        "on-start 2 emit {\"kind\":\"done\",\"outcome\":\"ok\"}\n" + record +
        "emit {\"kind\":\"done\",\"outcome\":\"ok\"}\n" + record + "emit {\"kind\":\"done\",\"outcome\":\"ok\"}\n");
    accounts::Registry unlimited = *s.hub->accounts();
    unlimited.ais[0].limit = false;
    std::vector<std::string> report;
    CHECK(s.hub->applyAccounts(unlimited, "reload", &report));
    s.hub->reportSignedIn("a1", true);
    CHECK(s.hub->apply(s.action(sync::ActionKind::SetAccount, "a1")));
    s.send("go");
    CHECK(s.serveUntil([&] { return s.sup().gathering(); }, 10));
    CHECK(s.hub->apply(s.action(sync::ActionKind::Queue, "next")));
    accounts::Registry without = unlimited;
    without.ais.clear();
    CHECK(s.hub->applyAccounts(without, "reload", &report));
    CHECK(s.serveUntil([&] { return none(s); }, 10));
    CHECK(s.hub->apply(s.action(sync::ActionKind::Queue, "late")));
    accounts::Registry limited = unlimited;
    limited.ais[0].limit = true;
    CHECK(s.hub->applyAccounts(limited, "reload", &report));
    CHECK(s.serveUntil([&] { return s.store.find(kAda)->queue.empty() && idle(s) && s.driver().nextTurn == 4; }, 10));
    CHECK(sendsOf(dir + "/sends", "next") == 1 && sendsOf(dir + "/sends", "late") == 1);
    const std::string starts = readFile(dir + "/starts.log");
    const std::size_t second = starts.find("cwd=", starts.find("cwd=") + 1);
    CHECK(second != std::string::npos && starts.find("limit=cwd", second) != std::string::npos);
}

// Limited on an instance in dir/a, a turn open and a prompt queued; taken out; another prompt queued while out; put
// back with the same instance name on another machine (nothing starts here, prompts kept), then taken out and put back
// with that instance in dir/b: the queued prompts go once each, to a limited driver started afresh in dir/b (never in
// dir/a or the drivers file's directory).
void aiBackMoved(Server& s, const std::string& dir, const std::string& fake) {
    std::filesystem::create_directories(dir + "/a");
    std::filesystem::create_directories(dir + "/b");
    const std::string record = "send\nrecord-send " + dir + "/sends\n";
    writeFile(dir + "/ada.script", "count-starts " + dir + "/starts\nrecord " + dir + "/starts.log\n" +
        std::string(kHello) + record + "on-start 1 emit {\"kind\":\"session\",\"id\":\"s-1\"}\n"
        "on-start 1 emit {\"kind\":\"text\",\"delta\":\"W\"}\non-start 1 await-stop\n"
        "on-start 1 emit {\"kind\":\"text-end\"}\non-start 1 emit {\"kind\":\"done\",\"outcome\":\"stopped\"}\n"
        "on-start 2 emit {\"kind\":\"done\",\"outcome\":\"ok\"}\n" + record +
        "emit {\"kind\":\"done\",\"outcome\":\"ok\"}\n");
    writeFile(dir + "/drivers", "driver fake " + fake + " --script " + dir + "/ada.script --account from-the-file\n"
        "AI ada fake " + dir + "\n");
    accounts::Registry inA;
    inA.accounts = {{"a1", "fake"}};
    inA.ais = {{"ada", {"a1"}, "a1", {{"here", kHost, dir + "/a"}}, true}};
    CHECK(!s.hub->useAccounts(inA, nullptr).empty());
    s.hub->reportSignedIn("a1", true);
    CHECK(s.hub->adoptDefaults().size() == 1);
    s.send("go");
    CHECK(s.serveUntil([&] { return s.sup().gathering(); }, 10));
    CHECK(s.hub->apply(s.action(sync::ActionKind::Queue, "q1")));
    accounts::Registry without = inA;
    without.ais.clear();
    std::vector<std::string> report;
    CHECK(s.hub->applyAccounts(without, "remove", &report));
    CHECK(s.serveUntil([&] { return none(s); }, 10));
    CHECK(s.hub->apply(s.action(sync::ActionKind::Queue, "q2")));
    // Put back on another machine: nothing starts here, and the queue stays as it is.
    accounts::Registry elsewhere = inA;
    elsewhere.ais = {{"ada", {"a1"}, "a1", {{"here", "macbook", dir + "/b"}}, true}};
    CHECK(s.hub->applyAccounts(elsewhere, "elsewhere", &report));
    s.serveUntil([] { return false; }, 0.3);
    CHECK(s.drivers->pid(kAda) < 0 && readFile(dir + "/starts") == "1\n" && s.activity() == sync::Activity::Down);
    CHECK((s.store.find(kAda)->queue == std::vector<std::string>{"q1", "q2"}));
    s.send("remote");
    CHECK(has(s.transcript().back(), "on the machine macbook, and this server runs on testhost"));
    // Taken out and put back on this machine in dir/b: the driver starts limited in dir/b, afresh with a summary.
    CHECK(s.hub->applyAccounts(without, "remove again", &report));
    accounts::Registry inB = inA;
    inB.ais = {{"ada", {"a1"}, "a1", {{"here", kHost, dir + "/b"}}, true}};
    CHECK(s.hub->applyAccounts(inB, "in b", &report));
    CHECK(s.serveUntil([&] { return s.store.find(kAda)->queue.empty() && idle(s) && s.driver().nextTurn == 4; }, 10));
    CHECK(readFile(dir + "/starts") == "2\n");
    const std::string starts = readFile(dir + "/starts.log");
    const std::size_t second = starts.find("cwd=", starts.find("cwd=") + 1);
    CHECK(second != std::string::npos);
    if (second != std::string::npos) {
        const std::string secondStart = starts.substr(second);
        CHECK(has(secondStart, "cwd=" + dir + "/b\ndir=" + dir + "/b\nresume=\n"));
        CHECK(has(secondStart, "limit=cwd\n"));
        CHECK(!has(secondStart, "cwd=" + dir + "/a\n"));
    }
    const std::string sends = readFile(dir + "/sends");
    CHECK(has(sends, "[nexus: the person's prompt follows.]\n\nq1\n") && sendsOf(dir + "/sends", "q2") == 1);
    CHECK(sendsOf(dir + "/sends", "remote") == 0);
}

// On account a1, a turn open and a prompt queued; taken out, then put back with a1 unlinked and signed-in a2 in its
// place: the queued prompt starts its driver on a2, never on a1. Taken out again with a prompt queued and put back on
// no account: nothing starts while the driver needs an account, and setting the session to a2 once linked lets it go.
void aiBackAccount(Server& s, const std::string& dir, const std::string& fake) {
    const std::string record = "send\nrecord-send " + dir + "/sends\n";
    setUpAccounts(s, dir, fake, "count-starts " + dir + "/starts\nrecord " + dir + "/starts.log\n" +
        std::string(kHello) + record + "on-start 1 emit {\"kind\":\"text\",\"delta\":\"W\"}\non-start 1 await-stop\n"
        "on-start 1 emit {\"kind\":\"text-end\"}\non-start 1 emit {\"kind\":\"done\",\"outcome\":\"stopped\"}\n"
        "on-start 2 emit {\"kind\":\"done\",\"outcome\":\"ok\"}\n"
        "on-start 3 emit {\"kind\":\"done\",\"outcome\":\"ok\"}\n" + record +
        "on-start 2 emit {\"kind\":\"text\",\"delta\":\"W2\"}\non-start 2 await-stop\n"
        "on-start 2 emit {\"kind\":\"text-end\"}\non-start 2 emit {\"kind\":\"done\",\"outcome\":\"stopped\"}\n" +
        record + "emit {\"kind\":\"done\",\"outcome\":\"ok\"}\n");
    s.hub->reportSignedIn("a1", true);
    s.hub->reportSignedIn("a2", true);
    CHECK(s.hub->apply(s.action(sync::ActionKind::SetAccount, "a1")));
    s.send("go");
    CHECK(s.serveUntil([&] { return s.sup().gathering(); }, 10));
    CHECK(s.hub->apply(s.action(sync::ActionKind::Queue, "q1")));
    accounts::Registry without = *s.hub->accounts();
    without.ais.clear();
    std::vector<std::string> report;
    CHECK(s.hub->applyAccounts(without, "remove", &report));
    CHECK(s.serveUntil([&] { return none(s); }, 10));
    accounts::Registry onA2 = without;
    onA2.ais = {{"ada", {"a2"}, "a2"}};
    CHECK(s.hub->applyAccounts(onA2, "back on a2", &report));
    CHECK(s.serveUntil([&] { return s.store.find(kAda)->queue.empty() && idle(s) && s.driver().nextTurn == 3; }, 10));
    CHECK(readFile(dir + "/starts") == "2\n");
    std::string starts = readFile(dir + "/starts.log");
    const std::size_t second = starts.find("cwd=", starts.find("cwd=") + 1);
    CHECK(second != std::string::npos);
    if (second != std::string::npos) {
        const std::string secondStart = starts.substr(second);
        CHECK(has(secondStart, "account=a2\n") && !has(secondStart, "account=a1\n"));
    }
    // A second removal, then put back with no linked account: nothing starts on a2, and q2 waits until a2 is chosen.
    s.send("go2");
    CHECK(s.serveUntil([&] { return s.sup().gathering(); }, 10));
    CHECK(s.hub->apply(s.action(sync::ActionKind::Queue, "q2")));
    CHECK(s.hub->applyAccounts(without, "remove again", &report));
    CHECK(s.serveUntil([&] { return none(s); }, 10));
    accounts::Registry unlinked = without;
    unlinked.ais = {{"ada", {}, ""}};
    CHECK(s.hub->applyAccounts(unlinked, "back with no account", &report));
    s.serveUntil([] { return false; }, 0.3);
    CHECK(s.drivers->pid(kAda) < 0 && readFile(dir + "/starts") == "2\n" && s.activity() == sync::Activity::Down);
    CHECK(s.store.find(kAda)->queue == std::vector<std::string>{"q2"});
    CHECK(s.hub->applyAccounts(onA2, "link a2", &report));
    CHECK(s.hub->apply(s.action(sync::ActionKind::SetAccount, "a2")));
    CHECK(s.serveUntil([&] { return s.store.find(kAda)->queue.empty() && idle(s) && s.driver().nextTurn == 5; }, 10));
    CHECK(readFile(dir + "/starts") == "3\n" && sendsOf(dir + "/sends", "q2") == 1);
    starts = readFile(dir + "/starts.log");
    const std::size_t third = starts.find("cwd=", second != std::string::npos ? second + 1 : 0);
    CHECK(third != std::string::npos && has(starts.substr(third), "account=a2\n"));
}

// ada with one instance, `instance`, and her session on it (as the first start after instances puts it): the
// drivers file's line, the account a1.
void setUpInstance(Server& s, const std::string& dir, const std::string& fake, const std::string& script,
                   const accounts::Instance& instance) {
    writeFile(dir + "/ada.script", script);
    writeFile(dir + "/drivers", "driver fake " + fake + " --script " + dir + "/ada.script\nai ada fake " + dir +
        "\n");
    accounts::Registry r;
    r.accounts = {{"a1", "fake"}};
    r.ais = {{"ada", {"a1"}, "a1", {instance}}};
    CHECK(has(s.hub->useAccounts(r, nullptr), "session 1 (ada): instance none -> " + instance.name));
    CHECK(s.hub->adoptDefaults().size() == 1);
    CHECK(s.store.find(kAda)->info.instance == instance.name && s.store.find(kAda)->info.account == "a1");
}

// ada's registry as setUpInstance made it, with her one instance replaced.
accounts::Registry withInstance(const accounts::Instance& instance) {
    accounts::Registry r;
    r.accounts = {{"a1", "fake"}};
    r.ais = {{"ada", {"a1"}, "a1", {instance}}};
    return r;
}

// Whether ada's transcript has a line holding part.
bool told(Server& s, const std::string& part) {
    for (const std::string& line : s.transcript()) {
        if (has(line, part)) {
            return true;
        }
    }
    return false;
}

// The instance's directory edited while its driver runs: the session is told, the idle driver is ended, and the next
// prompt starts one in the new directory, afresh (its harness session was made in the old one). Then the instance
// moved to another machine: the driver is ended, and none starts here.
void instanceMoved(Server& s, const std::string& dir, const std::string& fake) {
    std::filesystem::create_directories(dir + "/a");
    std::filesystem::create_directories(dir + "/b");
    setUpInstance(s, dir, fake, "count-starts " + dir + "/starts\nrecord " + dir + "/starts.log\n" + kHello +
        "send\nrecord-send " + dir + "/sends\nemit {\"kind\":\"session\",\"id\":\"s-1\"}\n"
        "emit {\"kind\":\"done\",\"outcome\":\"ok\"}\n", accounts::Instance{"here", "testhost", dir + "/a"});
    s.send("q");
    CHECK(s.serveUntil([&] { return idle(s) && s.driver().nextTurn == 2 && s.driver().session == "s-1"; }, 10));
    CHECK(s.driver().sessionDir == dir + "/a");
    const pid_t first = s.drivers->pid(kAda);
    CHECK(first > 0);
    std::vector<std::string> report;
    CHECK(s.hub->applyAccounts(withInstance({"here", "testhost", dir + "/b"}), "moved", &report));
    CHECK(has(report, "session 1 (ada): instance here moved, on testhost in " + dir + "/a -> on testhost in " + dir +
              "/b"));
    CHECK(told(s, "ada's instance here moved (the accounts and AIs were changed: moved)"));
    CHECK(s.serveUntil([&] { return s.drivers->pid(kAda) < 0; }, 10));
    s.send("r");
    CHECK(s.serveUntil([&] { return idle(s) && s.driver().nextTurn == 3; }, 10));
    const std::string log = readFile(dir + "/starts.log");
    CHECK(has(log, "cwd=" + dir + "/a\ndir=" + dir + "/a\nresume=\n"));
    CHECK(has(log, "cwd=" + dir + "/b\ndir=" + dir + "/b\nresume=\n"));
    CHECK(told(s, "ada now works in " + dir + "/b, and the conversation so far was held in " + dir + "/a"));
    CHECK(s.driver().sessionDir == dir + "/b" && !s.driver().summary);
    CHECK(readFile(dir + "/starts") == "2\n");
    // The first send there carries the summary of the turns before its prompt, then the prompt, once.
    const std::string sends = readFile(dir + "/sends");
    CHECK(has(sends, "send 1:\nq\nsend 2:\n[nexus: this conversation with ada continues in a fresh harness session"));
    CHECK(has(sends, "The person: q\n") && !has(sends, "The person: r\n"));
    CHECK(has(sends, "[nexus: the person's prompt follows.]\n\nr\n"));
    // Its machine edited to another: the driver is ended, and the next prompt is not sent.
    CHECK(s.hub->applyAccounts(withInstance({"here", "macbook", dir + "/b"}), "moved away", &report));
    CHECK(s.serveUntil([&] { return s.drivers->pid(kAda) < 0; }, 10));
    s.send("t");
    CHECK(s.serveUntil([&] { return told(s, "on the machine macbook, and this server runs on testhost"); }, 10));
    s.serveUntil([] { return false; }, 0.3);
    CHECK(s.drivers->pid(kAda) < 0 && readFile(dir + "/starts") == "2\n");
}

// The instance moved to another machine while its driver was known here, then every instance removed: the session is
// on no instance, so it works here, in the drivers file's directory, and the next prompt starts a driver there (it
// never stays down until a restart).
void instanceRemoved(Server& s, const std::string& dir, const std::string& fake) {
    std::filesystem::create_directories(dir + "/a");
    setUpInstance(s, dir, fake, "count-starts " + dir + "/starts\nrecord " + dir + "/starts.log\n" + kHello +
        "send\nemit {\"kind\":\"done\",\"outcome\":\"ok\"}\n", accounts::Instance{"here", "testhost", dir + "/a"});
    s.send("q");
    CHECK(s.serveUntil([&] { return idle(s) && s.driver().nextTurn == 2; }, 10));
    std::vector<std::string> report;
    CHECK(s.hub->applyAccounts(withInstance({"here", "macbook", dir + "/a"}), "moved away", &report));
    CHECK(s.serveUntil([&] { return s.drivers->pid(kAda) < 0; }, 10));
    CHECK(s.serveUntil([&] { return s.activity() == sync::Activity::Down; }, 10));
    accounts::Registry none;
    none.accounts = {{"a1", "fake"}};
    none.ais = {{"ada", {"a1"}, "a1", {}}};
    CHECK(s.hub->applyAccounts(none, "no instances", &report));
    CHECK(s.store.find(kAda)->info.instance.empty());
    CHECK(s.serveUntil([&] { return s.activity() == sync::Activity::Idle; }, 10));
    s.send("r");
    CHECK(s.serveUntil([&] { return idle(s) && s.driver().nextTurn == 3; }, 10));
    CHECK(readFile(dir + "/starts") == "2\n");
    CHECK(has(readFile(dir + "/starts.log"), "cwd=" + dir + "\n"));
}

// An inbound message follows the default instance, even when only another instance has a session.
void busDefaultInstance(Server& s, const std::string& dir, const std::string& fake) {
    const accounts::Instance first{"first", kHost, dir + "/first"};
    const accounts::Instance second{"second", kHost, dir + "/second"};
    std::filesystem::create_directories(first.dir);
    std::filesystem::create_directories(second.dir);
    setUpInstance(s, dir, fake, std::string(kHello) + "send\nrecord-send " + dir +
        "/sends\nemit {\"kind\":\"done\",\"outcome\":\"ok\"}\n", first);
    auto roster = withInstance(second);
    roster.ais.front().instances.push_back(first);
    CHECK(s.hub->applyAccounts(roster, "change default", nullptr));
    s.hub->reportSignedIn("a1", true);
    CHECK(s.store.size() == 1 && s.store.find(kAda)->info.instance == first.name);
    std::string why;
    CHECK(s.hub->receiveBus("ada", "100-0", "bea", "second workspace", "second workspace", &why));
    const auto next = s.store.find(2);
    CHECK(next && next->info.instance == second.name);
    if (!next) return;
    CHECK(s.serveUntil([&] { return next->driver.nextTurn == 2 && next->driver.openTurn == 0; }, 10));
    CHECK(s.store.find(kAda)->driver.nextTurn == 1);

    std::swap(roster.ais.front().instances[0], roster.ais.front().instances[1]);
    CHECK(s.hub->applyAccounts(roster, "restore default", nullptr));
    CHECK(s.hub->receiveBus("ada", "100-0", "bea", "duplicate", "duplicate", &why));
    CHECK(s.hub->receiveBus("ada", "101-0", "bea", "first workspace", "first workspace", &why));
    CHECK(s.serveUntil([&] { return s.driver().nextTurn == 2 && !s.driver().openTurn; }, 10));
    CHECK(next->driver.nextTurn == 2 && s.store.size() == 2);
    CHECK(readFile(dir + "/sends") == "send 1:\nsecond workspace\nsend 1:\nfirst workspace\n");
}

// A session that names no instance, while its AI has some (as a store that failed before the session was
// settled leaves it): its driver works in the AI's default instance's directory, never the drivers file's.
void instanceDefault(Server& s, const std::string& dir, const std::string& fake) {
    std::filesystem::create_directories(dir + "/checkout");
    setUpInstance(s, dir, fake, "record " + dir + "/starts.log\n" + kHello + "send\n"
        "emit {\"kind\":\"done\",\"outcome\":\"ok\"}\n", accounts::Instance{"here", "testhost", dir + "/checkout"});
    sync::SessionInfo info = s.store.find(kAda)->info;
    info.instance.clear();
    std::string why;
    CHECK(s.store.commit(kAda, {sync::infoChange(info)}, std::string(), 0, &why));
    s.send("q");
    CHECK(s.serveUntil([&] { return idle(s) && s.driver().nextTurn == 2; }, 10));
    CHECK(has(readFile(dir + "/starts.log"), "cwd=" + dir + "/checkout\n"));
}

// A session's instance on this machine: its driver works in the instance's directory, not the drivers file's.
void instanceDir(Server& s, const std::string& dir, const std::string& fake) {
    std::filesystem::create_directories(dir + "/checkout");
    setUpInstance(s, dir, fake, "record " + dir + "/starts.log\n" + kHello + "send\n"
        "emit {\"kind\":\"done\",\"outcome\":\"ok\"}\n", accounts::Instance{"here", "testhost", dir + "/checkout"});
    s.send("q");
    CHECK(s.serveUntil([&] { return idle(s) && s.driver().nextTurn == 2; }, 10));
    CHECK(has(readFile(dir + "/starts.log"), "cwd=" + dir + "/checkout\n"));
}

// A session's instance on another machine: no driver starts here yet, and the session is told so.
void instanceElsewhere(Server& s, const std::string& dir, const std::string& fake) {
    setUpInstance(s, dir, fake, "count-starts " + dir + "/starts\n" + std::string(kHello) + "send\n"
        "emit {\"kind\":\"done\",\"outcome\":\"ok\"}\n", accounts::Instance{"laptop", "macbook", "/Users/me"});
    s.send("q");
    const auto told = [&] {
        for (const std::string& line : s.transcript()) {
            if (has(line, "belongs to ada's instance laptop, on the machine macbook, and this server runs on testhost; "
                          "nexus-server does not yet start a driver on another machine")) {
                return true;
            }
        }
        return false;
    };
    CHECK(s.serveUntil(told, 10));
    s.serveUntil([] { return false; }, 0.3);
    CHECK(s.drivers->pid(kAda) < 0);
    CHECK(!std::filesystem::exists(dir + "/starts"));
}

// ada's driver as a shell script, for what the fake driver does not do: start processes of its own.
void setUpShell(const std::string& dir, const std::string& script) {
    writeFile(dir + "/ada.sh", script);
    writeFile(dir + "/drivers", "driver sh /bin/sh " + dir + "/ada.sh\nai ada sh " + dir + "\n");
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

const char* const kShellHello = "printf '%s\\n' '{\"kind\":\"hello\",\"protocol\":1,\"harness\":\"sh\",\"features\":[\"limit\"]}'\n";

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

// The value the secret cases paste: made up here, never a real one.
const std::string kFakeToken = "fakeDf7rKsVm" + std::string(64, 'q');

// ada's drivers file and script for a driver of the claude harness (the fake stands in), and the accounts: a1, a claude
// account (its login the file oauth-token) and ada's default, which her session runs on.
void setUpClaude(Server& s, const std::string& dir, const std::string& fake, const std::string& script) {
    writeFile(dir + "/ada.script", script);
    writeFile(dir + "/drivers", "driver claude " + fake + " --script " + dir + "/ada.script --account from-the-file\n"
        "AI ada claude " + dir + "\n");
    accounts::Registry r;
    r.accounts = {{"a1", "claude"}};
    r.ais = {{"ada", {"a1"}, "a1"}};
    CHECK(s.hub->useAccounts(r, nullptr).empty());
    CHECK(s.hub->apply(s.action(sync::ActionKind::SetAccount, "a1")));
}

// A window's answer to request `id`: the person pasted the fake value and pressed Enter.
bool paste(Server& s, std::uint64_t id, std::uint64_t num) {
    sync::Action a;
    a.num = num;
    a.kind = sync::ActionKind::SecretAnswer;
    a.request = id;
    a.secret = sync::SecretValue(kFakeToken);
    std::string why;
    const bool applied = s.hub->apply(a, &why, "w1");
    if (!applied) {
        std::fprintf(stderr, "  the answer was not applied: %s\n", why.c_str());
    }
    return applied;
}

// The value's file: exactly the value, 0600, in a 0700 directory.
void kept(const std::string& dir) {
    const std::string path = dir + "/accounts/a1/oauth-token";
    CHECK(readFile(path) == kFakeToken);
    namespace fs = std::filesystem;
    std::error_code ec;
    CHECK(fs::status(path, ec).permissions() == (fs::perms::owner_read | fs::perms::owner_write));
    CHECK(fs::status(dir + "/accounts/a1", ec).permissions() == fs::perms::owner_all);
}

// The environment this test process was started with, taken once at the start of main, before any server code runs:
// what came from the host (a CI runner's hashes and paths, where a length beside the short account name a1 can occur
// by chance). Never read again later, so that anything the server's code puts in the process's environment (which
// every driver inherits) is not taken as the host's.
std::set<std::string>& startEnvironment() {
    static std::set<std::string> lines;
    return lines;
}

void snapshotEnvironment() {
    for (char** e = environ; e != nullptr && *e != nullptr; ++e) {
        startEnvironment().insert(*e);
    }
}

// `text` (the driver's recorded environment, one `NAME=value` a line) without the lines of the start environment.
std::string withoutInherited(const std::string& text) {
    std::istringstream in(text);
    std::string out;
    for (std::string line; std::getline(in, line);) {
        if (startEnvironment().count(line) == 0) {
            out += line + "\n";
        }
    }
    return out;
}

// The value is nowhere but its file: no run, hex, base64 or length of it (tests/leak_scan.h) in any other file under
// `dir` (the store's files, what the driver read, its environment less what this process inherited, the script, the
// drivers file), the transcript, or the bytes the server's SecretLines gave the driver and the log.
// With `expectLeak` (the reversed witness, secret_leak_witness), the scan must find the value in the driver's recorded
// environment instead, and the other places are not checked.
void nowhereElse(Server& s, const std::string& dir, bool expectLeak = false) {
    const test::LeakScan scan(kFakeToken, "a1");
    namespace fs = std::filesystem;
    const fs::path file = fs::path(dir) / "accounts" / "a1" / "oauth-token";
    int read = 0;
    int leaks = 0;
    std::error_code ec;
    for (const auto& entry : fs::recursive_directory_iterator(dir, ec)) {
        if (!entry.is_regular_file() || entry.path() == file) {
            continue;
        }
        ++read;
        const std::string bytes = readFile(entry.path().string());
        const std::string found = scan.find(entry.path().filename() == "env" ? withoutInherited(bytes) : bytes);
        if (!found.empty()) {
            ++leaks;
            if (!expectLeak) {
                std::fprintf(stderr, "  the value reached %s: %s\n", entry.path().string().c_str(), found.c_str());
                CHECK(false);
            }
        }
    }
    CHECK(read > 3);
    if (expectLeak) {
        if (leaks == 0) {
            std::fprintf(stderr, "  the injected leak into the server's environment was not found by the scan\n");
        }
        CHECK(leaks > 0);
        return;
    }
    for (const std::string& line : s.transcript()) {
        CHECK(!scan.in(line));
    }
    for (const std::string& line : s.log) {
        CHECK(!scan.in(line));
    }
}

// A driver's ask for a secret, as docs/secrets.md defines it: with no window that can take one it is refused at once
// (no-client); with one, a request opens for the window, a pasted value goes to its file, and the driver hears only
// "stored". The value never reaches the driver, the store, the transcript or the log.
void secretAsk(Server& s, const std::string& dir, const std::string& fake, bool expectLeak = false) {
    const std::string heard = dir + "/heard";
    const auto ask = [](int n) {
        return "raw {\"kind\":\"ask-secret\",\"ask\":" + std::to_string(n) +
            ",\"account\":\"a1\",\"what\":\"oauth-token\",\"why\":\"the login expired\"}\n";
    };
    // The driver's whole environment goes to a file under `dir`, which nowhereElse() searches with the rest.
    setUpClaude(s, dir, fake, "option echo-input\nrecord-env " + dir + "/env\n" + std::string(kHello) + "send\n" +
        ask(1) + "await-secret " + heard + "\n"
        "emit {\"kind\":\"done\",\"outcome\":\"ok\"}\nsend\n" + ask(2) + "await-secret " + heard + "\n"
        "emit {\"kind\":\"done\",\"outcome\":\"ok\"}\n");
    s.send("first");
    CHECK(s.serveUntil([&] { return idle(s) && s.driver().nextTurn == 2; }, 10));
    CHECK(has(readFile(heard), "\"kind\":\"secret\""));
    CHECK(has(readFile(heard), "\"no-client\""));
    CHECK(s.hub->state().requests().empty());
    // A window that takes requests is connected: the ask opens one, offered to it, while the driver's turn waits.
    s.takers = true;
    s.send("second");
    CHECK(s.serveUntil([&] { return !s.hub->state().requests().empty(); }, 10));
    CHECK(s.hub->state().requests().size() == 1);
    if (s.hub->state().requests().size() == 1) {
        const sync::SecretRequest r = s.hub->state().requests()[0];
        CHECK(r.account == "a1" && r.what == "oauth-token" && r.asker == kAda && r.askerName == "ada" &&
              r.why == "the login expired");
        CHECK(s.driver().openTurn == 2);
        CHECK(paste(s, r.id, 1));
    }
    CHECK(s.serveUntil([&] { return idle(s) && s.driver().nextTurn == 3; }, 10));
    CHECK(has(readFile(heard), "\"stored\""));
    CHECK(s.hub->state().requests().empty());
    CHECK(has(readFile(dir + "/env"), "="));
    CHECK(signedIn(s, "a1"));
    kept(dir);
    nowhereElse(s, dir, expectLeak);
}

// The reversed witness of the scan: the value put into this process's environment after the start snapshot, as a
// server bug that set it in its own environment would (every driver inherits it, src/server/drivers.cpp), must be
// found in the driver's recorded environment. Test-only: nothing in the server does this.
void secretLeakWitness(Server& s, const std::string& dir, const std::string& fake) {
    ::setenv("NEXUS_TEST_INJECTED_LEAK", kFakeToken.c_str(), 1);
    secretAsk(s, dir, fake, true);
    ::unsetenv("NEXUS_TEST_INJECTED_LEAK");
}

// An account with no login: its driver says so (no-credential), nexus itself asks the window for it, and the pasted
// value signs the account in. The first prompt's turn is cut with the driver (protocol rule 11); the second, waiting
// while starting is held, is answered by a driver started for it, without a restart of nexus-server and without
// another prompt (Supervisor.tla SignIn).
void secretSignIn(Server& s, const std::string& dir, const std::string& fake) {
    setUpClaude(s, dir, fake, "option echo-input\nrecord-env " + dir + "/env\ncount-starts " + dir + "/starts\n"
        "emit {\"kind\":\"hello\",\"protocol\":1,\"harness\":\"fake\",\"features\":[\"limit\"]}\n"
        "on-start 1 emit {\"kind\":\"error\",\"cause\":\"a1 has no token\",\"fix\":\"paste one\",\"code\":\"no-credential\"}\n"
        "on-start 1 exit 0\n"
        "on-start 2 emit {\"kind\":\"credential\",\"billing\":\"subscription\",\"plan\":\"max\"}\n"
        "send\nemit {\"kind\":\"text\",\"delta\":\"signed in\"}\nemit {\"kind\":\"text-end\"}\n"
        "emit {\"kind\":\"done\",\"outcome\":\"ok\"}\n");
    s.takers = true;
    s.send("q");
    s.send("r");
    CHECK(s.serveUntil([&] { return !s.hub->state().requests().empty(); }, 10));
    CHECK(s.hub->state().requests().size() == 1);
    CHECK(!signedIn(s, "a1"));
    if (s.hub->state().requests().size() == 1) {
        const sync::SecretRequest r = s.hub->state().requests()[0];
        CHECK(r.account == "a1" && r.what == "oauth-token" && r.asker == 0 && r.askerName.empty());
        CHECK(s.serveUntil([&] { return none(s); }, 10));
        // held: no driver starts for the prompt waiting, however long the loop runs
        s.serveUntil([] { return false; }, 0.3);
        CHECK(none(s) && s.sup().held() && s.driver().waiting == std::vector<std::string>{"r"});
        CHECK(readFile(dir + "/starts") == "1\n");
        CHECK(paste(s, r.id, 1));
    }
    CHECK(s.serveUntil([&] { return idle(s) && s.driver().nextTurn == 3; }, 10));
    CHECK(readFile(dir + "/starts") == "2\n");
    CHECK(signedIn(s, "a1"));
    CHECK(has(s.transcript(), "F signed in"));
    kept(dir);
    nowhereElse(s, dir);
}

// ada taken out of ais.json with a prompt queued; the server restarts holding no registry (its committed files do
// not read: Hub::keepWith). Nothing is sent, no driver starts and the prompts are kept (models/Supervisor.tla
// NoSendUnknown). The first registry applied says, from itself alone, whether ada is in it: without her
// (`listedFirst` false), nothing goes, and only a later one with her lets the queue go on, each prompt once; with her,
// at once.
void noRegistry(const std::string& dir, const std::string& fake, bool listedFirst) {
    const std::string block = "send\nrecord-send " + dir + "/sends\nemit {\"kind\":\"done\",\"outcome\":\"ok\"}\n";
    accounts::Registry with;
    {
        Server s(dir);
        setUpAccounts(s, dir, fake, std::string(kHello) + block + block + block);
        s.hub->reportSignedIn("a1", true);
        CHECK(s.hub->apply(s.action(sync::ActionKind::SetAccount, "a1")));
        s.send("go");
        CHECK(s.serveUntil([&] { return idle(s) && s.driver().nextTurn == 2; }, 10));
        with = *s.hub->accounts();
        accounts::Registry without = with;
        without.ais.clear();
        std::vector<std::string> report;
        CHECK(s.hub->applyAccounts(without, "reload", &report));
        CHECK(s.serveUntil([&] { return none(s); }, 10));
        CHECK(s.hub->apply(s.action(sync::ActionKind::Queue, "q1")));
        s.end();
    }
    Server s(dir, false, false);    // no registry: the committed files did not read
    CHECK(s.hub->accounts() == nullptr);
    s.serveUntil([] { return false; }, 1.5);
    CHECK(sendsOf(dir + "/sends", "q1") == 0);
    CHECK(s.store.find(kAda)->queue == std::vector<std::string>{"q1"});
    CHECK(s.drivers->pid(kAda) < 0);
    CHECK(s.activity() == sync::Activity::Down);
    // A prompt sent now is refused, saying why; one queued is kept.
    s.send("now");
    CHECK(has(s.transcript().back(), "holds no AIs list"));
    CHECK(s.hub->apply(s.action(sync::ActionKind::Queue, "q2")));
    s.serveUntil([] { return false; }, 0.5);
    CHECK(sendsOf(dir + "/sends", "now") == 0 && sendsOf(dir + "/sends", "q2") == 0);
    CHECK(s.drivers->pid(kAda) < 0);
    std::vector<std::string> report;
    if (!listedFirst) {
        accounts::Registry without = with;
        without.ais.clear();
        CHECK(s.hub->applyAccounts(without, "reload", &report));
        s.serveUntil([] { return false; }, 1.0);
        CHECK(sendsOf(dir + "/sends", "q1") == 0 && sendsOf(dir + "/sends", "q2") == 0);
        CHECK((s.store.find(kAda)->queue == std::vector<std::string>{"q1", "q2"}));
        CHECK(s.drivers->pid(kAda) < 0);
        CHECK(s.activity() == sync::Activity::Down);
    }
    CHECK(s.hub->applyAccounts(with, "reload", &report));
    CHECK(s.serveUntil([&] { return s.store.find(kAda)->queue.empty() && idle(s); }, 10));
    CHECK(sendsOf(dir + "/sends", "q1") == 1 && sendsOf(dir + "/sends", "q2") == 1);
    CHECK(sendsOf(dir + "/sends", "now") == 0);
    s.end();
    if (failures != 0) {
        dump(s);
    }
}

} // namespace

int main(int argc, char** argv) {
    snapshotEnvironment();     // first, before any server code can change the environment
    if (argc != 4 && argc != 5) {
        std::fprintf(stderr, "usage: drivers_functional <nexus-fake-driver> <dir> <case> [<state dir>]\n");
        return 2;
    }
    const std::string fake = argv[1];
    const std::string dir = argv[2];
    const std::string name = argv[3];
    if (argc == 5) {
        stateDir = argv[4];
    }
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
        } else if (name == "restart_unconfigured") {
            // The server dies mid-turn with a prompt queued, and starts again with ada no longer configured: the turn
            // is cut, and the queued prompt stays queued, with no driver started for it.
            {
                Server s(dir);
                setUp(dir, fake, std::string(kHello) + "send\nemit {\"kind\":\"text\",\"delta\":\"a\"}\nhang\n");
                s.send("first");
                CHECK(s.serveUntil([&] { return s.sup().gathering(); }, 10));
                CHECK(s.hub->apply(s.action(sync::ActionKind::Queue, "queued")));
                CHECK(s.store.find(kAda)->queue == std::vector<std::string>{"queued"});
            }
            writeFile(dir + "/drivers", "");
            Server s(dir);
            CHECK(s.driver().openTurn == 0);
            CHECK(s.driver().waiting.empty());
            CHECK(s.store.find(kAda)->queue == std::vector<std::string>{"queued"});
            const std::vector<std::string> t = s.transcript();
            CHECK(t.size() == 2 && has(t.back(), "nexus-server restarted while ada was answering"));
            CHECK(s.drivers->pid(kAda) < 0);
            s.end();
            if (failures != 0) {
                dump(s);
            }
        } else if (name == "no_registry" || name == "no_registry_listed") {
            noRegistry(dir, fake, name == "no_registry_listed");
        } else if (name == "secret_ask" || name == "secret_sign_in" || name == "secret_leak_witness") {
            Server s(dir, true);
            if (name == "secret_ask") {
                secretAsk(s, dir, fake);
            } else if (name == "secret_sign_in") {
                secretSignIn(s, dir, fake);
            } else {
                secretLeakWitness(s, dir, fake);
            }
            s.end();
            if (failures != 0) {
                dump(s);
                for (const std::string& line : s.log) {
                    std::fprintf(stderr, "  log: %s\n", line.c_str());
                }
            }
        } else {
            Server s(dir);
            using Case = void (*)(Server&, const std::string&, const std::string&);
            const std::vector<std::pair<std::string, Case>> cases = {{"normal", normal}, {"stop", stop},
                {"silent", silent}, {"exit_mid_turn", exitMidTurn}, {"malformed", malformed},
                {"ignores_stop", ignoresStop}, {"kill", kill}, {"flood", flood},
                {"fails_first_start", failsFirstStart}, {"resume", resume}, {"primary_route", primaryRoute}, {"orphan", orphan},
                {"escaped_output", escapedOutput}, {"queue", queue}, {"queue_unconfigured", queueUnconfigured},
                {"unconfigured", unconfigured}, {"stop_hammered", stopHammered}, {"account", account},
                {"window", window}, {"bus_admission", busAdmission}, {"bus_default_instance", busDefaultInstance},
                {"sign_in", signIn}, {"no_credential", noCredential},
                {"usage_limit", usageLimit}, {"continuation", continuation}, {"bus_continuation", busContinuation},
                {"fable_switch_account", [](Server& s, const std::string& d, const std::string& f) { fableChoice(s, d, f, true); }},
                {"fable_switch_model", [](Server& s, const std::string& d, const std::string& f) { fableChoice(s, d, f, false); }},
                {"limit_unsupported", limitUnsupported}, {"no_account", noAccount},
                {"ai_removed", aiRemoved}, {"ai_back_limited", aiBackLimited},
                {"ai_removed_starting", aiRemovedStarting}, {"ai_back_before_stop", aiBackBeforeStop},
                {"ai_back_limited_queue", aiBackLimitedQueue}, {"ai_back_moved", aiBackMoved},
                {"ai_back_account", aiBackAccount},
                {"instance_dir", instanceDir},
                {"instance_elsewhere", instanceElsewhere}, {"instance_moved", instanceMoved},
                {"instance_removed", instanceRemoved}, {"instance_default", instanceDefault}, {"held", held}};
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
