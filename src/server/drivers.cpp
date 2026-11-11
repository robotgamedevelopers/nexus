#include "server/drivers.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <climits>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <sstream>

extern char** environ;

namespace nexus::server {

namespace {

// posix_spawn's way to start a child with no descriptor of ours but the ones its file actions name: on macOS the flag
// POSIX_SPAWN_CLOEXEC_DEFAULT (and addinherit_np for one to keep as it is); on Linux, glibc's addclosefrom_np(3) after
// the other file actions, which leaves 0 to 2 as they are.
#if defined(__APPLE__)
constexpr short kSpawnCloseOthers = POSIX_SPAWN_CLOEXEC_DEFAULT;
#else
constexpr short kSpawnCloseOthers = 0;
#endif

sync::EntryRef entry(EntryKind kind, std::string text) {
    return std::make_shared<const sync::TabEntry>(sync::TabEntry{kind, std::move(text), true});
}

void log(const std::string& tab, const std::string& what) {
    std::fprintf(stderr, "nexus-server: %s: %s\n", tab.c_str(), what.c_str());
}

// How a process ended, as a notice says it after "ada's driver ".
std::string howEnded(int status) {
    if (WIFEXITED(status)) {
        return "exited with status " + std::to_string(WEXITSTATUS(status));
    }
    if (WIFSIGNALED(status)) {
        const char* name = strsignal(WTERMSIG(status));
        return "was ended by signal " + std::to_string(WTERMSIG(status)) + (name != nullptr ? std::string(" (") +
            name + ")" : std::string());
    }
    return "ended (wait status " + std::to_string(status) + ")";
}

// One output after another, as one: the effects and commands in order, the latest of the rest.
void append(Output& into, Output from) {
    for (Effect& e : from.effects) {
        into.effects.push_back(std::move(e));
    }
    into.durable = into.durable || from.durable;
    for (Command& c : from.commands) {
        into.commands.push_back(std::move(c));
    }
    if (from.credential) {
        into.credential = std::move(from.credential);
    }
    if (from.sessionCostUsd) {
        into.sessionCostUsd = from.sessionCostUsd;
    }
}

// Starts `argv` with its standard input and output on pipes (ours non-blocking), its standard error this process's,
// in a process group of its own, with default signal handling and no other descriptor of ours. Returns the pid, or -1
// with why.
pid_t spawnDriver(const std::vector<std::string>& argv, int* in, int* out, std::string* why) {
    if (argv.empty()) {
        *why = "no program is configured";
        return -1;
    }
    int toChild[2] = {-1, -1};
    int fromChild[2] = {-1, -1};
    if (::pipe(toChild) != 0 || ::pipe(fromChild) != 0) {
        *why = std::string("cannot make its pipes: ") + std::strerror(errno);
        for (int fd : {toChild[0], toChild[1], fromChild[0], fromChild[1]}) {
            if (fd >= 0) {
                ::close(fd);
            }
        }
        return -1;
    }
    for (int fd : {toChild[0], toChild[1], fromChild[0], fromChild[1]}) {
        ::fcntl(fd, F_SETFD, FD_CLOEXEC);
    }
    posix_spawn_file_actions_t files;
    posix_spawn_file_actions_init(&files);
    posix_spawn_file_actions_adddup2(&files, toChild[0], 0);
    posix_spawn_file_actions_adddup2(&files, fromChild[1], 1);
#if defined(__APPLE__)
    posix_spawn_file_actions_addinherit_np(&files, 2);
#else
    posix_spawn_file_actions_addclosefrom_np(&files, 3);
#endif
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETSIGMASK |
        kSpawnCloseOthers);
    posix_spawnattr_setpgroup(&attr, 0);
    sigset_t defaults;
    sigemptyset(&defaults);
    for (int s : {SIGPIPE, SIGTERM, SIGINT, SIGHUP, SIGCHLD}) {
        sigaddset(&defaults, s);
    }
    posix_spawnattr_setsigdefault(&attr, &defaults);
    sigset_t none;
    sigemptyset(&none);
    posix_spawnattr_setsigmask(&attr, &none);
    std::vector<std::string> args = argv;
    std::vector<char*> raw;
    for (std::string& a : args) {
        raw.push_back(a.data());
    }
    raw.push_back(nullptr);
    pid_t pid = -1;
    const int rc = posix_spawnp(&pid, raw[0], &files, &attr, raw.data(), environ);
    posix_spawn_file_actions_destroy(&files);
    posix_spawnattr_destroy(&attr);
    ::close(toChild[0]);
    ::close(fromChild[1]);
    if (rc != 0) {
        ::close(toChild[1]);
        ::close(fromChild[0]);
        *why = "`" + argv[0] + "`: " + std::strerror(rc);
        return -1;
    }
    ::fcntl(toChild[1], F_SETFL, O_NONBLOCK);
    ::fcntl(fromChild[0], F_SETFL, O_NONBLOCK);
    *in = toChild[1];
    *out = fromChild[0];
    return pid;
}

} // namespace

