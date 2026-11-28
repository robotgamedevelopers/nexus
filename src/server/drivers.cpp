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
#include <filesystem>

#include "pr/json.h"
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
    for (const auto& [minutes, used] : from.quota) {
        into.quota[minutes] = used;
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

// What goes before the prompt of a driver started afresh on another account or in another directory: the conversation's newest entries,
// oldest first, within kSummaryBytes, each said whose it is. Only the turns before `prompt`: the transcript already
// ends with it (the person's entry, shown when it left the queue, and what followed it), and the send carries it
// after.
std::string summaryOf(const std::string& aiName, const sync::StoredSession& session,
                      const std::string& prompt, const std::vector<std::string>* waiting = nullptr) {
    const auto& transcript = session.transcript;
    using Position = std::vector<sync::EntryRef>::const_reverse_iterator;
    const auto submitted = [&](Position pos) -> const std::string* {
        const auto& entry = **pos;
        if (entry.kind == EntryKind::Person) return &entry.text;
        if (entry.kind != EntryKind::Bus) return nullptr;
        const auto index = static_cast<std::size_t>(pos.base() - transcript.begin() - 1);
        const auto bus = session.busPrompts.find(index);
        return bus == session.busPrompts.end() ? nullptr : &bus->second;
    };
    const auto matches = [&](Position pos, const std::string& text) {
        const auto* input = submitted(pos);
        return input && *input == text;
    };
    bool busRequest = false;
    auto before = transcript.rbegin();
    if (waiting == nullptr) {
        while (before != transcript.rend() && !matches(before, prompt)) {
            ++before;
        }
        if (before == transcript.rend()) {
            before = transcript.rbegin();
        } else {
            busRequest = (*before)->kind == EntryKind::Bus;
            ++before;
        }
    }
    std::size_t later = waiting == nullptr ? 0 : waiting->size();
    bool skippedRequest = false;
    std::vector<std::string> parts;
    // Preserve recent tool outcomes even after a large streamed reply consumed the text budget.
    std::size_t toolsLeft = waiting == nullptr ? 0 : kSummaryBytes / 4;
    std::size_t textLeft = kSummaryBytes - toolsLeft;
    for (auto it = before; it != transcript.rend() && (textLeft || toolsLeft); ++it) {
        if (waiting != nullptr) {
            // Later submissions are already visible in the transcript and will get their own turns.
            if (later != 0 && matches(it, (*waiting)[later - 1])) {
                --later;
                continue;
            }
            if (!skippedRequest && matches(it, prompt)) {
                skippedRequest = true;   // the continuation always includes the original request in full
                continue;
            }
            if ((*it)->kind == EntryKind::Notice) {
                continue;
            }
        }
        const char* who = (*it)->kind == EntryKind::Person ? "The person" :
            (*it)->kind == EntryKind::AI ? "You" : (*it)->kind == EntryKind::Tool ? "Tool activity" :
            (*it)->kind == EntryKind::Bus ? "Message bus" : "nexus";
        std::size_t& left = waiting != nullptr && (*it)->kind == EntryKind::Tool ? toolsLeft : textLeft;
        std::string part = std::string(who) + ": ";
        if (left <= part.size() + 1) {
            left = 0;
            continue;
        }
        // The bus envelope holds the subject and attachment path that the small display body may omit.
        const auto* envelope = (*it)->kind == EntryKind::Bus ? submitted(it) : nullptr;
        part += tailOf(envelope ? *envelope : (*it)->text, left - part.size() - 1) + "\n";
        left -= part.size();
        parts.push_back(std::move(part));
    }
    std::string text = "[nexus: this conversation with " + aiName + " continues in a fresh harness session. "
        "Its newest entries so far, oldest first:]\n";
    for (auto it = parts.rbegin(); it != parts.rend(); ++it) {
        text += *it;
    }
    if (waiting != nullptr) return text;
    return text + (busRequest ? "[nexus: the bus message follows.]\n\n" : "[nexus: the person's prompt follows.]\n\n");
}

class PosixHost : public Drivers::Host {
public:
    pid_t spawn(const std::vector<std::string>& argv, int* in, int* out, std::string* why) override {
        return spawnDriver(argv, in, out, why);
    }
    ssize_t write(int fd, const char* data, std::size_t size, int* err) override {
        const ssize_t n = ::write(fd, data, size);
        *err = n < 0 ? errno : 0;
        return n;
    }
    ssize_t read(int fd, char* data, std::size_t size, int* err) override {
        const ssize_t n = ::read(fd, data, size);
        *err = n < 0 ? errno : 0;
        return n;
    }
    void close(int fd) override { ::close(fd); }
    void killGroup(pid_t pid, int sig) override { ::killpg(pid, sig); }
    bool exited(pid_t pid) override {
        siginfo_t info{};
        return ::waitid(P_PID, static_cast<id_t>(pid), &info, WEXITED | WNOHANG | WNOWAIT) == 0 && info.si_pid == pid;
    }
    std::string reap(pid_t pid) override {
        int status = 0;
        while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
        }
        return howEnded(status);
    }
};

Drivers::Host& posixHost() {
    static PosixHost host;
    return host;
}

} // namespace

Drivers::Drivers(const sync::SessionStore& store, Options options)
    : store_(store), options_(std::move(options)), host_(options_.host != nullptr ? options_.host : &posixHost()) {}

