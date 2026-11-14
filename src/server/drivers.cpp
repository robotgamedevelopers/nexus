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

#include "server/home.h"

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
    return std::make_shared<const sync::SessionEntry>(sync::SessionEntry{kind, std::move(text), true});
}

void log(sync::SessionId id, const std::string& what) {
    std::fprintf(stderr, "nexus-server: session %llu: %s\n", static_cast<unsigned long long>(id), what.c_str());
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
    if (from.signedIn) {
        into.signedIn = from.signedIn;
        into.account = from.account;
    }
    if (from.usageLimit) {
        into.usageLimit = true;
        into.account = from.account;
    }
    into.taken += from.taken;
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

// The most of the conversation a summary carries. A judgement, not measured: enough of its recent turns to pick up
// from, far within the protocol's line limit with the hub's longest prompt after it.
constexpr std::size_t kSummaryBytes = 16 * 1024;

// The end of `text` that fits in `bytes`, cut at a UTF-8 character's start.
std::string tailOf(const std::string& text, std::size_t bytes) {
    if (text.size() <= bytes) {
        return text;
    }
    std::size_t from = text.size() - bytes;
    while (from < text.size() && (static_cast<unsigned char>(text[from]) & 0xC0) == 0x80) {
        ++from;
    }
    return text.substr(from);
}

// What goes before the prompt of a driver started afresh in another directory: the conversation's newest entries,
// oldest first, within kSummaryBytes, each said whose it is. Only the turns before `prompt`: the transcript already
// ends with it (the person's entry, shown when it left the queue, and what followed it), and the send carries it
// after.
std::string summaryOf(const std::string& friendName, const std::vector<sync::EntryRef>& transcript,
                      const std::string& prompt) {
    auto before = transcript.rbegin();
    while (before != transcript.rend() && !((*before)->kind == EntryKind::Person && (*before)->text == prompt)) {
        ++before;
    }
    if (before == transcript.rend()) {
        before = transcript.rbegin();   // not shown (cannot be, but then nothing is left out)
    } else {
        ++before;
    }
    std::vector<std::string> parts;
    std::size_t bytes = 0;
    for (auto it = before; it != transcript.rend() && bytes < kSummaryBytes; ++it) {
        const char* who = (*it)->kind == EntryKind::Person ? "The person" :
            (*it)->kind == EntryKind::Friend ? "You" : "nexus";
        std::string part = std::string(who) + ": " + tailOf((*it)->text, kSummaryBytes - bytes) + "\n";
        bytes += part.size();
        parts.push_back(std::move(part));
    }
    std::string text = "[nexus: this conversation with " + friendName + " was held in another working directory, "
        "where its harness session stays, so it starts afresh here. Its newest entries so far, oldest first:]\n";
    for (auto it = parts.rbegin(); it != parts.rend(); ++it) {
        text += *it;
    }
    return text + "[nexus: the person's prompt follows.]\n\n";
}

} // namespace

Drivers::Drivers(const sync::SessionStore& store, Options options) : store_(store), options_(std::move(options)) {}