Drivers::Drivers(const sync::TabStore& store, Options options) : store_(store), options_(std::move(options)) {}

Drivers::~Drivers() {
    for (auto& [name, t] : tabs_) {
        if (!t.proc) {
            continue;
        }
        if (t.proc->in >= 0) {
            ::close(t.proc->in);
        }
        if (t.proc->out >= 0) {
            ::close(t.proc->out);
        }
        ::killpg(t.proc->pid, SIGKILL);
        int status = 0;
        while (::waitpid(t.proc->pid, &status, 0) < 0 && errno == EINTR) {
        }
    }
}

ResolvedDriver Drivers::resolve(const std::string& tab) const {
    std::ifstream in(options_.table);
    std::string text;
    if (in) {
        std::ostringstream all;
        all << in.rdbuf();
        text = all.str();
    } else if (errno != ENOENT) {
        ResolvedDriver r;
        r.problem = "nexus-server cannot read " + options_.table + " (" + std::strerror(errno) + "), so the prompt to " +
            tab + " was not sent. Make the file readable by this user.";
        return r;
    }
    return resolveDriver(parseDriverTable(text, options_.home), tab, options_.table);
}

Config Drivers::config(const std::string& tab, const ResolvedDriver& driver) const {
    Config c;
    c.name = tab;
    c.command = driver.command;
    c.cwd = driver.cwd;
    c.configured = options_.table;
    c.timing = options_.timing;
    c.maxWaiting = hub_->limits().queue;
    c.maxWaitingBytes = hub_->limits().queueBytes;
    c.maxEntryBytes = hub_->limits().text;
    return c;
}

bool Drivers::start(Millis now) {
    for (const std::string& name : store_.names()) {
        sync::DriverState state = store_.find(name)->driver;
        if (state.openTurn == 0 && state.waiting.empty()) {
            continue;
        }
        const ResolvedDriver r = resolve(name);
        std::string dropped;
        if (!r.problem.empty() && !state.waiting.empty()) {
            dropped = std::to_string(state.waiting.size()) + " prompt" + (state.waiting.size() == 1 ? "" : "s") +
                " waiting for " + name + " when nexus-server stopped could not be sent: " + r.problem;
            state.waiting.clear();
        }
        Output out;
        tabs_.emplace(name, Tab(Supervisor(config(name, r), state, now, &out)));
        if (!dropped.empty()) {
            out.effects.push_back(Effect{Effect::What::Notice, dropped, 0});
            out.durable = true;
        }
        if (!run(name, std::move(out), now, false)) {
            return false;
        }
    }
    return true;
}

std::vector<sync::TabChange> Drivers::send(const std::string& tab, const std::string& prompt) {
    const Millis now = options_.clock();
    pendingSup_.reset();
    pendingOut_ = Output{};
    const ResolvedDriver r = resolve(tab);
    if (!r.problem.empty()) {
        return {sync::TabChange{entry(EntryKind::Person, prompt), {}}, sync::TabChange{entry(EntryKind::Notice,
            r.problem), {}}};
    }
    auto it = tabs_.find(tab);
    if (it != tabs_.end()) {
        pendingSup_ = it->second.sup;
        pendingSup_->configure(r.command, r.cwd, options_.table);
    } else {
        const sync::StoredTab* stored = store_.find(tab);
        pendingSup_.emplace(config(tab, r), stored != nullptr ? stored->driver : sync::DriverState{}, now, &pendingOut_);
    }
    append(pendingOut_, pendingSup_->prompt(prompt, now));
    std::vector<sync::TabChange> changes;
    for (const Effect& e : pendingOut_.effects) {
        changes.push_back(sync::TabChange{entry(entryKind(e.what), e.text), {}});
    }
    if (pendingOut_.durable) {
        changes.push_back(sync::TabChange{nullptr, {}, pendingSup_->state()});
    }
    return changes;
}

void Drivers::sent(const std::string& tab, bool committed) {
    if (!committed || !pendingSup_) {
        return;
    }
    auto it = tabs_.find(tab);
    if (it == tabs_.end()) {
        tabs_.emplace(tab, Tab(std::move(*pendingSup_)));
    } else {
        it->second.sup = std::move(*pendingSup_);
    }
    pendingSup_.reset();
    run(tab, std::move(pendingOut_), options_.clock(), true);
}