void Drivers::log(sync::SessionId id, const std::string& what) const {
    if (!options_.quiet) {
        std::fprintf(stderr, "nexus-server: session %llu: %s\n", static_cast<unsigned long long>(id), what.c_str());
    }
}

Drivers::~Drivers() {
    for (auto& [id, t] : slots_) {
        if (!t.proc) {
            continue;
        }
        if (t.proc->in >= 0) {
            host_->close(t.proc->in);
        }
        if (t.proc->out >= 0) {
            host_->close(t.proc->out);
        }
        host_->killGroup(t.proc->pid, SIGKILL);
        host_->reap(t.proc->pid);
    }
}

std::string Drivers::aiOf(sync::SessionId id) const {
    const sync::StoredSession* stored = store_.find(id);
    return stored == nullptr ? std::string() : stored->info.aiName;
}

// With no registry held (the committed files did not read at the start) no AI is known to be in ais.json, so
// none is listed: the deny direction, as limitedFor reads no registry as limited (models/Supervisor.tla NoSendUnknown).
bool Drivers::listed(sync::SessionId id) const {
    const accounts::Registry* registry = hub_ != nullptr ? hub_->accounts() : nullptr;
    return (hub_ && hub_->isPrimary(id)) || (registry && registry->aiNamed(aiOf(id)));
}

// The session's driver: its AI's line of the drivers file. The supervisor binds any `--account` argument at
// process start. A command that takes an account starts for a session with one only: a session on no account (one
// whose AI had no signed-in account left when its own was unlinked) gets the reason and how to link one. A session
// using a provider other than its AI's configured harness needs an account on that harness.
ResolvedDriver Drivers::resolve(sync::SessionId id) const {
    const std::string aiName = aiOf(id);
    DriverTable table;
    ResolvedDriver r = resolveAI(aiName, &table);
    const sync::StoredSession* stored = store_.find(id);
    const bool primary = hub_ && hub_->isPrimary(id);
    if (primary && stored) {
        if (!table.errors.empty() || !table.primary || !table.primary->matches(stored->info) ||
                !table.primaryDrivers.contains(table.primary->harness))
            return {{}, {}, "The primary harness configuration changed. Reconnect to the current workspace."};
        r = {table.primaryDrivers.at(table.primary->harness), table.primary->cwd, {}};
        if (std::find(r.command.begin(), r.command.end(), "--account") == r.command.end())
            r.command.insert(r.command.end(), {"--account", stored->info.account});
        if (!table.primary->resume.empty() && stored->driver.nextTurn == 1 && stored->driver.session.empty())
            r.command.insert(r.command.end(), {"--resume", table.primary->resume});
    }
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
    // The session's instance (none named: its AI's default): its machine must be this server's (a driver on another
    // machine is roadmap work, docs/roadmap.sexp), and its directory is where the driver works.
    if (const accounts::Instance* in = instanceOf(info)) {
        if (!sameMachine(in->machine, options_.machine, options_.hostName)) {
            const std::string here = options_.machine.empty() ? std::string("a machine with no name (gethostname "
                "failed, and the server's config sets no machine)") : options_.machine;
            return refuse("Session " + std::to_string(id) + " belongs to " + aiName + "'s instance " + in->name +
                ", on the machine " + in->machine + ", and this server runs on " + here + "; nexus-server does not "
                "yet start a driver on another machine, so the prompt was not sent. Make a session of an instance of " +
                aiName + " on this machine, or add one in ais.json (docs/accounts.md)" +
                (options_.machine.empty() ? std::string(", and set `machine <name>` in the server's config") : "") +
                ".");
        }
        r.cwd = in->dir;
    }
    if (info.account.empty()) {
        if (takesAccount && hub_ != nullptr && hub_->accounts() != nullptr) {
            return refuse("Session " + std::to_string(id) + " with " + aiName + " runs on no account, and " +
                aiName + "'s driver needs one, so the prompt was not sent. Link an account to " + aiName +
                " in ais.json (its \"accounts\", and its \"default\") and run `nexus-server config reload`, then "
                "set this session to it; docs/accounts.md says how.");
        }
        return r;
    }
    // The account's provider, where the server holds accounts (before them, an account is any name, of no provider).
    const accounts::Account* held = hub_ != nullptr && hub_->accounts() != nullptr ?
        hub_->accounts()->account(info.account) : nullptr;
    auto line = table.ais.find(aiName);
    const std::string harness = primary ? info.harness : line == table.ais.end() ? std::string() : line->second.harness;
    if (held != nullptr && held->provider != harness) {
        return refuse("Session " + std::to_string(id) + " runs on the account " + info.account + ", whose provider is " +
            held->provider + ", but " + aiName + "'s driver in " + options_.table + " is " + harness + "'s, and "
            "nexus-server has no " + held->provider + " driver for it yet, so the prompt was not sent. Set the "
            "session to one of " + aiName + "'s " + harness + " accounts.");
    }
    if (held != nullptr && held->requiresVerifiedEmail()) {
        if (!takesAccount || options_.verifiedHelper.empty() || options_.authClaude.empty()) {
            return refuse("Configure the Claude sign-in helper, Claude executable and driver --account for " +
                          held->name + " to verify its email.");
        }
        r.command.insert(r.command.end(), {"--account-email", held->email, "--verified-helper", options_.verifiedHelper,
                                           "--auth-claude", options_.authClaude});
    }
    return r;
}