Drivers::~Drivers() {
    for (auto& [id, t] : slots_) {
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

std::string Drivers::friendOf(sync::SessionId id) const {
    const sync::StoredSession* stored = store_.find(id);
    return stored == nullptr ? std::string() : stored->info.friendName;
}

// The session's driver: its friend's line of the drivers file, with the session's account in place of the value its
// command passes after `--account` (the Claude driver's: part 4 of the sign-in work gives every driver its account,
// docs/roadmap.sexp). A command that takes an account starts for a session with one only: a session on no account (one
// whose friend had no signed-in account left when its own was unlinked) gets the reason and how to link one. A session
// on an account of another provider than its friend's harness has no driver here until that provider's driver exists.
ResolvedDriver Drivers::resolve(sync::SessionId id) const {
    const std::string friendName = friendOf(id);
    DriverTable table;
    ResolvedDriver r = resolveFriend(friendName, &table);
    const sync::StoredSession* stored = store_.find(id);
    if (!r.problem.empty() || stored == nullptr) {
        return r;
    }
    const sync::SessionInfo& info = stored->info;
    const bool takesAccount = std::find(r.command.begin(), r.command.end(), "--account") != r.command.end();
    auto refuse = [&](const std::string& problem) {
        r.problem = problem;
        r.command.clear();
        return r;
    };
    // The session's instance (none named: its friend's default): its machine must be this server's (a driver on another
    // machine is roadmap work, docs/roadmap.sexp), and its directory is where the driver works.
    if (const accounts::Instance* in = instanceOf(info)) {
        if (!sameMachine(in->machine, options_.machine, options_.hostName)) {
            const std::string here = options_.machine.empty() ? std::string("a machine with no name (gethostname "
                "failed, and the server's config sets no machine)") : options_.machine;
            return refuse("Session " + std::to_string(id) + " belongs to " + friendName + "'s instance " + in->name +
                ", on the machine " + in->machine + ", and this server runs on " + here + "; nexus-server does not "
                "yet start a driver on another machine, so the prompt was not sent. Make a session of an instance of " +
                friendName + " on this machine, or add one in friends.json (docs/accounts.md)" +
                (options_.machine.empty() ? std::string(", and set `machine <name>` in the server's config") : "") +
                ".");
        }
        r.cwd = in->dir;
    }
    if (info.account.empty()) {
        if (takesAccount && hub_ != nullptr && hub_->accounts() != nullptr) {
            return refuse("Session " + std::to_string(id) + " with " + friendName + " runs on no account, and " +
                friendName + "'s driver needs one, so the prompt was not sent. Link an account to " + friendName +
                " in friends.json (its \"accounts\", and its \"default\") and run `nexus-server config reload`, then "
                "set this session to it; docs/accounts.md says how.");
        }
        return r;
    }
    // The account's provider, where the server holds accounts (before them, an account is any name, of no provider).
    const accounts::Account* held = hub_ != nullptr && hub_->accounts() != nullptr ?
        hub_->accounts()->account(info.account) : nullptr;
    auto line = table.friends.find(friendName);
    const std::string harness = line == table.friends.end() ? std::string() : line->second.harness;
    if (held != nullptr && held->provider != harness) {
        return refuse("Session " + std::to_string(id) + " runs on the account " + info.account + ", whose provider is " +
            held->provider + ", but " + friendName + "'s driver in " + options_.table + " is " + harness + "'s, and "
            "nexus-server has no " + held->provider + " driver for it yet, so the prompt was not sent. Set the "
            "session to one of " + friendName + "'s " + harness + " accounts.");
    }
    for (std::size_t i = 1; i + 1 < r.command.size(); ++i) {
        if (r.command[i] == "--account") {
            r.command[i + 1] = info.account;
        }
    }
    return r;
}

// The instance a session belongs to, as the hub holds its friend: the one it names, or its friend's default (its
// first) when it names none. Null when the hub holds no accounts, the friend has no instances, or (until the hub settles
// it) the one named is gone.
const accounts::Instance* Drivers::instanceOf(const sync::SessionInfo& info) const {
    const accounts::Friend* fr = hub_ != nullptr && hub_->accounts() != nullptr ?
        hub_->accounts()->friendNamed(info.friendName) : nullptr;
    if (fr == nullptr) {
        return nullptr;
    }
    if (info.instance.empty()) {
        return fr->instances.empty() ? nullptr : &fr->instances.front();
    }
    return fr->instance(info.instance);
}

ResolvedDriver Drivers::resolveFriend(const std::string& friendName, DriverTable* table) const {
    std::ifstream in(options_.table);
    std::string text;
    if (in) {
        std::ostringstream all;
        all << in.rdbuf();
        text = all.str();
    } else if (errno != ENOENT) {
        ResolvedDriver r;
        r.problem = "nexus-server cannot read " + options_.table + " (" + std::strerror(errno) + "), so the prompt to " +
            friendName + " was not sent. Make the file readable by this user.";
        return r;
    }
    DriverTable parsed = parseDriverTable(text, options_.home);
    ResolvedDriver r = resolveDriver(parsed, friendName, options_.table);
    if (table != nullptr) {
        *table = std::move(parsed);
    }
    return r;
}

sync::SessionInfo Drivers::defaults(const std::string& friendName) const {
    // The drivers file names a program per friend, not a harness: every driver nexus has is Claude's, for now.
    return sync::SessionInfo{friendName, sync::kDefaultHarness, {}, {}, {}};
}

Config Drivers::config(sync::SessionId id, const ResolvedDriver& driver) const {
    Config c;
    const sync::StoredSession* stored = store_.find(id);
    c.name = friendOf(id);
    if (stored != nullptr) {
        c.model = stored->info.model;
        c.effort = stored->info.effort;
        c.account = stored->info.account;
    }
    c.command = driver.command;
    c.cwd = driver.cwd;
    if (stored != nullptr) {
        const accounts::Instance* in = instanceOf(stored->info);
        c.here = in == nullptr || sameMachine(in->machine, options_.machine, options_.hostName);
    }
    c.summarize = [this, id, name = c.name](const std::string& prompt) {
        const sync::StoredSession* s = store_.find(id);
        return s == nullptr ? std::string() : summaryOf(name, s->transcript, prompt);
    };
    c.configured = options_.table;
    c.timing = options_.timing;
    c.maxWaiting = hub_->limits().queue;
    c.maxWaitingBytes = hub_->limits().queueBytes;
    c.maxEntryBytes = hub_->limits().text;
    return c;
}

bool Drivers::start(Millis now) {
    for (sync::SessionId id : store_.ids()) {
        const sync::StoredSession* stored = store_.find(id);
        sync::DriverState state = stored->driver;
        const ResolvedDriver r = resolve(id);
        if (state.openTurn == 0 && state.waiting.empty() && (stored->queue.empty() || !r.problem.empty())) {
            // Nothing to run, or only queued prompts and no driver to give them (they stay queued, as they are): the
            // session shows idle, or down when its friend has no driver configured.
            showResolved(id, r);
            continue;
        }
        std::string dropped;
        if (!r.problem.empty() && !state.waiting.empty()) {
            dropped = std::to_string(state.waiting.size()) + " prompt" + (state.waiting.size() == 1 ? "" : "s") +
                " waiting for " + stored->info.friendName + " when nexus-server stopped could not be sent: " +
                r.problem;
            state.waiting.clear();
        }
        Output out;
        slots_.emplace(id, Slot(Supervisor(config(id, r), state, now, &out, stored->queue)));
        if (!dropped.empty()) {
            out.effects.push_back(Effect{Effect::What::Notice, dropped, 0});
            out.durable = true;
        }
        if (!run(id, std::move(out), now, false)) {
            return false;
        }
    }
    return true;
}

// The session's supervisor as the action in progress changes it: a copy of the one running, or a new one over what the
// store holds, kept aside until the hub has committed the action (sent()).
Supervisor& Drivers::pending(sync::SessionId id, const ResolvedDriver& driver, Millis now) {
    auto it = slots_.find(id);
    if (it != slots_.end()) {
        pendingSup_ = it->second.sup;
        // A friend whose driver no longer resolves has none: what is queued stays queued until it has one again. Its
        // directory stays the session's: a driver that does not resolve names none, and a driver already running
        // there is not moved by it.
        const bool none = !driver.problem.empty();
        append(pendingOut_, pendingSup_->configure(none ? std::vector<std::string>{} : driver.command,
            none ? pendingSup_->cwd() : driver.cwd, options_.table, now));
    } else {
        const sync::StoredSession* stored = store_.find(id);
        pendingSup_.emplace(config(id, driver), stored != nullptr ? stored->driver : sync::DriverState{}, now,
            &pendingOut_, stored != nullptr ? stored->queue : std::vector<std::string>{});
    }
    return *pendingSup_;
}

std::vector<sync::SessionChange> Drivers::send(sync::SessionId id, const std::string& prompt) {
    const Millis now = options_.clock();
    pendingSup_.reset();
    pendingOut_ = Output{};
    const ResolvedDriver r = resolve(id);
    if (!r.problem.empty()) {
        return {sync::SessionChange{entry(EntryKind::Person, prompt), {}}, sync::SessionChange{entry(EntryKind::Notice,
            r.problem), {}}};
    }
    Supervisor& sup = pending(id, r, now);
    append(pendingOut_, sup.prompt(prompt, now));
    return changes(pendingOut_, sup);
}

// The prompt at the end of the queue first: the supervisor's output follows it, and takes from the head.
std::vector<sync::SessionChange> Drivers::queue(sync::SessionId id, const std::string& prompt) {
    const Millis now = options_.clock();
    pendingSup_.reset();
    pendingOut_ = Output{};
    std::vector<sync::SessionChange> out = {sync::SessionChange{nullptr, prompt}};
    const ResolvedDriver r = resolve(id);
    if (!r.problem.empty() && slots_.find(id) == slots_.end()) {
        return out;     // no driver to give it to: it stays queued until the id has one
    }
    Supervisor& sup = pending(id, r, now);
    append(pendingOut_, sup.queued(prompt, now));
    for (sync::SessionChange& c : changes(pendingOut_, sup)) {
        out.push_back(std::move(c));
    }
    return out;
}

// A stop of an open turn changes nothing in the store: its stop line is written once the hub has counted the action.
// A stop of a start cancels the turn about to begin: its prompt leaves the waiting prompts, committed with a notice.
std::vector<sync::SessionChange> Drivers::stop(sync::SessionId id) {
    pendingSup_.reset();
    pendingOut_ = Output{};
    auto it = slots_.find(id);
    if (it == slots_.end()) {
        return {};      // no supervisor: no driver runs for the id, so nothing to stop
    }
    pendingSup_ = it->second.sup;
    append(pendingOut_, pendingSup_->stop(options_.clock()));
    return changes(pendingOut_, *pendingSup_);
}

void Drivers::sent(sync::SessionId id, bool committed) {
    if (!committed) {
        pendingSup_.reset();
        return;
    }
    if (!pendingSup_) {
        showResolved(id, resolve(id));  // refused for want of a driver, or a stop with none running
        return;
    }
    auto it = slots_.find(id);
    if (it == slots_.end()) {
        slots_.emplace(id, Slot(std::move(*pendingSup_)));
    } else {
        it->second.sup = std::move(*pendingSup_);
    }
    pendingSup_.reset();
    run(id, std::move(pendingOut_), options_.clock(), true);
}

// The session's account, model, effort or instance changed, durably: a supervisor already running takes them from its
// next turn (one started later reads them from the store and the accounts). An account change ends its driver once no
// turn is open, so the next starts under the new account, with the same session and transcript; so does a change of
// the instance's directory (the next starts there, afresh when the harness session was made elsewhere) or a move of
// the instance to another machine (then none starts here). A session left with no instance works in the drivers file's
// directory, here.
void Drivers::configured(sync::SessionId id, const sync::SessionInfo& info) {
    auto it = slots_.find(id);
    if (it == slots_.end()) {
        return;
    }
    const Millis now = options_.clock();
    Output out = it->second.sup.setOptions(info.model, info.effort, now);
    append(out, it->second.sup.setAccount(info.account, now));
    if (const accounts::Instance* in = instanceOf(info)) {
        const bool here = sameMachine(in->machine, options_.machine, options_.hostName);
        append(out, it->second.sup.setPlace(in->dir, here, now));
    } else {
        // No instance (its friend has none now): the drivers file's directory, on this machine, so that a session
        // whose instance was elsewhere is never left not here.
        const ResolvedDriver line = resolveFriend(info.friendName, nullptr);
        append(out, it->second.sup.setPlace(line.problem.empty() ? line.cwd : it->second.sup.cwd(), true, now));
    }
    run(id, std::move(out), now, false);
}

void Drivers::made(sync::SessionId id) {
    showResolved(id, resolve(id));
}

// A session's driver as the drivers file now says, for what the window shows of it: a session with no supervisor
// shows down when its friend has none configured, idle otherwise (nothing runs in it); one with a supervisor is told,
// and shows what that makes of it (Supervisor::activity).
// It is read again each kResolveMs while the friend has none (serve()). False when the store did not take a step.
bool Drivers::showResolved(sync::SessionId id, const ResolvedDriver& driver) {
    if (driver.problem.empty()) {
        unresolved_.erase(id);
    } else {
        unresolved_.insert(id);
    }
    auto it = slots_.find(id);
    const Millis now = options_.clock();
    if (it == slots_.end()) {
        const sync::StoredSession* stored = store_.find(id);
        if (!driver.problem.empty() || stored == nullptr || stored->queue.empty()) {
            hub_->setActivity(id, driver.problem.empty() ? sync::Activity::Idle : sync::Activity::Down);
            return true;
        }
        // A driver at last for the prompts queued without one: a supervisor for the session, which takes the oldest.
        Output out;
        slots_.emplace(id, Slot(Supervisor(config(id, driver), stored->driver, now, &out, stored->queue)));
        return run(id, std::move(out), now, false);
    }
    const bool none = !driver.problem.empty();
    Supervisor& sup = it->second.sup;
    return run(id, sup.configure(none ? std::vector<std::string>{} : driver.command, none ? sup.cwd() : driver.cwd,
        options_.table, now), now, false);
}

bool Drivers::shutdown(Millis now) {
    for (auto& [id, t] : slots_) {
        if (!run(id, t.sup.shutdown(now), now, false)) {
            return false;
        }
    }
    return true;
}

bool Drivers::alive() const {
    return std::any_of(slots_.begin(), slots_.end(), [](const auto& t) { return t.second.proc != nullptr; });
}

const Supervisor* Drivers::supervisor(sync::SessionId id) const {
    auto it = slots_.find(id);
    return it == slots_.end() ? nullptr : &it->second.sup;
}

pid_t Drivers::pid(sync::SessionId id) const {
    auto it = slots_.find(id);
    return it == slots_.end() || !it->second.proc ? -1 : it->second.proc->pid;
}

// An output as the changes of one step of the store: the prompts it took from the queue's head, its entries, and the
// supervisor's durable state when it changed.
std::vector<sync::SessionChange> Drivers::changes(const Output& out, const Supervisor& sup) const {
    std::vector<sync::SessionChange> changes;
    for (std::size_t i = 0; i < out.taken; ++i) {
        changes.push_back(sync::takeQueued());
    }
    for (const Effect& e : out.effects) {
        changes.push_back(sync::SessionChange{entry(entryKind(e.what), e.text), {}});
    }
    if (out.durable) {
        changes.push_back(sync::SessionChange{nullptr, {}, sup.state()});
    }
    return changes;
}

// The friend's text still growing, as the session's open entry, and what the friend is doing (the window's state
// cursor): in memory only, never in the store. True when either changed.
bool Drivers::showOpen(sync::SessionId id, const Slot& t) {
    const bool open = hub_->setOpen(id, t.sup.gathered());
    const bool activity = hub_->setActivity(id, t.sup.activity());
    return open || activity;
}

// The step's effects and durable state, in one step of the store.
bool Drivers::commit(sync::SessionId id, const Output& out, bool* changed) {
    const std::vector<sync::SessionChange> changes = this->changes(out, slots_.at(id).sup);
    if (changes.empty()) {
        return true;
    }
    std::string why;
    if (!hub_->commit(id, changes, &why)) {
        log(id, "the store did not take the driver's step: " + why);
        return false;
    }
    if (changed != nullptr) {
        *changed = true;
    }
    return true;
}

// An output: committed (unless it was already, with the window's action), then its commands carried out in order;
// what carrying them out gives (a driver that could not start) goes the same way.
bool Drivers::run(sync::SessionId id, Output out, Millis now, bool committed, bool* changed) {
    std::vector<Output> queue;
    queue.push_back(std::move(out));
    for (std::size_t i = 0; i < queue.size(); ++i) {
        Output o = std::move(queue[i]);
        if (!(i == 0 && committed) && !commit(id, o, changed)) {
            return false;
        }
        if (o.credential) {
            log(id, "the driver's credential: " + std::string(driver::billingName(o.credential->billing)) +
                (o.credential->plan.empty() ? std::string() : " (" + o.credential->plan + ")"));
        }
        if (o.sessionCostUsd) {
            char cost[64];
            std::snprintf(cost, sizeof cost, "the session has cost $%.4f so far", *o.sessionCostUsd);
            log(id, cost);
        }
        Slot& t = slots_.at(id);
        std::vector<Output> more;
        for (const Command& c : o.commands) {
            carryOut(id, t, c, more, now);
        }
        for (Output& m : more) {
            queue.push_back(std::move(m));
        }
    }
    if (showOpen(id, slots_.at(id)) && changed != nullptr) {
        *changed = true;
    }
    return true;
}

void Drivers::carryOut(sync::SessionId id, Slot& t, const Command& c, std::vector<Output>& more, Millis now) {
    Process* p = t.proc.get();
    switch (c.kind) {
        case Command::Kind::Start: {
            if (p != nullptr) {
                log(id, "a driver start while one runs was ignored: a bug in the supervisor");
                return;
            }
            auto proc = std::make_unique<Process>();
            std::string why;
            proc->pid = spawnDriver(c.argv, &proc->in, &proc->out, &why);
            if (proc->pid < 0) {
                more.push_back(t.sup.exited("could not be started: " + why, now));
                return;
            }
            log(id, "driver " + std::to_string(proc->pid) + " started: " + c.argv[0]);
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
void Drivers::readSome(Slot& t, Output& out, Millis now, std::size_t limit) {
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
void Drivers::collect(sync::SessionId id, Slot& t, Output& out, Millis now) {
    Process& p = *t.proc;
    siginfo_t info{};
    if (::waitid(P_PID, static_cast<id_t>(p.pid), &info, WEXITED | WNOHANG | WNOWAIT) != 0 || info.si_pid != p.pid) {
        return;
    }
    // What it started in its process group (its harness) goes with it, while its pid, not yet collected, still names
    // the group: a next driver for the session never works beside it.
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
    log(id, "driver " + std::to_string(p.pid) + " " + howEnded(status));
    t.proc.reset();
    append(out, t.sup.exited(howEnded(status), now));
}

std::vector<pollfd> Drivers::fds() const {
    std::vector<pollfd> out;
    for (const auto& [id, t] : slots_) {
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
    if (!unresolved_.empty()) {
        wait = resolveAt_ > now ? resolveAt_ - now : 0;
    }
    for (const auto& [id, t] : slots_) {
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
    if (!unresolved_.empty() && now >= resolveAt_) {
        resolveAt_ = now + kResolveMs;
        const std::set<sync::SessionId> ids = unresolved_;
        for (sync::SessionId id : ids) {
            const ResolvedDriver r = resolve(id);
            if (!r.problem.empty()) {
                continue;
            }
            if (!showResolved(id, r)) {
                return false;
            }
            if (changed != nullptr) {
                *changed = true;
            }
        }
    }
    struct Report {
        sync::SessionId id;
        std::string account;
        std::optional<bool> signedIn;
        bool usageLimit;
    };
    std::vector<Report> reports;
    for (auto& [id, t] : slots_) {
        Output out;
        if (t.proc) {
            flushInput(*t.proc, now);
            readSome(t, out, now, kReadChunk);
        }
        if (t.proc) {
            collect(id, t, out, now);
        }
        if (t.proc && t.proc->brokenSince && !t.proc->brokenTold && now - *t.proc->brokenSince >= kPipeGraceMs) {
            t.proc->brokenTold = true;
            append(out, t.sup.closed(t.proc->broken, now));
        }
        append(out, t.sup.tick(now));
        if ((out.signedIn || out.usageLimit) && !out.account.empty()) {
            reports.push_back(Report{id, out.account, out.signedIn, out.usageLimit});
        }
        const bool any = !out.effects.empty() || out.durable || !out.commands.empty() || out.taken != 0;
        if (any) {
            if (!commit(id, out, changed)) {
                return false;
            }
            if (!run(id, std::move(out), now, true, changed)) {
                return false;
            }
        } else if (showOpen(id, t) && changed != nullptr) {
            *changed = true;    // text that only grew changes nothing durable: it shows as the open entry
        }
    }
    // What the drivers said of their accounts, once every slot is served: the hub's sign-in state, and a session
    // whose account reached its usage limit moved to its friend's next signed-in account (a rotation changes the
    // session's settings, which reach its supervisor through configured()).
    for (const Report& r : reports) {
        if (hub_ == nullptr) {
            break;
        }
        if (r.signedIn) {
            hub_->reportSignedIn(r.account, *r.signedIn);
            log(r.id, "the account " + r.account + (*r.signedIn ? " is signed in" : " needs sign-in"));
        }
        const sync::StoredSession* s = store_.find(r.id);
        if (r.usageLimit && s != nullptr && s->info.account == r.account) {
            std::string why;
            if (hub_->rotate(r.id, &why)) {
                log(r.id, "the account " + r.account + " reached its usage limit; the session moved to " +
                    store_.find(r.id)->info.account);
            } else {
                log(r.id, "the account " + r.account + " reached its usage limit: " + why);
            }
            if (changed != nullptr) {
                *changed = true;
            }
        }
    }
    return hub_ == nullptr || !hub_->failed();
}

} // namespace nexus::server