bool Drivers::stop(const std::string& tab, Millis now) {
    auto it = tabs_.find(tab);
    return it == tabs_.end() || run(tab, it->second.sup.stop(now), now, false);
}

bool Drivers::shutdown(Millis now) {
    for (auto& [name, t] : tabs_) {
        if (!run(name, t.sup.shutdown(now), now, false)) {
            return false;
        }
    }
    return true;
}

bool Drivers::alive() const {
    return std::any_of(tabs_.begin(), tabs_.end(), [](const auto& t) { return t.second.proc != nullptr; });
}

const Supervisor* Drivers::supervisor(const std::string& tab) const {
    auto it = tabs_.find(tab);
    return it == tabs_.end() ? nullptr : &it->second.sup;
}

pid_t Drivers::pid(const std::string& tab) const {
    auto it = tabs_.find(tab);
    return it == tabs_.end() || !it->second.proc ? -1 : it->second.proc->pid;
}

// The step's effects and durable state, in one step of the store.
bool Drivers::commit(const std::string& tab, const Output& out, bool* changed) {
    std::vector<sync::TabChange> changes;
    for (const Effect& e : out.effects) {
        changes.push_back(sync::TabChange{entry(entryKind(e.what), e.text), {}});
    }
    if (out.durable) {
        changes.push_back(sync::TabChange{nullptr, {}, tabs_.at(tab).sup.state()});
    }
    if (changes.empty()) {
        return true;
    }
    std::string why;
    if (!hub_->commit(tab, changes, &why)) {
        log(tab, "the store did not take the driver's step: " + why);
        return false;
    }
    if (changed != nullptr) {
        *changed = true;
    }
    return true;
}

// An output: committed (unless it was already, with the window's action), then its commands carried out in order;
// what carrying them out gives (a driver that could not start) goes the same way.
bool Drivers::run(const std::string& tab, Output out, Millis now, bool committed) {
    std::vector<Output> queue;
    queue.push_back(std::move(out));
    for (std::size_t i = 0; i < queue.size(); ++i) {
        Output o = std::move(queue[i]);
        if (!(i == 0 && committed) && !commit(tab, o)) {
            return false;
        }
        if (o.credential) {
            log(tab, "the driver's credential: " + std::string(driver::billingName(o.credential->billing)) +
                (o.credential->plan.empty() ? std::string() : " (" + o.credential->plan + ")"));
        }
        if (o.sessionCostUsd) {
            char cost[64];
            std::snprintf(cost, sizeof cost, "the session has cost $%.4f so far", *o.sessionCostUsd);
            log(tab, cost);
        }
        Tab& t = tabs_.at(tab);
        std::vector<Output> more;
        for (const Command& c : o.commands) {
            carryOut(tab, t, c, more, now);
        }
        for (Output& m : more) {
            queue.push_back(std::move(m));
        }
    }
    return true;
}

void Drivers::carryOut(const std::string& tab, Tab& t, const Command& c, std::vector<Output>& more, Millis now) {
    Process* p = t.proc.get();
    switch (c.kind) {
        case Command::Kind::Start: {
            if (p != nullptr) {
                log(tab, "a driver start while one runs was ignored: a bug in the supervisor");
                return;
            }
            auto proc = std::make_unique<Process>();
            std::string why;
            proc->pid = spawnDriver(c.argv, &proc->in, &proc->out, &why);
            if (proc->pid < 0) {
                more.push_back(t.sup.exited("could not be started: " + why, now));
                return;
            }
            log(tab, "driver " + std::to_string(proc->pid) + " started: " + c.argv[0]);
            t.proc = std::move(proc);
            return;
        }
        case Command::Kind::Write:
            if (p != nullptr) {
                p->pending += c.line;
                flushInput(*p, now);
            }
            return;
        case Command::Kind::CloseInput:
            if (p != nullptr) {
                p->closeInput = true;
                flushInput(*p, now);
            }
            return;
        case Command::Kind::Terminate:
        case Command::Kind::Kill:
            if (p != nullptr) {
                ::killpg(p->pid, c.kind == Command::Kind::Terminate ? SIGTERM : SIGKILL);
            }
            return;
    }
}