// The instance a session belongs to, as the hub holds its AI: the one it names, or its AI's default (its
// first) when it names none. Null when the hub holds no accounts, the AI has no instances, or (until the hub settles
// it) the one named is gone.
const accounts::Instance* Drivers::instanceOf(const sync::SessionInfo& info) const {
    const accounts::AI* fr = hub_ != nullptr && hub_->accounts() != nullptr ?
        hub_->accounts()->aiNamed(info.aiName) : nullptr;
    if (fr == nullptr) {
        return nullptr;
    }
    if (info.instance.empty()) {
        return fr->instances.empty() ? nullptr : &fr->instances.front();
    }
    return fr->instance(info.instance);
}

// Whether the session's driver starts limited to its directory: the server decides, from the accounts and AIs it
// holds (accounts::limitedFor: the AI's "limit", always for an API-billed account, and limited when it holds none).
bool Drivers::limitedOf(const sync::SessionInfo& info) const {
    return accounts::limitedFor(hub_ != nullptr ? hub_->accounts() : nullptr, info.aiName, info.account);
}

// The directories a limited driver keeps out on top of its own (ais.json "limit_keeps_out"); none without files.
std::vector<std::string> Drivers::keepsOut() const {
    return hub_ != nullptr && hub_->accounts() != nullptr ? hub_->accounts()->keepsOut : std::vector<std::string>{};
}

DriverTable Drivers::readDriverTable(std::string& problem) const {
    std::string text;
    if (options_.readTable) {
        if (!options_.readTable(&text, &problem)) return {};
    } else {
        std::ifstream in(options_.tableNow ? options_.tableNow() : options_.table);
        if (in) {
            std::ostringstream all;
            all << in.rdbuf();
            text = all.str();
        } else if (errno != ENOENT) {
            problem = "nexus-server cannot read " + options_.table + " (" + std::strerror(errno) +
                "). Make the file readable by this user.";
            return {};
        }
    }
    return parseDriverTable(text, options_.home);
}

std::optional<sync::AccountRoutes> Drivers::accountRoutes() const {
    std::string problem;
    const DriverTable table = readDriverTable(problem);
    sync::AccountRoutes routes;
    if (problem.empty()) {
        for (const auto& [name, row] : table.ais) {
            const auto driver = table.drivers.find(row.harness);
            if (driver != table.drivers.end() &&
                    std::find(driver->second.begin(), driver->second.end(), "--account") != driver->second.end())
                routes.emplace(name, row.harness);
        }
    }
    return routes;
}

ResolvedDriver Drivers::resolveAI(const std::string& aiName, DriverTable* table) const {
    std::string problem;
    DriverTable parsed = readDriverTable(problem);
    if (!problem.empty()) {
        ResolvedDriver r;
        r.problem = std::move(problem);
        return r;
    }
    ResolvedDriver r = resolveDriver(parsed, aiName, options_.table);
    if (table != nullptr) {
        *table = std::move(parsed);
    }
    return r;
}

sync::SessionInfo Drivers::defaults(const std::string& aiName) const {
    // A server without a roster uses the legacy harness default. A named account supplies its own provider.
    return sync::SessionInfo{aiName, sync::kDefaultHarness, {}, {}, {}};
}

Config Drivers::config(sync::SessionId id, const ResolvedDriver& driver) const {
    Config c;
    const sync::StoredSession* stored = store_.find(id);
    c.name = aiOf(id);
    if (stored != nullptr) {
        c.model = stored->info.model;
        c.effort = stored->info.effort;
        c.account = stored->info.account;
        c.autoAccount = stored->info.autoAccount;
    }
    c.command = driver.command;
    c.cwd = driver.cwd;
    c.keepsOut = keepsOut();
    if (stored != nullptr) {
        const accounts::Instance* in = instanceOf(stored->info);
        c.here = in == nullptr || sameMachine(in->machine, options_.machine, options_.hostName);
        c.limited = limitedOf(stored->info);
    } else {
        c.limited = true;   // a session the store does not hold: limited, the safe direction
    }
    c.present = listed(id);
    c.summarize = [this, id, name = c.name](const std::string& prompt) {
        const sync::StoredSession* s = store_.find(id);
        return s == nullptr ? std::string() : summaryOf(name, *s, prompt);
    };
    c.continuationContext = [this, id, name = c.name] {
        const sync::StoredSession* s = store_.find(id);
        return s == nullptr ? std::string() :
            summaryOf(name, *s, s->driver.continuationPrompt, &s->driver.waiting);
    };
    c.configured = options_.table;
    c.timing = options_.timing;
    c.maxWaiting = hub_->limits().queue;
    c.maxWaitingBytes = hub_->limits().queueBytes;
    c.maxEntryBytes = hub_->limits().text;
    return c;
}