// Writes what the driver will take now; a driver that stopped reading its input loses what is left.
void Drivers::flushInput(Process& p, Millis now) {
    while (!p.pending.empty() && p.in >= 0) {
        const ssize_t n = ::write(p.in, p.pending.data(), p.pending.size());
        if (n > 0) {
            p.pending.erase(0, static_cast<std::size_t>(n));
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else if (n < 0 && errno == EAGAIN) {
            return;
        } else {
            p.pending.clear();
            ::close(p.in);
            p.in = -1;
            if (!p.brokenSince) {
                p.brokenSince = now;
                p.broken = "input";
            }
        }
    }
    if (p.closeInput && p.pending.empty() && p.in >= 0) {
        ::close(p.in);
        p.in = -1;
    }
}

// Up to `limit` bytes of the driver's output, as messages to its supervisor.
void Drivers::readSome(Tab& t, Output& out, Millis now, std::size_t limit) {
    Process& p = *t.proc;
    std::vector<driver::DriverReader::Item> items;
    char buffer[16 * 1024];
    std::size_t total = 0;
    while (p.out >= 0 && total < limit) {
        const ssize_t n = ::read(p.out, buffer, sizeof buffer);
        if (n > 0) {
            p.reader.feed(std::string_view(buffer, static_cast<std::size_t>(n)), items);
            total += static_cast<std::size_t>(n);
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else if (n < 0 && errno == EAGAIN) {
            break;
        } else {
            p.reader.finish(items);
            ::close(p.out);
            p.out = -1;
            if (!p.brokenSince) {
                p.brokenSince = now;
                p.broken = "output";
            }
        }
    }
    for (driver::DriverReader::Item& item : items) {
        if (item.message) {
            append(out, t.sup.message(*item.message, now));
        } else {
            append(out, t.sup.refused(item.error, now));
        }
    }
}

// The driver's exit, if it has exited: what it wrote before, then the exit.
void Drivers::collect(const std::string& tab, Tab& t, Output& out, Millis now) {
    Process& p = *t.proc;
    siginfo_t info{};
    if (::waitid(P_PID, static_cast<id_t>(p.pid), &info, WEXITED | WNOHANG | WNOWAIT) != 0 || info.si_pid != p.pid) {
        return;
    }
    // What it started in its process group (its harness) goes with it, while its pid, not yet collected, still names
    // the group: a next driver for the tab never works beside it.
    ::killpg(p.pid, SIGKILL);
    int status = 0;
    while (::waitpid(p.pid, &status, 0) < 0 && errno == EINTR) {
    }
    readSome(t, out, now, kReadChunk);
    if (p.out >= 0) {
        // Its output is still held open (by a process it started): what is read so far is all it gets.
        std::vector<driver::DriverReader::Item> rest;
        p.reader.finish(rest);
        for (driver::DriverReader::Item& item : rest) {
            append(out, item.message ? t.sup.message(*item.message, now) : t.sup.refused(item.error, now));
        }
        ::close(p.out);
    }
    if (p.in >= 0) {
        ::close(p.in);
    }
    log(tab, "driver " + std::to_string(p.pid) + " " + howEnded(status));
    t.proc.reset();
    append(out, t.sup.exited(howEnded(status), now));
}

std::vector<pollfd> Drivers::fds() const {
    std::vector<pollfd> out;
    for (const auto& [name, t] : tabs_) {
        if (!t.proc) {
            continue;
        }
        if (t.proc->out >= 0) {
            out.push_back(pollfd{t.proc->out, POLLIN, 0});
        }
        if (t.proc->in >= 0 && !t.proc->pending.empty()) {
            out.push_back(pollfd{t.proc->in, POLLOUT, 0});
        }
    }
    return out;
}

int Drivers::waitMs(Millis now) const {
    Millis wait = INT_MAX;
    for (const auto& [name, t] : tabs_) {
        if (const std::optional<Millis> d = t.sup.deadline()) {
            wait = std::min(wait, *d > now ? *d - now : 0);
        }
        // An exit is collected by polling for it: soon, once its output has ended, and within kCollectMs while a
        // process outside the driver's group holds its output open.
        if (t.proc && t.proc->brokenSince) {
            wait = std::min<Millis>(wait, 10);
        } else if (t.proc) {
            wait = std::min<Millis>(wait, kCollectMs);
        }
    }
    return static_cast<int>(wait);
}

bool Drivers::serve(Millis now, bool* changed) {
    for (auto& [name, t] : tabs_) {
        Output out;
        if (t.proc) {
            flushInput(*t.proc, now);
            readSome(t, out, now, kReadChunk);
        }
        if (t.proc) {
            collect(name, t, out, now);
        }
        if (t.proc && t.proc->brokenSince && !t.proc->brokenTold && now - *t.proc->brokenSince >= kPipeGraceMs) {
            t.proc->brokenTold = true;
            append(out, t.sup.closed(t.proc->broken, now));
        }
        append(out, t.sup.tick(now));
        const bool any = !out.effects.empty() || out.durable || !out.commands.empty();
        if (any) {
            if (!commit(name, out, changed)) {
                return false;
            }
            if (!run(name, std::move(out), now, true)) {
                return false;
            }
        }
    }
    return true;
}

} // namespace nexus::server