bool Drivers::start(Millis now) {
    started_ = true;
    for (sync::SessionId id : store_.ids()) {
        const sync::StoredSession* stored = store_.find(id);
        sync::DriverState state = stored->driver;
        const ResolvedDriver r = resolve(id);
        if (state.openTurn == 0 && state.waiting.empty() && state.continuationPrompt.empty() &&
            (stored->queue.empty() || !r.problem.empty() || !listed(id))) {
            // Nothing to run, or only queued prompts and no driver to give them (they stay queued, as they are): the
            // session shows idle, or down when its AI has no driver configured or is not in ais.json.
            showResolved(id, r);
            continue;
        }
        std::string dropped;
        if (!r.problem.empty() && !state.waiting.empty()) {
            dropped = std::to_string(state.waiting.size()) + " prompt" + (state.waiting.size() == 1 ? "" : "s") +
                " waiting for " + stored->info.aiName + " when nexus-server stopped could not be sent: " +
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
        // An AI whose driver no longer resolves has none: what is queued stays queued until it has one again. Its
        // directory stays the session's: a driver that does not resolve, or whose AI is out of ais.json (so
        // its instances are not read), names none, and a driver already running or waiting there is not moved by it.
        const bool none = !driver.problem.empty();
        const bool keepCwd = none || !listed(id);
        append(pendingOut_, pendingSup_->configure(none ? std::vector<std::string>{} : driver.command,
            keepCwd ? pendingSup_->cwd() : driver.cwd, options_.table, now));
    } else {
        const sync::StoredSession* stored = store_.find(id);
        pendingSup_.emplace(config(id, driver), stored != nullptr ? stored->driver : sync::DriverState{}, now,
            &pendingOut_, stored != nullptr ? stored->queue : std::vector<std::string>{});
    }
    return *pendingSup_;
}

std::vector<sync::SessionChange> Drivers::send(sync::SessionId id, const std::string& prompt) {
    return preparePrompt(id, prompt, nullptr);
}

std::vector<sync::SessionChange> Drivers::prepareBus(sync::SessionId id, const std::string& prompt, std::string& why) {
    why.clear();
    return preparePrompt(id, prompt, &why);
}

std::vector<sync::SessionChange> Drivers::preparePrompt(sync::SessionId id, const std::string& prompt,
                                                       std::string* busRefusal) {
    const Millis now = options_.clock();
    pendingSup_.reset();
    pendingOut_ = Output{};
    // An AI with no driver gets that notice first (what to fix in the drivers file); one with a driver but out of
    // ais.json gets the notice that says how to put it back.
    const ResolvedDriver r = resolve(id);
    if (!r.problem.empty()) {
        if (busRefusal) {
            *busRefusal = r.problem;
            return {};
        }
        return {sync::SessionChange{entry(EntryKind::Person, prompt), {}}, sync::SessionChange{entry(EntryKind::Notice,
            r.problem), {}}};
    }
    if (!listed(id)) {
        if (busRefusal) {
            *busRefusal = "The message bus recipient is not in the current AI roster.";
            return {};
        }
        const std::string name = aiOf(id);
        const bool none = hub_ == nullptr || hub_->accounts() == nullptr;
        return {sync::SessionChange{entry(EntryKind::Person, prompt), {}}, sync::SessionChange{entry(EntryKind::Notice,
            none ? "nexus-server holds no AIs list (its committed accounts.json and ais.json did not read "
                "when it started; server.log says why), so it does not know whether " + name + " is in it, and the "
                "prompt was not sent. Fix the files and run `nexus-server config reload` (docs/accounts.md); this "
                "session and its queued prompts are kept." :
            name + " is not in ais.json, so the prompt was not sent. Add " + name + " to ais.json and run "
            "`nexus-server config reload` (docs/accounts.md); this session and its queued prompts are kept."), {}}};
    }
    Supervisor& sup = pending(id, r, now);
    auto submitted = sup.prompt(prompt, now);
    if (busRefusal) {
        // Supervisor::prompt emits its admission first, before any effects of starting or advancing the turn.
        // A refused bus message stays in its inbox. An admitted one has its attributed entry written by the hub.
        if (submitted.effects.empty() || submitted.effects.front().what != Effect::What::Prompt) {
            *busRefusal = submitted.effects.empty() ? "The driver could not admit the message." : submitted.effects.front().text;
            return {};
        }
        submitted.effects.erase(submitted.effects.begin());
    }
    append(pendingOut_, std::move(submitted));
    return changes(pendingOut_, sup);
}

// The prompt at the end of the queue first: the supervisor's output follows it, and takes from the head.
std::vector<sync::SessionChange> Drivers::queue(sync::SessionId id, const std::string& prompt) {
    const Millis now = options_.clock();
    pendingSup_.reset();
    pendingOut_ = Output{};
    std::vector<sync::SessionChange> out = {sync::SessionChange{nullptr, prompt}};
    const ResolvedDriver r = resolve(id);
    if ((!r.problem.empty() || !listed(id)) && slots_.find(id) == slots_.end()) {
        return out;     // no driver to give it to, or its AI not in ais.json: it stays queued until then
    }
    Supervisor& sup = pending(id, r, now);
    append(pendingOut_, sup.queued(prompt, now));
    for (sync::SessionChange& c : changes(pendingOut_, sup)) {
        out.push_back(std::move(c));
    }
    return out;
}

// A stop of an open turn changes nothing in the store: its stop line is written once the hub has counted the action.
std::string Drivers::aiInfo(const std::string& name) const {
    if (name.empty() || name.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._-") != std::string::npos)
        return {};
    const auto dir = std::filesystem::path(options_.table).parent_path();
    std::ifstream input(dir / "ai-info" / (name + ".json"), std::ios::binary);
    // friend-info: where a launcher from before the friend names became AI still writes it. The word stays here only.
    if (!input) input.open(dir / "friend-info" / (name + ".json"), std::ios::binary);
    if (!input) return {};
    std::string data(16385, '\0');
    input.read(data.data(), static_cast<std::streamsize>(data.size()));
    data.resize(static_cast<size_t>(input.gcount()));
    std::unique_ptr<PrJson, decltype(&prJsonDestroy)> doc(prJsonCreate(), prJsonDestroy);
    if (data.size() > 16384 || !prJsonParse(doc.get(), {data.data(), data.size()}, nullptr) ||
        prJsonKind(doc.get(), 0) != PrJsonKind_Object) return {};
    return data;
}

std::string Drivers::catalog(const sync::SessionInfo& info) const {
    if (info.harness.empty() || info.harness.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_-") != std::string::npos)
        return {};
    const auto now = options_.clock();
    auto& cached = catalogs_[info.harness];
    if (now < cached.nextRead) return cached.json;
    cached.nextRead = now + 1000;
    cached.json.clear();
    const auto path = std::filesystem::path(options_.table).parent_path() / "catalogs" / (info.harness + ".json");
    std::ifstream input(path, std::ios::binary);
    if (!input) return {};
    std::string json(16385, '\0');
    input.read(json.data(), static_cast<std::streamsize>(json.size()));
    json.resize(static_cast<size_t>(input.gcount()));
    if (json.size() > 16384) return {};
    std::unique_ptr<PrJson, decltype(&prJsonDestroy)> parsed(prJsonCreate(), prJsonDestroy);
    if (!prJsonParse(parsed.get(), {json.data(), json.size()}, nullptr) ||
            prJsonKind(parsed.get(), prJsonMember(parsed.get(), 0, {"models", 6})) != PrJsonKind_Object) return {};
    cached.json = std::move(json);
    return cached.json;
}

std::vector<sync::SessionChange> Drivers::resetContext(sync::SessionId id, const sync::DriverState& state, bool compact) {
    pendingSup_.reset();
    pendingOut_ = {};
    auto slot = slots_.find(id);
    if (slot == slots_.end()) return Responder::resetContext(id, state, compact);
    pendingSup_ = slot->second.sup;
    pendingOut_ = pendingSup_->resetContext(compact, options_.clock());
    return changes(pendingOut_, *pendingSup_);
}

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

// Brings a slot's supervisor to the session's settings, account, limit, place and resolved command as the store, the
// accounts and the drivers file hold them now, tightening before opening: a move off this machine or a command that no
// longer resolves holds back before anything can take from the queue, and the limit and account are set before a
// resolved command or a move onto this machine lets what waits go on.
Output Drivers::syncSlot(sync::SessionId id, Supervisor& sup, Millis now) {
    const sync::StoredSession* s = store_.find(id);
    if (s == nullptr) {
        return {};
    }
    const ResolvedDriver r = resolve(id);
    if (r.problem.empty()) {
        unresolved_.erase(id);
    } else {
        unresolved_.insert(id);
    }
    const bool none = !r.problem.empty();
    std::string dir;
    bool here = true;
    if (const accounts::Instance* in = instanceOf(s->info)) {
        here = sameMachine(in->machine, options_.machine, options_.hostName);
        dir = in->dir;
    } else {
        // No instance (its AI has none now): the drivers file's directory, on this machine, so that a session
        // whose instance was elsewhere is never left not here.
        const ResolvedDriver line = resolveAI(s->info.aiName, nullptr);
        dir = line.problem.empty() ? line.cwd : sup.cwd();
    }
    Output out;
    if (!here) {
        append(out, sup.setPlace(dir, false, now));
    }
    if (none) {
        append(out, sup.configure({}, dir, options_.table, now));
    }
    append(out, sup.setOptions(s->info.model, s->info.effort, now));
    sup.acceptModelChoice(s->driver);
    append(out, sup.setAccount(s->info.account, now, s->info.autoAccount, &r.command));
    append(out, sup.setLimited(limitedOf(s->info), now));
    append(out, sup.setKeepsOut(keepsOut(), now));
    if (!none) {
        append(out, sup.configure(r.command, dir, options_.table, now));
    }
    if (here) {
        append(out, sup.setPlace(dir, true, now));
    }
    return out;
}

// The session's account, model, effort or instance changed, durably: a supervisor already running takes them from its
// next turn (one started later reads them from the store and the accounts). An account change ends its driver once no
// turn is open, so the next starts under the new account, with the same session and transcript; so does a change of
// the instance's directory (the next starts there, afresh when the harness session was made elsewhere) or a move of
// the instance to another machine (then none starts here). A session left with no instance works in the drivers file's
// directory, here.
void Drivers::configured(sync::SessionId id, const sync::SessionInfo& info) {
    (void)info;
    auto it = slots_.find(id);
    if (it == slots_.end()) {
        if (started_) {
            showResolved(id, resolve(id));
        }
        return;
    }
    const Millis now = options_.clock();
    run(id, syncSlot(id, it->second.sup, now), now, false);
}

void Drivers::made(sync::SessionId id) {
    showResolved(id, resolve(id));
}

bool Drivers::refreshSandbox(const std::set<std::string>& unsandboxed) {
    if (unsandboxed == unsandboxed_) return true;
    const auto now = options_.clock();
    for (auto& [id, slot] : slots_) {
        const auto* session = store_.find(id);
        if (!session) continue;
        const auto& name = session->info.aiName;
        const bool enabled = unsandboxed.contains(name);
        if (enabled != unsandboxed_.contains(name) &&
            !run(id, slot.sup.refreshSandbox(!enabled, now), now, false)) return false;
    }
    unsandboxed_ = unsandboxed;
    return true;
}

std::string Drivers::retireForSignOut(const std::string& account) {
    for (const auto& [id, slot] : slots_) {
        (void)id;
        const auto& sup = slot.sup;
        if ((sup.driverAccount() == account && (sup.phase() == Phase::Starting || sup.state().openTurn != 0)) ||
                (sup.account() == account && !sup.state().waiting.empty()))
            return "Finish or stop the work using " + account + ", then retry /logout.";
    }
    const auto now = options_.clock();
    for (auto& [id, slot] : slots_)
        if (slot.sup.driverAccount() == account && slot.sup.phase() == Phase::Idle &&
                !run(id, slot.sup.retireIdleLogin(now), now, false))
            return "Nexus could not retire the driver for " + account + ". Check the server and retry /logout.";
    return {};
}

void Drivers::signedIn(const std::string& account) {
    const Millis now = options_.clock();
    for (auto& [id, t] : slots_) {
        const sync::StoredSession* s = store_.find(id);
        if (s == nullptr || s->info.account != account || !t.sup.held()) {
            continue;
        }
        log(id, "the account " + account + " signed in: its driver may start again");
        run(id, t.sup.signedIn(now), now, false);
    }
}

// The hub has committed the notice of an AI taken out, or settled a session whose AI is back: its supervisor is
// told (stopping what runs, or letting what waits go on). Put back, it first takes the settings, account, command,
// limit, directories kept out and place that the session and the AI's record say now, which may differ from those
// it held when the AI was taken out, so that the next driver starts under them. With none, a session put back with
// prompts queued gets one, which reads them as it starts and takes the oldest; otherwise only what it shows changes.
void Drivers::present(sync::SessionId id, bool present) {
    auto it = slots_.find(id);
    if (it == slots_.end()) {
        showResolved(id, resolve(id));
        return;
    }
    const Millis now = options_.clock();
    if (it->second.sup.present() != present) {
        log(id, present ? "its AI is in ais.json again: what waits goes on" :
            "its AI was taken out of ais.json: what runs is stopped and its driver ended");
    }
    Output out;
    if (present) {
        out = syncSlot(id, it->second.sup, now);
    }
    append(out, it->second.sup.setPresent(present, now));
    run(id, std::move(out), now, false);
}

void Drivers::tellSecret(sync::SessionId id, const std::string& line) {
    auto it = slots_.find(id);
    if (id == 0 || it == slots_.end() || !it->second.proc) {
        return;
    }
    const Phase phase = it->second.sup.phase();
    if (phase != Phase::Idle && phase != Phase::Working && phase != Phase::Stopping) {
        return;
    }
    tell(*it->second.proc, line, options_.clock());
}

// A session's driver as the drivers file now says, for what the window shows of it: a session with no supervisor
// shows down when its AI has none configured, idle otherwise (nothing runs in it); one with a supervisor is told,
// and shows what that makes of it (Supervisor::activity).
// It is read again each kResolveMs while the AI has none (serve()). False when the store did not take a step.
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
        const bool listedNow = listed(id);
        if (!driver.problem.empty() || !listedNow || stored == nullptr || stored->queue.empty()) {
            hub_->setActivity(id, driver.problem.empty() && listedNow ? sync::Activity::Idle : sync::Activity::Down);
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

bool Drivers::busy() const {
    return std::any_of(slots_.begin(), slots_.end(),
        [](const auto& t) { return t.second.sup.state().openTurn != 0 || t.second.sup.phase() == Phase::Starting; });
}

const Supervisor* Drivers::supervisor(sync::SessionId id) const {
    auto it = slots_.find(id);
    return it == slots_.end() ? nullptr : &it->second.sup;
}

std::size_t Drivers::unwritten(sync::SessionId id) const {
    auto it = slots_.find(id);
    return it == slots_.end() || !it->second.proc ? 0 : it->second.proc->pending.size();
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

// The AI's text still growing, as the session's open entry, and what the AI is doing (the window's state
// cursor): in memory only, never in the store. True when either changed.
bool Drivers::showOpen(sync::SessionId id, const Slot& t) {
    const bool open = hub_->setOpen(id, t.sup.gathered());
    const bool activity = hub_->setActivity(id, t.sup.activity(), t.sup.activeAgents());
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
            proc->pid = host_->spawn(c.argv, &proc->in, &proc->out, &why);
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
                if (holding_) {
                    held_.push_back(Held{p->pid, 0});
                } else {
                    flushInput(*p, now);
                }
            }
            return;
        case Command::Kind::CloseInput:
            if (p != nullptr) {
                p->closeInput = true;
                if (holding_) {
                    held_.push_back(Held{p->pid, 0});
                } else {
                    flushInput(*p, now);
                }
            }
            return;
        case Command::Kind::Terminate:
        case Command::Kind::Kill:
            if (p != nullptr) {
                const int sig = c.kind == Command::Kind::Terminate ? SIGTERM : SIGKILL;
                if (holding_) {
                    held_.push_back(Held{p->pid, sig});
                } else {
                    host_->killGroup(p->pid, sig);
                }
            }
            return;
    }
}

// The batch is durable: what its steps told the drivers goes out in the order they told it, each write or close of a
// driver's input and each signal in turn, as carryOut does them unheld. A process's input is written up to where it was
// told (its bytes are queued in order, and a close waits for them); a write the pipe will not take now is finished by
// serve() later, as unheld. Then whatever an earlier pass left unwritten, which serve() kept while holding. Every
// process a signal was kept for is still uncollected (only serve() collects, and the server releases before it serves
// again), so its pid still names its group.
void Drivers::release(Millis now) {
    holding_ = false;
    for (const Held& h : held_) {
        if (h.sig != 0) {
            host_->killGroup(h.pid, h.sig);
            continue;
        }
        for (auto& [id, t] : slots_) {
            if (t.proc && t.proc->pid == h.pid) {
                flushInput(*t.proc, now);
            }
        }
    }
    held_.clear();
    for (auto& [id, t] : slots_) {
        if (t.proc) {
            flushInput(*t.proc, now);
        }
    }
}

// A line for the driver that is no step of its supervisor's (the server's features, a secret's answer): queued after
// what it was told before, and held like the rest until the batch is durable.
void Drivers::tell(Process& p, const std::string& line, Millis now) {
    p.pending += line;
    if (holding_) {
        held_.push_back(Held{p.pid, 0});
    } else {
        flushInput(p, now);
    }
}

// Writes what the driver will take now; a driver that stopped reading its input loses what is left.
void Drivers::flushInput(Process& p, Millis now) {
    while (!p.pending.empty() && p.in >= 0) {
        int err = 0;
        const ssize_t n = host_->write(p.in, p.pending.data(), p.pending.size(), &err);
        if (n > 0) {
            p.pending.erase(0, static_cast<std::size_t>(n));
        } else if (n < 0 && err == EINTR) {
            continue;
        } else if (n < 0 && err == EAGAIN) {
            return;
        } else {
            p.pending.clear();
            host_->close(p.in);
            p.in = -1;
            if (!p.brokenSince) {
                p.brokenSince = now;
                p.broken = "input";
            }
        }
    }
    if (p.closeInput && p.pending.empty() && p.in >= 0) {
        host_->close(p.in);
        p.in = -1;
    }
}

// Up to `limit` bytes of the driver's output, as messages to its supervisor.
void Drivers::readSome(sync::SessionId id, Slot& t, Output& out, Millis now, std::size_t limit) {
    Process& p = *t.proc;
    std::vector<driver::DriverReader::Item> items;
    char buffer[16 * 1024];
    std::size_t total = 0;
    while (p.out >= 0 && total < limit) {
        int err = 0;
        const ssize_t n = host_->read(p.out, buffer, sizeof buffer, &err);
        if (n > 0) {
            p.reader.feed(std::string_view(buffer, static_cast<std::size_t>(n)), items);
            total += static_cast<std::size_t>(n);
        } else if (n < 0 && err == EINTR) {
            continue;
        } else if (n < 0 && err == EAGAIN) {
            break;
        } else {
            p.reader.finish(items);
            host_->close(p.out);
            p.out = -1;
            if (!p.brokenSince) {
                p.brokenSince = now;
                p.broken = "output";
            }
        }
    }
    for (driver::DriverReader::Item& item : items) {
        if (!item.message) {
            append(out, t.sup.refused(item.error, now));
            continue;
        }
        const Phase before = t.sup.phase();
        const bool talking = before == Phase::Idle || before == Phase::Working || before == Phase::Stopping;
        if (const auto* send = std::get_if<driver::BusSend>(&*item.message)) {
            if (p.nativeBus && before == Phase::Working && t.sup.state().openTurn == send->turn) {
                mail_.push_back({id, p.pid, *send});
            } else {
                tell(p, driver::encode(driver::BusResult{send->nonce, {}, "Send from an active Nexus turn."}).line, now);
            }
            continue;
        }
        if (const driver::AskSecret* ask = std::get_if<driver::AskSecret>(&*item.message); ask != nullptr && talking) {
            if (options_.secrets) {
                asks_.push_back(Ask{id, *ask});
            }
            continue;   // a server that takes no secrets told the driver none, and skips its ask (rule 8)
        }
        append(out, t.sup.message(*item.message, now));
        const Phase after = t.sup.phase();
        // Its hello: what this server takes beyond version 1, before anything else it is told.
        if (before == Phase::Starting && (after == Phase::Idle || after == Phase::Working)) {
            const auto* hello = std::get_if<driver::Hello>(&*item.message);
            p.nativeBus = hello && std::find(hello->features.begin(), hello->features.end(), "native-bus") != hello->features.end();
            driver::ServerInfo info;
            if (options_.secrets) info.features.push_back("secrets");
            if (p.nativeBus) info.features.push_back("native-bus");
            if (!info.features.empty()) tell(p, driver::encode(info).line, now);
        }
    }
}

// The driver's exit, if it has exited: what it wrote before, then the exit.
void Drivers::collect(sync::SessionId id, Slot& t, Output& out, Millis now) {
    Process& p = *t.proc;
    if (!host_->exited(p.pid)) {
        return;
    }
    // What it started in its process group (its harness) goes with it, while its pid, not yet collected, still names
    // the group: a next driver for the session never works beside it.
    host_->killGroup(p.pid, SIGKILL);
    const std::string ended = host_->reap(p.pid);
    readSome(id, t, out, now, kReadChunk);
    if (p.out >= 0) {
        // Its output is still held open (by a process it started): what is read so far is all it gets.
        std::vector<driver::DriverReader::Item> rest;
        p.reader.finish(rest);
        for (driver::DriverReader::Item& item : rest) {
            append(out, item.message ? t.sup.message(*item.message, now) : t.sup.refused(item.error, now));
        }
        host_->close(p.out);
    }
    if (p.in >= 0) {
        host_->close(p.in);
    }
    log(id, "driver " + std::to_string(p.pid) + " " + ended);
    t.proc.reset();
    append(out, t.sup.exited(ended, now));
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
        if (t.sup.awaitingRotation()) {
            return 0;
        }
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
            if (!holding_) {
                flushInput(*t.proc, now);
            }
            readSome(id, t, out, now, kReadChunk);
        }
        if (t.proc) {
            collect(id, t, out, now);
        }
        if (t.proc && t.proc->brokenSince && !t.proc->brokenTold && now - *t.proc->brokenSince >= kPipeGraceMs) {
            t.proc->brokenTold = true;
            append(out, t.sup.closed(t.proc->broken, now));
        }
        append(out, t.sup.tick(now));
        // A committed checkpoint also requests rotation after a server restart between the quota report and move.
        if (hub_ && out.usageLimit) hub_->reportQuota(out.account, 0, 100);
        if (t.sup.awaitingRotation()) {
            out.account = t.sup.account();
            out.usageLimit = true;
        }
        if (hub_) {
            for (const auto& [minutes, used] : out.quota)
                hub_->reportQuota(out.account, minutes, used);
        }
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
    // whose account reached its usage limit moved to its AI's next signed-in account (a rotation changes the
    // session's settings, which reach its supervisor through configured()).
    for (const Report& r : reports) {
        if (hub_ == nullptr) {
            break;
        }
        if (r.signedIn) {
            hub_->reportSignedIn(r.account, *r.signedIn);
            log(r.id, "the account " + r.account + (*r.signedIn ? " is signed in" : " needs sign-in"));
            if (changed != nullptr) {
                *changed = true;    // one that needs sign-in may have opened a secret request (Hub::reportSignedIn)
            }
        }
        const sync::StoredSession* s = store_.find(r.id);
        if (r.usageLimit && s != nullptr && s->info.account == r.account) {
            std::string why;
            const auto exhausted = s->driver.exhaustedAccounts;
            bool noEligible = false;
            if (hub_->rotate(r.id, &why, exhausted, &noEligible)) {
                log(r.id, "the account " + r.account + " reached its usage limit; the session moved to " +
                    store_.find(r.id)->info.account);
            } else {
                log(r.id, "the account " + r.account + " reached its usage limit: " + why);
                if (!noEligible) {
                    return false;   // the move or notice failed to commit; recover the checkpoint on restart
                }
                auto it = slots_.find(r.id);
                if (it != slots_.end() && !run(r.id, it->second.sup.rotationExhausted(), now, false, changed)) {
                    return false;
                }
            }
            if (changed != nullptr) {
                *changed = true;
            }
        }
    }
    // Receiving can start another driver. Finish traversing slots before admitting peer turns.
    for (const auto& send : mail_) {
        auto it = slots_.find(send.session);
        if (!hub_ || it == slots_.end() || !it->second.proc || it->second.proc->pid != send.pid) continue;
        const auto& m = send.message;
        std::string why;
        auto receipt = hub_->sendBus(send.session, m.nonce, m.to, m.subject, m.body, m.replyTo, why);
        if (hub_->failed()) return false;
        if (changed && !receipt.empty()) *changed = true;
        auto sender = slots_.find(send.session);
        if (sender != slots_.end() && sender->second.proc && sender->second.proc->pid == send.pid)
            tell(*sender->second.proc, driver::encode(driver::BusResult{m.nonce, receipt, why}).line, now);
    }
    mail_.clear();
    // The drivers' asks for secrets: each opens a request, or is answered at once (busy, no-client); a malformed one
    // ends its driver as a line refused (docs/secrets.md). The answer goes back through tellSecret().
    const std::vector<Ask> asks = std::move(asks_);
    asks_.clear();
    for (const Ask& a : asks) {
        if (hub_ == nullptr) {
            break;
        }
        std::string refused;
        const bool asked = hub_->askSecret(sync::SecretAsker{a.id, a.ask.ask, aiOf(a.id)}, a.ask.account,
                                           a.ask.what, a.ask.why, &refused);
        if (changed != nullptr) {
            *changed = true;
        }
        auto it = slots_.find(a.id);
        if (!asked && !hub_->failed() && it != slots_.end()) {
            log(a.id, "the driver's ask for a secret was refused: " + refused);
            if (!run(a.id, it->second.sup.refused("its ask-secret: " + refused, now), now, false, changed)) {
                return false;
            }
        }
    }
    return hub_ == nullptr || !hub_->failed();
}

} // namespace nexus::server
