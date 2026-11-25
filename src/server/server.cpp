#include "server/server.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/param.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <sstream>
#include <thread>
#include <vector>

#include "link/bootstrap.h"
#include "link/mosh_link.h"
#include "server/builds.h"
#include "server/bus_inbox.h"
#include "server/config_repo.h"
#include "server/drain.h"
#include "server/driver_table.h"
#include "server/drivers.h"
#include "server/durable.h"
#include "server/flush_gate.h"
#include "server/home.h"
#include "server/migration.h"
#include "server/pass.h"
#include "server/login_life.h"
#include "server/secret_files.h"
#include "server/secret_lines.h"
#include "server/service_manifest.h"
#include "server/service_process.h"
#include "server/signin_runner.h"
#include "server/start_order.h"
#include "server/supervise_state.h"
#include "store/fs.h"
#include "store/store.h"
#include "sync/formats.h"
#include "sync/hub.h"

extern char** environ;

namespace nexus::server {

namespace {

constexpr std::size_t kMaxLinks = 64;      // windows' links (mosh sessions) at once
constexpr int kControlTimeoutMs = 2000;     // for one request or reply on the control socket
// The last-resort bound on `new` (ask): past the started server's whole wait for the store's lock (kHeldWaitMs) and a
// stop's end, so that only a server neither serving nor exiting reaches it.
constexpr std::uint64_t kNewLimitMs = 30000;
constexpr int kMaxWaitMs = 1000;
// The longest a stopping server waits for its windows to acknowledge that their sessions ended: an acknowledgement
// takes one round trip, which on loopback or a tailnet is well under it (chosen, not measured past that).
constexpr int kLinkShutdownMs = 500;
constexpr std::uint64_t kWaitProgressMs = 1000;    // how often a start waiting for the store's lock says so
constexpr const char* kSocketName = "server.sock";  // the control socket, in the server's directory

volatile sig_atomic_t gStop = 0;

void onSignal(int) {
    gStop = 1;
}

// This machine's host name, as gethostname gives it; empty when it fails (serverMachine() cuts it short).
std::string hostName() {
    char name[256] = {};
    if (gethostname(name, sizeof name - 1) != 0) {
        return std::string();
    }
    return std::string(name);
}

// Milliseconds on the steady clock: the drivers' deadlines.
Millis nowMs() {
    return static_cast<Millis>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

// Milliseconds since 1970 on the system's clock: a stored login's age (server/login_life.h), against its file's time.
long long wallMs() {
    return static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}

// How often the stored logins' ends are checked: an hour, so a notice is at most an hour late against days of warning.
constexpr Millis kLoginCheckMs = 60ULL * 60 * 1000;

std::string errnoText(const std::string& what) {
    return what + ": " + std::strerror(errno);
}

// The user on the other end of a connected Unix socket: getpeereid on macOS, SO_PEERCRED on Linux.
bool peerUid(int fd, uid_t* uid) {
#if defined(__APPLE__)
    gid_t gid = 0;
    return getpeereid(fd, uid, &gid) == 0;
#else
    ucred cred{};
    socklen_t size = sizeof cred;
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &size) != 0 || size != sizeof cred) {
        return false;
    }
    *uid = cred.uid;
    return true;
#endif
}

bool socketAddress(const std::string& path, sockaddr_un* addr) {
    std::memset(addr, 0, sizeof *addr);
    addr->sun_family = AF_UNIX;
    if (path.size() >= sizeof addr->sun_path)
        return false;
    std::memcpy(addr->sun_path, path.c_str(), path.size() + 1);
    return true;
}

// Binds `sock` to `name` in the directory held by `dir`, whatever path that directory has now: on Linux through the
// descriptor's /proc link, on macOS (which has no bind relative to a directory) by changing into it for the call and
// back (by `path`, the name the server was given, only when the working directory cannot be opened to come back to).
// False, with why, on failure; the working directory is always put back.
bool bindIn(int sock, int dir, const char* name, const std::string& path, std::string* why) {
#if defined(__APPLE__)
    const int here = ::open(".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (here < 0) {
        sockaddr_un byPath;
        if (!socketAddress(path, &byPath) ||
                bind(sock, reinterpret_cast<const sockaddr*>(&byPath), sizeof byPath) != 0) {
            *why = std::strerror(errno);
            return false;
        }
        return true;
    }
    sockaddr_un addr;
    socketAddress(name, &addr);
    bool bound = false;
    if (fchdir(dir) != 0) {
        *why = errnoText("cannot change into the server's directory");
    } else {
        bound = bind(sock, reinterpret_cast<const sockaddr*>(&addr), sizeof addr) == 0;
        if (!bound) {
            *why = std::strerror(errno);
        }
        if (fchdir(here) != 0) {
            std::fprintf(stderr, "nexus-server: %s\n", errnoText("cannot change back to the working directory").c_str());
        }
    }
    close(here);
    return bound;
#else
    (void)path;
    sockaddr_un addr;
    if (!socketAddress("/proc/self/fd/" + std::to_string(dir) + "/" + name, &addr)) {
        *why = "the server directory's descriptor path is too long";
        return false;
    }
    if (bind(sock, reinterpret_cast<const sockaddr*>(&addr), sizeof addr) != 0) {
        *why = std::strerror(errno);
        return false;
    }
    return true;
#endif
}

// A connection to the control socket, or -1 with errno set.
int connectControl(const std::string& path) {
    sockaddr_un addr;
    if (!socketAddress(path, &addr)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    if (connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof addr) != 0) {
        const int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }
    return fd;
}

bool readLine(int fd, std::string* line, int timeoutMs);
bool writeAll(int fd, const std::string& s);

// The question a start (and `new`) asks the control socket, and the one answer that counts as a server serving: a
// connection alone does not, as a server that has accepted `stop` still holds its socket until its loop ends (and the
// kernel accepts connections on it meanwhile), and then closes it with them unanswered.
constexpr const char* kStatusRequest = "status";
constexpr const char* kServingReply = "SERVING";
// The wait for that answer: past one connection's read (kControlTimeoutMs) that a serving server may be in.
constexpr int kStatusReplyMs = 3000;

// Whether a server answers on `socket` that it serves: SERVING; or, from a server from before this question (which
// takes no store lock and knows no `status`), the refusal it gives every request it does not know, which it gives only
// from its serving loop, as it accepts nothing once it has accepted `stop`. Anything else (no socket, no connection, no
// reply in kStatusReplyMs, the connection closed unanswered, any other line) is not serving.
bool serving(const std::string& socket) {
    const int fd = connectControl(socket);
    if (fd < 0) {
        return false;
    }
    std::string reply;
    const bool replied = writeAll(fd, std::string(kStatusRequest) + "\n") && readLine(fd, &reply, kStatusReplyMs);
    close(fd);
    return replied && (reply == kServingReply || reply.rfind("ERROR unknown request", 0) == 0);
}

// One line from fd, without its newline, within the timeout. False on timeout, error or end before a newline.
bool readLine(int fd, std::string* line, int timeoutMs) {
    line->clear();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    for (;;) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now()).count();
        if (left <= 0)
            return false;
        pollfd p{fd, POLLIN, 0};
        if (::poll(&p, 1, static_cast<int>(left)) <= 0)
            continue;
        char c;
        const ssize_t n = read(fd, &c, 1);
        if (n <= 0)
            return false;
        if (c == '\n')
            return true;
        if (line->size() > 4096)
            return false;
        line->push_back(c);
    }
}

bool writeAll(int fd, const std::string& s) {
    std::size_t at = 0;
    while (at < s.size()) {
        const ssize_t n = write(fd, s.data() + at, s.size() - at);
        if (n <= 0)
            return false;
        at += static_cast<std::size_t>(n);
    }
    return true;
}

// The directory: made 0700 if missing; it must be a directory this user owns, and it is set to 0700.
bool ensureDir(const std::string& dir, std::string* why) {
    if (mkdir(dir.c_str(), 0700) != 0 && errno != EEXIST) {
        *why = errnoText("cannot make " + dir);
        return false;
    }
    struct stat st;
    if (lstat(dir.c_str(), &st) != 0) {
        *why = errnoText("cannot read " + dir);
        return false;
    }
    if (!S_ISDIR(st.st_mode) || st.st_uid != getuid()) {
        *why = dir + " is not a directory of this user's; move it aside";
        return false;
    }
    if ((st.st_mode & 0777) != 0700 && chmod(dir.c_str(), 0700) != 0) {
        *why = errnoText("cannot set " + dir + " to mode 0700");
        return false;
    }
    return true;
}

// The server's home: ~/.nexus first, when the directory is the default under it, then the directory itself.
bool ensureHome(const Paths& p, std::string* why) {
    return (p.top.empty() || ensureDir(p.top, why)) && ensureDir(p.dir, why);
}

// The server's config: none is fine; a malformed line stops the server, naming the file and the line; an unknown key
// is logged.
bool readConfig(const std::string& path, ServerSettings* settings, std::string* why) {
    const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        if (errno == ENOENT) {
            return true;
        }
        *why = errnoText("cannot read " + path) + "; make it readable by you, or remove it";
        return false;
    }
    std::string text;
    char buf[4096];
    for (;;) {
        const ssize_t n = read(fd, buf, sizeof buf);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n < 0) {
            *why = errnoText("cannot read " + path);
            close(fd);
            return false;
        }
        if (n == 0) {
            break;
        }
        text.append(buf, static_cast<std::size_t>(n));
    }
    close(fd);
    std::vector<std::string> warnings;
    const bool ok = readServerConfig(text, path, settings, &warnings, why);
    for (const std::string& w : warnings) {
        std::fprintf(stderr, "nexus-server: %s\n", w.c_str());
    }
    return ok;
}

// A number of 1 to `digits` decimal digits, from 1.
bool positive(const std::string& num, std::size_t digits, std::uint64_t* out) {
    if (num.empty() || num.size() > digits || num.find_first_not_of("0123456789") != std::string::npos) {
        return false;
    }
    *out = std::strtoull(num.c_str(), nullptr, 10);
    return *out != 0;
}

// One window's link: a mosh session, with the client it named. (The sessions with friends are the hub's.)
struct Link {
    std::uint64_t id;
    std::string client;     // empty: a session that did not name its client
    std::unique_ptr<link::ServerSession> link;
    std::string signIn;     // the claim token its window claims sign-ins by (sync/signin.h): its state carries their URLs
};

// A test seam, set only by tests/link/server_lock.cpp: the start's wait for a lock held by a writer that does not
// answer (server/start_order.h kHeldWaitMs), in milliseconds, so that a test sees it run out without waiting 10 s.
constexpr const char* kHeldWaitVariable = "NEXUS_TEST_HELD_WAIT_MS";

class Server : private StartSteps, private PassSteps {
public:
    explicit Server(const Paths& p) : paths_(p) {}
    ~Server() override {
        if (dirFd_ >= 0) {
            close(dirFd_);
        }
    }
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;
    int run();

private:
    // The start's steps (server/start_order.h), each taken through take_step.
    StartOrder::Outcome probe(std::string* why) override { return probeControl(why); }
    StartOrder::Outcome open(bool unlocked, std::string* why) override;
    StartOrder::Outcome listen(std::string* why) override { return listenControl(why); }
    StartOrder::Outcome restore(std::string* why) override {
        return restoreAll(why) ? StartOrder::Outcome::Ok : StartOrder::Outcome::Failed;
    }
    void pause(std::uint64_t ms) override { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
    std::uint64_t now_ms() override { return static_cast<std::uint64_t>(nowMs()); }

    // One pass's steps (server/pass.h, models/LoopPass.tla), each taken through take_pass_step.
    void tick() override;
    PassOrder::Outcome wait(std::string* why) override;
    PassOrder::Outcome hold(bool holdDrivers, std::string* why) override;
    PassOrder::Outcome apply(std::string* why) override;
    PassOrder::Outcome flush(std::string* why) override;
    void release() override;
    void state() override;

    struct Pass {
        std::vector<pollfd> fds;
        std::vector<std::size_t> owner;
        std::size_t linkFds = 0;
        bool busRosterRefreshed = false;
    };
    Pass pass_;

    StartOrder::Outcome openStore(std::string* why);
    StartOrder::Outcome probeControl(std::string* why);
    StartOrder::Outcome listenControl(std::string* why);
    bool restoreAll(std::string* why);
    // The control socket's file removed, only when its name still names the socket this process bound (its device
    // and inode, taken at the bind): never another server's, made at that name after this one's was moved or removed.
    void unlinkOwnSocket();
    // The server's directory, held by descriptor from the start (after it is made) to the end: the socket is bound,
    // checked, changed and removed in it, and the config repository and the drivers file are found through it, so a
    // directory moved aside while the server runs keeps everything this server does in one directory.
    int dirFd_ = -1;
    std::string dirNow(const std::string& name) const;
    // The control socket in the held directory, by its present path; by the name the server was given when that path
    // is too long for a Unix socket's address.
    std::string socketNow() const {
        const std::string now = dirNow(kSocketName);
        sockaddr_un addr;
        return socketAddress(now, &addr) ? now : paths_.socket;
    }
    bool bound_ = false;
    dev_t socketDev_ = 0;
    ino_t socketIno_ = 0;
    void control(int fd);
    bool endDrivers();
    std::size_t shutDownLinks();
    void configApply(int fd, const std::string& request);
    bool reloadServices();
    Paths paths_;
    std::unique_ptr<ServiceProcesses> services_;
    std::string serviceProblem_;
    std::string verifiedHelper_;
    std::string authClaude_;
    std::unique_ptr<store::Fs> fs_;
    std::unique_ptr<store::Fs> secretFs_;
    std::unique_ptr<AccountFiles> secretFiles_;
    // The stored logins' ends (server/login_life.h), checked at the start and each hour after.
    LoginWatch loginWatch_;
    Millis nextLoginCheck_ = 0;
    void checkLogins();
    // The secret dialog's effects (docs/secrets.md): each answer to its asker's driver, each log line to server.log.
    SecretLines secretLines_{[this](sync::SessionId id, const std::string& line) {
                                 if (drivers_) {
                                     drivers_->tellSecret(id, line);
                                 }
                             },
                             [](const std::string& line) { std::fprintf(stderr, "%s\n", line.c_str()); }};
    DurableStore store_;
    std::unique_ptr<Drivers> drivers_;
    std::unique_ptr<sync::Hub> hub_;
    std::unique_ptr<BusInbox> bus_;
    Millis nextBusRefresh_ = 0;
    std::unique_ptr<ConfigRepo> config_;
    // In-app sign-in's processes (server/signin_runner.h): `claude setup-token` per flow a window claims.
    std::unique_ptr<SignInRunner> signIns_;
    void openSignIn(const std::string& host);
    void signInRequest(int fd, const std::string& account);
    ServerSettings settings_;
    std::vector<Link> links_;
    std::uint64_t nextId_ = 1;
    int listen_ = -1;
    bool stopping_ = false;
    Drain drain_;
};

StartOrder::Outcome Server::open(bool unlocked, std::string* why) {
    if (unlocked) {
        *why = "an unlocked open is the trace binding's fault, never the server's";
        return StartOrder::Outcome::Failed;
    }
    return openStore(why);
}

// The store: its lock (exclusive, on its directory, never waiting on a writer), then its replay. Nothing else of the
// server is made yet, so nothing writes to the journal and no driver starts (server/start_order.h).
StartOrder::Outcome Server::openStore(std::string* why) {
    if (!fs_) {
        fs_ = store::make_posix_fs();
        // A test seam, set only by tests/link/link_e2e.cpp's flush run (server/flush_gate.h).
        if (const char* gate = std::getenv(kFlushGateVariable)) {
            fs_ = withFlushGate(std::move(fs_), gate);
        }
    }
    if (!store_.open(*fs_, dirNow("store"), why)) {
        return store_.busy() ? StartOrder::Outcome::Held : StartOrder::Outcome::Failed;
    }
    const store::OpenReport& r = store_.report();
    std::fprintf(stderr, "nexus-server: store %s: %s, %zu session%s, %llu record%s replayed, %llu bytes of torn tail "
        "cut; its lock (on the directory) held\n", paths_.store.c_str(), r.fresh ? "new" : "opened", store_.size(),
        store_.size() == 1 ? "" : "s", static_cast<unsigned long long>(r.records), r.records == 1 ? "" : "s",
        static_cast<unsigned long long>(r.cut_bytes));
    if (r.migrated > 0) {
        std::fprintf(stderr, "nexus-server: store %s: its %llu friend tab%s read as session%s, one each, and "
            "written in the sessions format\n", paths_.store.c_str(), static_cast<unsigned long long>(r.migrated),
            r.migrated == 1 ? "" : "s", r.migrated == 1 ? "" : "s");
    }
    if (r.skipped > 0) {
        std::fprintf(stderr, "nexus-server: store %s: %llu unknown field%s or change%s from a later server skipped\n",
            paths_.store.c_str(), static_cast<unsigned long long>(r.skipped), r.skipped == 1 ? "" : "s",
            r.skipped == 1 ? "" : "s");
    }
    return StartOrder::Outcome::Ok;
}

// Everything over the store, made with its lock and the control socket both held: the drivers, the hub, the secret
// dialog, the accounts (whose adoption commits to the journal), and last the drivers restored (which may commit and
// start a driver for a queued prompt).
bool Server::restoreAll(std::string* why) {
    // The machine a friend's instance names to run here: the config's, else the short host name.
    const std::string host = hostName();
    const std::string machine = serverMachine(settings_.machine, host);
    if (machine.empty()) {
        std::fprintf(stderr, "nexus-server: this machine has no name (gethostname failed, and the config sets no "
            "machine), so no friend's instance runs here; set `machine <name>` in %s\n", paths_.config.c_str());
    }
    if (!paths_.self.empty()) verifiedHelper_ = store::dir_of(paths_.self) + "/nexus-claude-auth";
    const char* path = std::getenv("PATH");
    authClaude_ = settings_.claude.empty() ? claudeOnPath(path ? path : "/usr/bin:/bin") : settings_.claude;
    drivers_ = std::make_unique<Drivers>(store_, Drivers::Options{paths_.drivers, paths_.home, Timing{}, nowMs,
                                                                  machine, host, true,
                                                                  [this] { return dirNow("drivers"); }, nullptr, {}, false,
                                                                  verifiedHelper_, authClaude_});
    hub_ = std::make_unique<sync::Hub>(store_, sync::Limits{}, drivers_.get(),
        [this] { return dirNow("policy.bundle.json"); });
    drivers_->attach(*hub_);
    bus_ = std::make_unique<BusInbox>(paths_.home, paths_.self, machine, host, dirFd_);
    bus_->listeners.setLauncher(settings_.busLauncher);
    // The secret dialog (docs/secrets.md): a value goes to its account's file, durable before the asker hears
    // "stored" (server/secret_files.h); a request is offered to every window whose link reads requests.
    secretFs_ = store::make_posix_fs();
    secretFiles_ = std::make_unique<AccountFiles>(*secretFs_, paths_.accounts);
    // A temporary file a write left (a crash, a failure) holds a value no one reads: it goes before anything is asked.
    for (const std::string& line : secretFiles_->sweep()) {
        std::fprintf(stderr, "nexus-server: accounts: %s\n", line.c_str());
    }
    hub_->secretsWith(secretFiles_.get(), &secretLines_, [this] {
        return std::any_of(links_.begin(), links_.end(),
                           [](const Link& l) { return l.link->format() >= sync::kSecretsFormat; });
    }, [] { return static_cast<std::uint64_t>(nowMs()); });
    // The accounts and friends: what the config repository's HEAD holds (server/config_repo.h). The sessions are
    // settled against them before any driver starts.
    config_ = std::make_unique<ConfigRepo>([this] { return dirNow("nexus-config"); });
    hub_->signOutWith([this](const accounts::Account& account) -> std::string {
        if (signIns_ && signIns_->running(account.name))
            return "Finish the browser sign-in for this account, then retry /logout.";
        // Make the command's sequence number durable before unlinking either login file.
        std::string why;
        if (!store_.flush(&why)) return "Could not persist the sign-out command: " + why;
        store_.beginBatch();
        auto error = drivers_->retireForSignOut(account.name);
        if (!error.empty()) return error;
        if (account.provider == "claude") return secretFiles_->forgetClaude(account.name);
        return forgetEnrollment(paths_.home, account.provider, account.name);
    });
    // An account is taken as signed in from the start when its login is there, usable, and not past its end, until
    // its driver says otherwise (a no-credential or authentication error): for a claude account, its oauth-token,
    // where the driver reads it (server/home.h accountsDir, under HOME whatever --dir says; server/login_life.h).
    hub_->probeSignInWith([logins = paths_.accounts, home = paths_.home](const accounts::Account& a) {
        if (a.requiresVerifiedEmail()) return hasVerifiedClaude(logins + "/" + a.name, a.email);
        if (sync::usesLocalSignInPage(a.provider)) {
            return hasEnrollment(home, a.provider, a.name);
        }
        const std::string file = sync::loginFile(a.provider);
        return !file.empty() &&
            loginLive(a.provider, inspectLogin(logins + "/" + a.name + "/" + file),
                      static_cast<std::uint64_t>(wallMs()));
    });
    // A login lost: ask in the window only when a paste can fix it, saying why (server/login_life.h loginAsk).
    hub_->loginsWith([logins = paths_.accounts](const accounts::Account& a) {
        if (a.requiresVerifiedEmail()) return sync::Hub::LoginAsk{false, "Sign in to " + a.email + " in Nexus to verify this account."};
        return loginAsk(a.name, a.provider, inspectLogin(logins + "/" + a.name + "/" + sync::loginFile(a.provider)),
                        static_cast<std::uint64_t>(wallMs()));
    });
    std::string configWhy;
    accounts::Registry registry;
    std::vector<std::string> report;
    const bool opened = config_->open(&configWhy);
    if (!opened) {
        std::fprintf(stderr, "nexus-server: config: %s; the server holds no accounts and no friends list, so no "
            "friend's driver starts and no prompt is sent; queued prompts are kept\n", configWhy.c_str());
    } else if (!config_->hasAccounts() && !config_->pending(&configWhy)) {
        // A server from before accounts: its accounts are what the drivers file passed with --account, so that no
        // session moves (server/driver_table.h accountsFromDrivers), committed as an upgrade.
        std::string text;
        std::ifstream in(dirNow("drivers"));
        if (in) {
            std::ostringstream all;
            all << in.rdbuf();
            text = all.str();
        }
        const accounts::Registry seed = accountsFromDrivers(parseDriverTable(text, paths_.home), machine);
        if (!seed.accounts.empty() || !seed.friends.empty()) {
            std::string why;
            if (config_->keepSeed(seed, "upgrade: the accounts and friends taken from the drivers file (its --account "
                              "values, and each friend's directory on this machine as its instance), so that every "
                              "session runs as it ran before", &why)) {
                std::fprintf(stderr, "nexus-server: config: accounts.json and friends.json made from %s\n",
                    paths_.drivers.c_str());
            } else {
                std::fprintf(stderr, "nexus-server: config: the accounts in %s were not taken: %s\n",
                    paths_.drivers.c_str(), why.c_str());
            }
        }
    }
    if (!opened) {
        // the line above said why
    } else if (!config_->committed(&registry, &report)) {
        for (const std::string& l : report) {
            std::fprintf(stderr, "nexus-server: config: %s\n", l.c_str());
        }
        std::fprintf(stderr, "nexus-server: config: the committed accounts and friends do not read, so the server "
            "holds none until `nexus-server config reload` applies good ones; until then no friend's driver starts "
            "and no prompt is sent, and queued prompts are kept\n");
        hub_->keepWith(config_.get());
    } else {
        for (const std::string& l : report) {
            std::fprintf(stderr, "nexus-server: config: %s\n", l.c_str());
        }
        for (const std::string& l : hub_->useAccounts(registry, config_.get())) {
            std::fprintf(stderr, "nexus-server: config: %s\n", l.c_str());
        }
        if (config_->adopting()) {
            // The sessions from before accounts ran on their friend's --account: they keep it. Until all have, the
            // mark stays, so a server stopped part way (a store that failed) adopts the rest at its next start.
            for (const std::string& l : hub_->adoptDefaults()) {
                std::fprintf(stderr, "nexus-server: config: %s\n", l.c_str());
            }
            if (hub_->failed()) {
                *why = "the store failed while the sessions took the drivers file's accounts; restart the server, "
                    "which takes them again";
                return false;
            }
            std::string adoptWhy;
            if (!config_->adopted(&adoptWhy)) {
                std::fprintf(stderr, "nexus-server: config: %s; until it is removed, each start gives every session "
                    "on no account its friend's default account again\n", adoptWhy.c_str());
            }
        }
        std::fprintf(stderr, "nexus-server: config: %zu account%s and %zu friend%s from %s\n", registry.accounts.size(),
            registry.accounts.size() == 1 ? "" : "s", registry.friends.size(), registry.friends.size() == 1 ? "" : "s",
            config_->dir().c_str());
    }
    // In-app sign-in, once the accounts are held: each claude account with no login is offered for a browser sign-in.
    openSignIn(host);
    if (std::ifstream in{dirNow("drivers")}) {
        std::ostringstream text;
        text << in.rdbuf();
        const auto table = parseDriverTable(text.str(), paths_.home);
        if (table.primary && table.errors.empty() && table.primaryDrivers.contains(table.primary->harness)) {
            const auto& binding = *table.primary;
            std::string problem;
            if (!hub_->ensurePrimary({kPrimaryChat, binding.harness, binding.account, {}, {}, binding.instance()}, &problem))
                std::fprintf(stderr, "nexus-server: primary chat: %s\n", problem.c_str());
        }
    }
    if (!drivers_->start(nowMs())) {
        *why = "the store failed while the friends' drivers were restored; restart the server";
        return false;
    }
    services_ = std::make_unique<ServiceProcesses>(paths_.self, [this] { return dirNow("service-leases"); });
    reloadServices();
    return true;
}

bool Server::reloadServices() {
    std::vector<ManagedService> next;
    if (!readServiceManifest(dirNow("services.json"), next, serviceProblem_)) {
        std::fprintf(stderr, "nexus-server: services: %s\n", serviceProblem_.c_str());
        return false;
    }
    services_->configure(next, nowMs());
    serviceProblem_.clear();
    return true;
}

// Another server answers on the control socket: Answered. Asked before the store is opened (a server from before the
// store's lock holds no lock, so only its socket shows it), at each try of a wait for a held lock, and again just
// before the socket is bound.
StartOrder::Outcome Server::probeControl(std::string* why) {
    if (serving(socketNow())) {
        *why = "another nexus-server is serving on " + paths_.socket + ", so this one exits; `nexus-server stop` "
            "stops it";
        return StartOrder::Outcome::Answered;
    }
    return StartOrder::Outcome::Ok;
}

// The present path of `name` in the server's directory, held by descriptor: where it is now, moved or not (macOS
// F_GETPATH; Linux the descriptor's /proc link). The name the server was started with when nothing is held.
std::string Server::dirNow(const std::string& name) const {
    std::string base = paths_.dir;
    if (dirFd_ >= 0) {
#if defined(__APPLE__)
        char path[MAXPATHLEN] = {};
        if (fcntl(dirFd_, F_GETPATH, path) == 0) {
            base = path;
        }
#else
        char path[PATH_MAX] = {};
        const ssize_t n = readlink(("/proc/self/fd/" + std::to_string(dirFd_)).c_str(), path, sizeof path - 1);
        if (n > 0) {
            base.assign(path, static_cast<std::size_t>(n));
        }
#endif
    }
    return base + "/" + name;
}

void Server::unlinkOwnSocket() {
    if (!bound_) {
        return;
    }
    bound_ = false;
    // In the held server directory (wherever it is now), and only the socket this process bound. What remains: between
    // this look and the removal another process could put a file of its own at the name, inside the server's own
    // directory (mode 0700); there is no call that removes a name only if it is a given file.
    struct stat st;
    if (fstatat(dirFd_, kSocketName, &st, AT_SYMLINK_NOFOLLOW) == 0 && S_ISSOCK(st.st_mode) &&
            st.st_dev == socketDev_ && st.st_ino == socketIno_) {
        unlinkat(dirFd_, kSocketName, 0);
    }
}

void Server::openSignIn(const std::string& host) {
    const std::string self = paths_.self;
    const std::string helper = self.empty() ? std::string() :
        self.substr(0, self.rfind('/') == std::string::npos ? 0 : self.rfind('/')) + "/nexus-signin-open";
    if (helper.empty() || access(helper.c_str(), X_OK) != 0) {
        std::fprintf(stderr, "nexus-server: sign-in: no nexus-signin-open beside this program, so accounts are signed "
            "in by a paste in the window\n");
        return;
    }
    SignInRunner::Options o;
    o.claude = settings_.claude;
    o.helper = helper;
    o.accounts = paths_.accounts;
    o.dir = paths_.dir;
    o.home = paths_.home;
    if (const char* path = std::getenv("PATH")) {
        o.path = path;
    }
    if (o.path.empty()) {
        o.path = "/usr/bin:/bin";
    }
    o.now = [] { return static_cast<std::uint64_t>(nowMs()); };
    SignInRunner::Events e;
    e.url = [this](const std::string& account, std::uint64_t id, const sync::SecretValue& url) {
        hub_->signInUrl(account, id, url);
    };
    e.printed = [this](const std::string& account, const sync::SecretValue& token) {
        hub_->signInPrinted(account, token);
    };
    e.enrolled = [this](const std::string& account, const std::string& provider, std::uint64_t id) {
        hub_->signInEnrolled(account, provider, id);
    };
    e.exited = [this](const std::string& account, std::uint64_t id, const std::string& why) {
        hub_->signInExited(account, id, why);
    };
    e.log = [](const std::string& line) { std::fprintf(stderr, "nexus-server: %s\n", line.c_str()); };
    signIns_ = std::make_unique<SignInRunner>(std::move(o), std::move(e));
    std::string why;
    if (!signIns_->open(&why)) {
        std::fprintf(stderr, "nexus-server: sign-in: %s; accounts are signed in by a paste in the window\n",
            why.c_str());
        signIns_.reset();
        return;
    }
    sync::SignInHooks hooks;
    hooks.start = [this](std::uint64_t id, const std::string& account) {
        const auto* registry = hub_->accounts();
        const auto* a = registry ? registry->account(account) : nullptr;
        if (!a) {
            hub_->signInExited(account, id, "this account was removed");
            return;
        }
        if (a->requiresVerifiedEmail()) {
            if (authClaude_.empty() || verifiedHelper_.empty() || access(verifiedHelper_.c_str(), X_OK) != 0) {
                hub_->signInExited(account, id, "install nexus-claude-auth beside nexus-server and configure the Claude executable");
                return;
            }
            signIns_->start(id, account, a->provider, {verifiedHelper_, "enroll", "--account-dir",
                paths_.accounts + "/" + account, "--email", a->email, "--claude", authClaude_});
            return;
        }
        if (a->provider == "claude") {
            signIns_->start(id, account);
            return;
        }
        std::ifstream in(dirNow("drivers"));
        std::ostringstream text;
        text << in.rdbuf();
        auto table = parseDriverTable(text.str(), paths_.home);
        auto cmd = table.signins.find(a->provider);
        if (cmd == table.signins.end()) {
            hub_->signInExited(account, id, "configure a signin command for " + a->provider +
                                            " in the server's drivers file");
            return;
        }
        cmd->second.insert(cmd->second.end(), {"--account", account});
        signIns_->start(id, account, a->provider, std::move(cmd->second));
    };
    hooks.stop = [this](const std::string& account) { signIns_->stop(account); };
    hooks.replay = [this](std::uint64_t id, const std::string& account, const sync::SignInTarget& target,
                          const sync::SecretValue& query) { signIns_->replay(id, account, target, query); };
    // A window acts on a sign-in when its link is of format 5 or later, the older of the two Hellos' protos
    // (sync/formats.h takesSignIn): that alone.
    hooks.takers = [this] {
        return std::any_of(links_.begin(), links_.end(),
                           [](const Link& l) { return sync::takesSignIn(l.link->format()); });
    };
    hub_->signInWith(std::move(hooks), host);
}

// `config sign-in <account>`: one line back, never anything of the flow: "OK signed in", "OK needs sign-in: ...", or
// "ERROR <why>". The CLI asks again until the first or the last.
void Server::signInRequest(int fd, const std::string& account) {
    std::string said;
    const bool ok = hub_->requestSignIn(account, &said);
    writeAll(fd, (ok ? "OK " : "ERROR ") + said + "\n");
    for (Link& s : links_) {
        s.link->setState(hub_->stateFor(s.signIn, s.client));
    }
}

StartOrder::Outcome Server::listenControl(std::string* why) {
    sockaddr_un addr;
    if (!socketAddress(paths_.socket, &addr)) {
        *why = paths_.socket + " is too long for a Unix socket's address";
        return StartOrder::Outcome::Failed;
    }
    if (probeControl(why) != StartOrder::Outcome::Ok) {
        return StartOrder::Outcome::Answered;
    }
    // A socket left by a server that ended without removing it: no server of this build can have bound it since, as
    // binding takes this store's lock first, which this one holds (docs/formats.md, for a server from before the lock).
    // Removed, bound, read back and changed in the held directory, never by a path that could now name another.
    unlinkat(dirFd_, kSocketName, 0);
    listen_ = socket(AF_UNIX, SOCK_STREAM, 0);
    if (listen_ < 0) {
        *why = errnoText("socket");
        return StartOrder::Outcome::Failed;
    }
    // Close-on-exec: a child the server starts (git for the config repository, a driver) never holds the socket, so a
    // server that dies leaves nothing answering on it.
    if (fcntl(listen_, F_SETFD, FD_CLOEXEC) != 0) {
        *why = errnoText("cannot set the control socket close-on-exec");
        return StartOrder::Outcome::Failed;
    }
    if (!bindIn(listen_, dirFd_, kSocketName, paths_.socket, why)) {
        *why = "cannot bind " + paths_.socket + ": " + *why;
        return StartOrder::Outcome::Failed;
    }
    // What remains (docs/formats.md): between the bind and this look, another process of this user could replace the
    // name in the server's own directory (mode 0700); the identity is then of that file, and the stop leaves it.
    struct stat st;
    if (fstatat(dirFd_, kSocketName, &st, AT_SYMLINK_NOFOLLOW) != 0 || !S_ISSOCK(st.st_mode)) {
        *why = errnoText("cannot read back " + paths_.socket);
        return StartOrder::Outcome::Failed;
    }
    bound_ = true;
    socketDev_ = st.st_dev;
    socketIno_ = st.st_ino;
    if (fchmodat(dirFd_, kSocketName, 0600, 0) != 0 || ::listen(listen_, 16) != 0) {
        *why = errnoText("cannot listen on " + paths_.socket);
        return StartOrder::Outcome::Failed;
    }
    fcntl(listen_, F_SETFL, O_NONBLOCK);
    return StartOrder::Outcome::Ok;
}

// One request on the control socket: "new" (a session: its CONNECT line) or "stop". Only this user may ask.
void Server::control(int fd) {
    uid_t uid = 0;
    if (!peerUid(fd, &uid) || uid != getuid()) {
        std::fprintf(stderr, "nexus-server: refused a control request from user %u\n", static_cast<unsigned>(uid));
        return;
    }
    std::string request;
    if (!readLine(fd, &request, kControlTimeoutMs)) {
        writeAll(fd, "ERROR no request within 2 s\n");
        return;
    }
    if (request.starts_with("history-page ")) {
        std::istringstream fields(request.substr(13));
        std::uint64_t id, epoch, first;
        std::string extra;
        if (!(fields >> id >> epoch >> first) || fields >> extra) return;
        auto page = hub_->historyPage(id, epoch, first);
        if (page.find(id)) writeAll(fd, page.init_diff());
        return;
    }
    if (request == "stop") {
        supervision::noteIntentionalStopAt(dirNow(supervision::kSuperviseDir));
        writeAll(fd, "OK\n");
        stopping_ = true;
        return;
    }
    if (request == "drain") {
        drain_.request(static_cast<std::uint64_t>(nowMs()));
        writeAll(fd, "OK\n");
        return;
    }
    // A start's (and `new`'s) question: answered only from this loop, so SERVING is true when said. Once `stop` is
    // accepted the loop takes no more requests, and the connections still queued are closed unanswered with the socket
    // (models/store/StoreLock.tla StopAccept, then CloseSocket; RefusedServes).
    // Its second line, which a start's question does not read, is the build this server runs from (server/builds.h,
    // server/drain.h).
    if (request == kStatusRequest) {
        writeAll(fd, std::string(stopping_ ? "STOPPING" : kServingReply) + "\n" +
                         statusBuildLine(paths_.build, drain_.draining()) + "\n");
        return;
    }
    constexpr std::string_view workspaceRequest = "primary-workspace ";
    if (request.starts_with(workspaceRequest)) {
        const auto primary = hub_->primarySession();
        const auto* session = primary ? store_.find(*primary) : nullptr;
        const auto* registry = hub_->accounts();
        const auto* account = registry && session ? registry->account(session->info.account) : nullptr;
        if (session && account && account->billing == accounts::Billing::Subscription &&
                account->provider == session->info.harness && request.substr(workspaceRequest.size()) == session->info.instance) {
            std::ifstream in(dirNow("drivers"));
            std::ostringstream content;
            content << in.rdbuf();
            const auto table = parseDriverTable(content.str(), paths_.home);
            if (table.primary && table.primary->matches(session->info)) {
                writeAll(fd, "OK " + table.primary->cwd + "\n");
                return;
            }
        }
        writeAll(fd, "ERROR the selected primary workspace is no longer active; reconnect and try again\n");
        return;
    }
    if (request == "services reload" || request == "services status") {
        if (request == "services reload" && drain_.draining()) {
            writeAll(fd, "ERROR server is draining; reload services on its replacement\n");
            return;
        }
        if (request == "services reload")
            reloadServices();
        else
            writeAll(fd, services_->status());
        writeAll(fd, serviceProblem_.empty() ? "OK\n" : "ERROR " + serviceProblem_ + "\n");
        return;
    }
    if (request == "config reload" || request.rfind("config restore ", 0) == 0) {
        configApply(fd, request);
        return;
    }
    if (request.rfind("config sign-in ", 0) == 0) {
        signInRequest(fd, request.substr(std::string("config sign-in ").size()));
        return;
    }
    Client client;
    if (!parseNewRequest(request, &client)) {
        // The reply's start is what a `new` that states a format reads as a server from before formats: keep it.
        writeAll(fd, "ERROR unknown request; the requests are `new`, `new <client> <next>`, either with "
            "` format <n>` and then ` hello <hello>`, `status`, `drain`, `stop`, `config reload`, "
            "`config restore <commit>`, `config sign-in <account>`, `services reload` and `services status`\n");
        return;
    }
    // The window's Hello against the server's (sync/formats.h): a window below the server's min_peer, or a server
    // below the window's, is refused in one plain line, which the window shows and the log keeps. Every other pairing
    // of versions works.
    const std::string refused = formatRefusalReply(client);
    if (!refused.empty()) {
        writeAll(fd, refused + "\n");
        std::fprintf(stderr, "nexus-server: refused a window: %s\n", refused.c_str() + 6);
        return;
    }
    // A client's new session ends its earlier one: the window has left it.
    if (!client.id.empty()) {
        for (const Link& s : links_) {
            if (s.client == client.id) {
                hub_->signInWindowGone(s.signIn);
            }
        }
        links_.erase(std::remove_if(links_.begin(), links_.end(),
            [&](const Link& s) { return s.client == client.id; }), links_.end());
    }
    if (links_.size() >= kMaxLinks) {
        writeAll(fd, "ERROR the server holds " + std::to_string(kMaxLinks) + " links, its limit\n");
        return;
    }
    const std::uint64_t applied = client.id.empty() ? 0 : store_.applied(client.id);
    if (client.next > applied + 1) {
        // The client holds no action numbered applied + 1 .. next - 1, so they could never arrive.
        writeAll(fd, "ERROR client " + client.id + ": the server has applied its actions up to " +
            std::to_string(applied) + ", so a session from action " + std::to_string(client.next) +
            " would skip those in between; start from action " + std::to_string(applied + 1) + " or before\n");
        return;
    }
    try {
        // The link's format: the newer one both ends read, the older of the two protos. A window that stated none
        // reads format 1, and its CONNECT line states none, as it expects; the server's Hello goes back only to a
        // window that stated one.
        const std::uint32_t format = sync::linkFormat(helloOf(client).proto, sync::kStateFormat);
        Link s{nextId_++, client.id,
            std::make_unique<link::ServerSession>(hub_->state(), applied, client.next, format), std::string()};
        const std::string line = link::connectLine(s.link->port(), s.link->key(),
            client.format == 0 && !client.hasHello ? 0 : format,
            client.hasHello ? sync::helloText(sync::kHello) : std::string());
        // The key goes on this one line to `nexus-server new`, and nowhere else.
        if (!writeAll(fd, line + "\n")) {
            std::fprintf(stderr, "nexus-server: link %llu: the CONNECT line could not be written; dropped\n",
                static_cast<unsigned long long>(s.id));
            return;
        }
        std::fprintf(stderr, "nexus-server: link %llu on UDP port %s%s%s, from action %llu (applied %llu)\n",
            static_cast<unsigned long long>(s.id), s.link->port().c_str(), s.client.empty() ? "" : ", client ",
            s.client.c_str(), static_cast<unsigned long long>(client.next), static_cast<unsigned long long>(applied));
        links_.push_back(std::move(s));
        // A window from before sign-ins, and none newer: the accounts waiting on a sign-in are asked for by a paste.
        hub_->signInWindowsChanged();
        for (Link& l : links_) {
            l.link->setState(hub_->stateFor(l.signIn, l.client));
        }
    } catch (const std::exception& e) {
        writeAll(fd, std::string("ERROR ") + e.what() + "\n");
        std::fprintf(stderr, "nexus-server: %s\n", e.what());
    }
}

// `config reload` (the working files, hand-edited) or `config restore <commit>` (the files as that commit held them),
// applied by the hub and committed, with what was applied or refused written back line by line, then OK or ERROR, and
// to the log. Decided against the server's state as it is now.
void Server::configApply(int fd, const std::string& request) {
    std::vector<std::string> lines;
    accounts::Registry next;
    std::string message;
    bool ok = false;
    if (!config_) {
        lines.push_back("the server has no config repository");
    } else if (request == "config reload") {
        ok = config_->working(&next, &lines);
        message = "reload: the hand edit of accounts.json and friends.json applied (the person's or their AI's)";
    } else {
        const std::string commit = request.substr(std::string("config restore ").size());
        ok = config_->restoring(commit, &next, &lines);
        message = "restore: the files as commit " + commit + " held them, applied as a new version";
    }
    if (ok) {
        const bool reload = request == "config reload";
        config_->applying(reload);   // only a reload commits over the working files: they are what it applies
        ok = hub_->applyAccounts(next, message, &lines);
        nextBusRefresh_ = 0;
        config_->applying(false);
        for (const std::string& n : config_->notes()) {
            lines.push_back(n);
        }
        for (Link& s : links_) {
            s.link->setState(hub_->stateFor(s.signIn, s.client));
        }
    }
    if (ok && lines.empty()) {
        lines.push_back("nothing changed: the files already said what the server holds");
    }
    std::string reply;
    for (const std::string& l : lines) {
        reply += (ok ? "applied: " : "refused: ") + l + "\n";
        std::fprintf(stderr, "nexus-server: config %s: %s%s\n", request.c_str() + 7, ok ? "applied: " : "refused: ",
            l.c_str());
    }
    reply += ok ? "OK\n" : "ERROR nothing was applied; the server keeps the accounts and friends it had\n";
    writeAll(fd, reply);
}

// mosh's shutdown on every link, ticked until each is acknowledged or given up on, or kLinkShutdownMs: how many were
// acknowledged.
std::size_t Server::shutDownLinks() {
    if (links_.empty()) {
        return 0;
    }
    link::freezeClock();
    for (Link& s : links_) {
        s.link->shutdown();
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kLinkShutdownMs);
    std::string why;
    for (;;) {
        link::freezeClock();
        bool done = true;
        std::vector<pollfd> fds;
        int wait = kLinkShutdownMs;
        for (Link& s : links_) {
            s.link->tick(&why);
            done = done && s.link->shutdownDone();
            wait = std::min(wait, std::max(0, s.link->waitMs()));
            for (int fd : s.link->fds()) {
                fds.push_back(pollfd{fd, POLLIN, 0});
            }
        }
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline -
            std::chrono::steady_clock::now()).count();
        if (done || left <= 0) {
            break;
        }
        ::poll(fds.data(), static_cast<nfds_t>(fds.size()), std::min(wait, static_cast<int>(left)));
        link::freezeClock();
        for (const pollfd& p : fds) {
            if (p.revents & POLLIN) {
                for (Link& s : links_) {
                    for (int fd : s.link->fds()) {
                        if (fd == p.fd) {
                            s.link->receive(&why);
                        }
                    }
                }
            }
        }
    }
    std::size_t acknowledged = 0;
    for (const Link& s : links_) {
        acknowledged += s.link->shutdownAcknowledged() ? 1 : 0;
    }
    return acknowledged;
}

// Ends every driver (input closed, then SIGTERM, then SIGKILL, each after its grace period) and waits for them. False
// when the store failed on the way; the drivers are killed and collected even then.
bool Server::endDrivers() {
    if (!drivers_->shutdown(nowMs()))
        return false;     // Drivers' destructor kills and collects them
    while (drivers_->alive()) {
        std::vector<pollfd> fds = drivers_->fds();
        if (::poll(fds.data(), static_cast<nfds_t>(fds.size()), drivers_->waitMs(nowMs())) < 0 && errno != EINTR)
            break;
        bool changed = false;
        if (!drivers_->serve(nowMs(), &changed))
            return false;
    }
    return true;
}

// Each stored login's end (server/login_life.h): a notice in its account's sessions 30, 7 and 1 days ahead, and on the
// day it is signed out and a new one asked for in the window, saying why.
void Server::checkLogins() {
    const accounts::Registry* registry = hub_->accounts();
    if (registry == nullptr) {
        return;
    }
    std::vector<LoginWatch::Login> logins;
    for (const accounts::Account& a : registry->accounts) {
        if (a.requiresVerifiedEmail()) continue;
        const std::string file = sync::loginFile(a.provider);
        if (file.empty()) {
            continue;
        }
        const LoginSeen seen = inspectLogin(paths_.accounts + "/" + a.name + "/" + file);
        if (seen.file == LoginSeen::File::Usable) {
            logins.push_back(LoginWatch::Login{a.name, a.provider, seen.storedAtMs});
        }
    }
    for (const LoginWatch::Notice& n : loginWatch_.check(logins, static_cast<std::uint64_t>(wallMs()))) {
        if (n.expired) {
            hub_->needsSignIn(n.account, n.text);
        } else {
            hub_->tellAccount(n.account, n.text);
        }
    }
}

void Server::tick() {
    services_->tick(nowMs());
    link::freezeClock();
    std::string why;
    for (Link& s : links_) {
        if (!s.link->tick(&why))
            std::fprintf(stderr, "nexus-server: link %llu: %s\n", static_cast<unsigned long long>(s.id),
                why.c_str());
    }
    bool linksEnded = false;
    for (auto it = links_.begin(); it != links_.end();) {
        if (it->link->ended()) {
            std::fprintf(stderr, "nexus-server: link %llu ended by its client after %llu actions\n",
                static_cast<unsigned long long>(it->id), static_cast<unsigned long long>(it->link->applied()));
            // The sign-ins its window held are let go of, for another window to take over; with none left that
            // can, signInWindowsChanged ends them and asks for a paste.
            hub_->signInWindowGone(it->signIn);
            it = links_.erase(it);
            linksEnded = true;
        } else {
            ++it;
        }
    }
    if (linksEnded) {
        hub_->signInWindowsChanged();   // only older windows left: a paste instead of a sign-in
        for (Link& s : links_) {
            s.link->setState(hub_->stateFor(s.signIn, s.client));
        }
    }
}

PassOrder::Outcome Server::wait(std::string* why) {
    (void)why;
    // A commit to the config repository asked for a push: it runs as a child the loop watches, never here.
    if (config_ && config_->pushWanted() && !config_->pushRunning()) {
        std::string pushWhy;
        if (!config_->pushStart(nowMs(), &pushWhy)) {
            std::fprintf(stderr, "nexus-server: config push: %s\n", pushWhy.c_str());
        }
    }
    const auto now = nowMs();
    pass_.busRosterRefreshed = !drain_.draining() && now >= nextBusRefresh_;
    if (pass_.busRosterRefreshed) {
        bus_->refresh(dirNow("drivers"), hub_->accounts(), now);
        nextBusRefresh_ = now + kResolveMs;
    }
    pass_.fds = {pollfd{listen_, POLLIN, 0}};
    pass_.owner = {0};
    int waitMs = std::min(kMaxWaitMs, drivers_->waitMs(nowMs()));
    waitMs = std::min(waitMs, services_->waitMs(nowMs()));
    if (const std::optional<std::uint64_t> due = drain_.untilDeadline(static_cast<std::uint64_t>(nowMs()))) {
        waitMs = std::min(waitMs, static_cast<int>(std::min<std::uint64_t>(*due, static_cast<std::uint64_t>(kMaxWaitMs))));
    }
    if (const std::optional<std::uint64_t> due = hub_->secretWake()) {
        const std::uint64_t now = static_cast<std::uint64_t>(nowMs());
        waitMs = std::min(waitMs, *due > now ? static_cast<int>(std::min<std::uint64_t>(*due - now, kMaxWaitMs)) : 0);
    }
    if (config_ && config_->pushRunning()) {
        waitMs = std::min(waitMs, config_->pushWaitMs(nowMs()));
    }
    if (const std::optional<std::uint64_t> due = hub_->signInDeadline()) {
        const std::uint64_t now = static_cast<std::uint64_t>(nowMs());
        waitMs = std::min(waitMs, *due > now ? static_cast<int>(std::min<std::uint64_t>(*due - now, kMaxWaitMs)) : 0);
    }
    if (signIns_ && signIns_->waitMs() >= 0) {
        waitMs = std::min(waitMs, signIns_->waitMs());
    }
    for (std::size_t i = 0; i < links_.size(); ++i) {
        waitMs = std::min(waitMs, std::max(0, links_[i].link->waitMs()));
        for (int fd : links_[i].link->fds()) {
            pass_.fds.push_back(pollfd{fd, POLLIN, 0});
            pass_.owner.push_back(i);
        }
    }
    // The drivers' pipes last: serve() below looks at every driver, so their events need no owner.
    pass_.linkFds = pass_.fds.size();
    for (const pollfd& p : drivers_->fds())
        pass_.fds.push_back(p);
    if (!drain_.draining()) {
        for (const auto& p : bus_->listeners.fds()) pass_.fds.push_back(p);
        const int busWait = bus_->listeners.waitMs();
        if (busWait >= 0) waitMs = std::min(waitMs, busWait);
    }
    if (config_) {
        for (int fd : config_->pushFds()) {
            pass_.fds.push_back(pollfd{fd, POLLIN, 0});
        }
    }
    // Sign-in's processes, helper and replays: serve() below looks at all of them.
    if (signIns_) {
        for (const pollfd& p : signIns_->fds()) {
            pass_.fds.push_back(p);
        }
    }
    if (::poll(pass_.fds.data(), static_cast<nfds_t>(pass_.fds.size()), waitMs) < 0 && errno != EINTR) {
        std::fprintf(stderr, "nexus-server: %s\n", errnoText("poll").c_str());
        return PassOrder::Outcome::Stopped;
    }
    link::freezeClock();
    std::string pushed;
    if (config_ && config_->pushServe(nowMs(), &pushed)) {
        std::fprintf(stderr, "nexus-server: config push: %s\n", pushed.c_str());
    }
    return PassOrder::Outcome::Ok;
}

// One batch per pass (group commit): every step this pass makes, from the drivers and from every window's actions, is
// applied and appended, then made durable by one flush. Nothing of the batch leaves the server before that flush has
// returned: no driver is told anything (Drivers::hold), no state is set on a link, and no link ticks (mosh's
// acknowledgement of a window's actions goes out on its next tick).
PassOrder::Outcome Server::hold(bool holdDrivers, std::string* why) {
    if (!holdDrivers) {
        *why = "an unheld pass is the trace binding's fault, never the server's";
        return PassOrder::Outcome::Failed;
    }
    store_.beginBatch();
    drivers_->hold();
    return PassOrder::Outcome::Ok;
}

PassOrder::Outcome Server::apply(std::string* why) {
    bool changed = pass_.busRosterRefreshed && hub_->setBusSenders(bus_->knownSenders());
    if ((pass_.busRosterRefreshed && !drivers_->refreshSandbox(bus_->unsandboxed())) ||
        !drivers_->serve(nowMs(), &changed)) {
        *why = "the store failed; stopping without acknowledging the actions since";
        return PassOrder::Outcome::Failed;
    }
    if (!drain_.draining() && !stopping_ && bus_->listeners.serve([this](const std::string& recipient,
                                                                         const BusMessage& message) {
        return bus_->receive(*hub_, recipient, message);
    })) changed = true;
    // A secret request past its deadline is refused (timeout), and a held value whose time has come is written again
    // (docs/secrets.md: a value that arrived is never dropped while the server runs), in this pass's batch like any
    // step. nexus's own ask whose paste was refused malformed is asked again.
    if (const std::optional<std::uint64_t> due = hub_->secretDeadline();
            due && *due <= static_cast<std::uint64_t>(nowMs())) {
        hub_->expireSecrets();
        changed = true;
    }
    if (const std::optional<std::uint64_t> due = hub_->secretRetryAt();
            due && *due <= static_cast<std::uint64_t>(nowMs())) {
        hub_->retrySecrets();
        changed = true;
    }
    if (hub_->reaskPending() && hub_->reaskSecrets()) {
        changed = true;
    }
    // Sign-in: the processes' URLs, tokens and ends, the helper's lines, the replays; and the flows past their
    // deadline. A token goes straight into the one-chance store in this pass's batch.
    if (signIns_ && signIns_->serve()) {
        changed = true;
    }
    if (const std::optional<std::uint64_t> due = hub_->signInDeadline();
            due && *due <= static_cast<std::uint64_t>(nowMs())) {
        hub_->expireSignIns();
        changed = true;
    }
    // The stored logins' ends: notices ahead of one that cannot be renewed, and a new one asked for on its day.
    if (nowMs() >= nextLoginCheck_) {
        checkLogins();
        nextLoginCheck_ = nowMs() + kLoginCheckMs;
        changed = true;
    }
    std::string linkWhy;
    for (std::size_t k = 1; k < pass_.linkFds; ++k) {
        if (!(pass_.fds[k].revents & POLLIN))
            continue;
        Link& s = links_[pass_.owner[k]];
        if (!s.link->receive(&linkWhy)) {
            std::fprintf(stderr, "nexus-server: link %llu: %s\n", static_cast<unsigned long long>(s.id),
                linkWhy.c_str());
            continue;
        }
        std::string problem;
        // Each action is durable (with the client's counter), by the flush below, before the next tick can
        // acknowledge it.
        for (const sync::Action& a : s.link->take(&problem)) {
            std::string refused;
#ifdef NEXUS_LEAKY_SIGNIN_WITNESS
            // The test-only leaky build alone (nexus-server-leaky, CMakeLists.txt): the leak link_e2e_signin's
            // search of server.log must find. Never in nexus-server.
            if (a.kind == sync::ActionKind::SignInCallback) {
                std::fprintf(stderr, "nexus-server: leaky witness build: callback %s\n", a.secret.bytes().c_str());
            }
#endif
            // The claim token a window claims by names its link: that link's state carries the URLs it holds.
            if (a.kind == sync::ActionKind::SignInClaim && sync::isClaimToken(a.text) && s.signIn != a.text) {
                hub_->signInWindowGone(s.signIn);
                s.signIn = a.text;
            }
            // An opened or a callback counts only from the link that claimed by its token: no other window speaks
            // for a flow (the state never shows one window's token to another, sync/signin.h kSignInHeldBy).
            if ((a.kind == sync::ActionKind::SignInOpened || a.kind == sync::ActionKind::SignInCallback) &&
                    a.text != s.signIn) {
                sync::Action named = a;
                named.text = s.signIn;
                hub_->apply(named, &refused, s.client);
            } else {
                hub_->apply(a, &refused, s.client);
            }
            changed = true;
            if (!refused.empty())
                std::fprintf(stderr, "nexus-server: link %llu, action %llu: %s\n",
                    static_cast<unsigned long long>(s.id), static_cast<unsigned long long>(a.num),
                    refused.c_str());
            if (hub_->failed())
                break;
        }
        if (!problem.empty())
            std::fprintf(stderr, "nexus-server: link %llu: %s\n", static_cast<unsigned long long>(s.id),
                problem.c_str());
        if (hub_->failed())
            break;
    }
    if (hub_->failed()) {
        // What reached the disk is unknown: stop before acknowledging anything more. A restart reopens the store from
        // what is durable, and the windows reconnect.
        *why = "the store failed; stopping without acknowledging the actions since";
        return PassOrder::Outcome::Failed;
    }
    return changed ? PassOrder::Outcome::Changed : PassOrder::Outcome::Unchanged;
}

PassOrder::Outcome Server::flush(std::string* why) {
    std::string flushWhy;
    if (!store_.flush(&flushWhy)) {
        // The batch's flush failed: what reached the disk is unknown, so nothing of it is told to anyone.
        *why = "the store failed (" + flushWhy + "); stopping without acknowledging the actions since";
        return PassOrder::Outcome::Failed;
    }
    return PassOrder::Outcome::Ok;
}

void Server::release() {
    bus_->listeners.flushed();
    drivers_->release(nowMs());
}

void Server::state() {
    for (Link& s : links_) {
        s.link->setState(hub_->stateFor(s.signIn, s.client));
    }
}

int Server::run() {
    std::string why;
    if (!ensureHome(paths_, &why)) {
        std::fprintf(stderr, "nexus-server: %s\n", why.c_str());
        return kFailedExit;
    }
    // Migrate an old ~/.nexus layout once into ~/.nexus/server before reading config or opening the store, unless a
    // live server already answers on ~/.nexus/server/server.sock (which StartOrder::Probe below refuses with exit 0).
    if (!paths_.top.empty() && !serving(paths_.socket)) {
        std::unique_ptr<store::Fs> migFs = store::make_posix_fs();
        migration::Options migOpts;
        migOpts.old_server_live = [this] { return migration::probeOldServerSocket(paths_.top + "/" + kSocketName); };
        migration::Report migReport;
        const store::Status migSt = migration::migrateOldLayout(*migFs, paths_.top, paths_.dir, migOpts, &migReport);
        if (!migSt.is_ok()) {
            std::fprintf(stderr, "nexus-server: %s\n", migSt.message().c_str());
            return kFailedExit;
        }
        if (migReport.migrated) {
            std::fprintf(stderr, "nexus-server: migrated the old layout in %s to %s%s\n", paths_.top.c_str(),
                paths_.dir.c_str(), migReport.resumed ? " (resumed)" : "");
        }
    }
    if (!readConfig(paths_.config, &settings_, &why)) {
        std::fprintf(stderr, "nexus-server: %s\n", why.c_str());
        return kFailedExit;
    }
    dirFd_ = ::open(paths_.dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dirFd_ < 0) {
        std::fprintf(stderr, "nexus-server: %s\n", errnoText("cannot open " + paths_.dir).c_str());
        return kFailedExit;
    }
    // The start, in its order (server/start_order.h, models/store/StoreLock.tla), each step through take_step, the
    // dispatch the trace walk runs too: the socket asked, the store's lock and its replay (waiting, a bounded time,
    // while a writer holds the lock and no server answers), the socket bound, and only then the drivers restored. No
    // window is served before the loop, so no session is made on a state not yet read.
    std::uint64_t heldWait = kHeldWaitMs;
    if (const char* wait = std::getenv(kHeldWaitVariable)) {
        heldWait = std::strtoull(wait, nullptr, 10);
    }
    StartOrder start(StartOrder::Fault::None, heldWait);
    std::string heldWhy;     // what the lock's holder is, from the last open that found it held
    bool said = false;
    Millis waitFrom = 0;     // when the wait began
    Millis saidAt = 0;       // when it was last said
    for (;;) {
        const StartOrder::Step step = take_step(start, *this, &why);
        if (step == StartOrder::Step::Open && start.next() == StartOrder::Step::Wait) {
            heldWhy = why;
            const Millis now = nowMs();
            if (!said) {
                std::fprintf(stderr, "nexus-server: %s, and no server on %s says it serves: waiting up to %llu ms "
                    "for it to let go (a server starting or stopping)\n", why.c_str(), paths_.socket.c_str(),
                    static_cast<unsigned long long>(heldWait));
                said = true;
                waitFrom = now;
                saidAt = now;
            } else if (now - saidAt >= kWaitProgressMs) {
                // A person watching sees the wait go on, a line a second, not one line and then silence.
                std::fprintf(stderr, "nexus-server: still waiting for the store's lock: %llu of %llu ms\n",
                    static_cast<unsigned long long>(now - waitFrom), static_cast<unsigned long long>(heldWait));
                saidAt = now;
            }
        }
        const StartOrder::Step at = start.next();
        if (at == StartOrder::Step::Serve) {
            break;
        }
        if (at == StartOrder::Step::Refused) {
            // A live server answers: said in one line, and an exit a service does not restart.
            std::fprintf(stderr, "nexus-server: %s\n", why.c_str());
            if (listen_ >= 0) {
                close(listen_);
            }
            return kRefusedExit;
        }
        if (at == StartOrder::Step::Stuck) {
            std::fprintf(stderr, "nexus-server: %s, and no server on %s said it serves in %llu ms: its holder is neither "
                "serving nor ending; find it (lsof %s) and stop it, then start the server again\n", heldWhy.c_str(),
                paths_.socket.c_str(), static_cast<unsigned long long>(heldWait), paths_.store.c_str());
            return kStuckExit;
        }
        if (at == StartOrder::Step::Failed) {
            std::fprintf(stderr, "nexus-server: %s\n", why.c_str());
            if (listen_ >= 0) {
                close(listen_);
                unlinkOwnSocket();
            }
            return kFailedExit;
        }
    }
    if (const char* drainEnv = std::getenv(kDrainLimitVariable)) {
        char* end = nullptr;
        const unsigned long long ms = std::strtoull(drainEnv, &end, 10);
        if (end != drainEnv && *end == 0) {
            drain_ = Drain(static_cast<std::uint64_t>(ms));
        }
    }
    std::fprintf(stderr, "nexus-server: serving on %s\n", paths_.socket.c_str());
    int status = 0;
    bool drained = false;
    while (!stopping_ && !gStop) {
        if (drain_.stopNow(drivers_->busy(), static_cast<std::uint64_t>(nowMs()))) {
            drained = true;
            break;
        }
        // One pass of the loop, in its order (server/pass.h, models/LoopPass.tla), each step through take_pass_step,
        // the dispatch the trace walk runs too: the links' ticks, the poll, the batch held, its steps applied, its one
        // flush, the drivers released and the state staged for the next pass's tick.
        PassOrder pass;
        for (;;) {
            take_pass_step(pass, *this, &why);
            const PassOrder::Step at = pass.next();
            if (at == PassOrder::Step::Done || at == PassOrder::Step::Stopped || at == PassOrder::Step::Failed) {
                break;
            }
        }
        if (pass.next() == PassOrder::Step::Stopped) {
            break;
        }
        if (pass.next() == PassOrder::Step::Failed) {
            std::fprintf(stderr, "nexus-server: %s\n", why.c_str());
            status = 1;
            break;
        }
        // The control socket last: a new link may end an earlier one, and the links' fds above are by index.
        if (pass_.fds[0].revents & POLLIN) {
            const int fd = accept(listen_, nullptr, nullptr);
            if (fd >= 0) {
                fcntl(fd, F_SETFL, 0);
                control(fd);
                close(fd);
            }
        }
        if (drain_.stopNow(drivers_->busy(), static_cast<std::uint64_t>(nowMs()))) {
            drained = true;
            break;
        }
    }
    // The stop, in its order (models/store/StoreLock.tla Stop, EndDrivers, Exit): the control socket first, closed and
    // its file removed (only if it is still this server's), so that from now on no probe finds a server that is going
    // away and no window starts a session on it; then the drivers ended, whose last records are written under the
    // store's lock, still held; the lock last, when the process ends. A server started in between finds the lock held
    // and no socket, and waits for the lock (server/start_order.h Wait) rather than leave nothing serving.
    close(listen_);
    unlinkOwnSocket();
    // Every open secret request ends unanswered with the server (its asker ends with it), each logged.
    hub_->dropSecrets();
    // Every sign-in is dropped and its process ended.
    hub_->dropSignIns();
    signIns_.reset();
    // After the control socket closes, so no window sends a prompt to a driver being ended.
    if (!endDrivers() && status == 0) {
        std::fprintf(stderr, "nexus-server: the store failed while the drivers were ended; their turns are cut when "
            "the server next starts\n");
        status = 1;
    }
    services_->stop();
    // On a clean stop or drain, each window is told the session ended (mosh's shutdown), so it makes a session with
    // the next server at once rather than after kQuietMs of silence (sync/redial.h). Skipped when the store failed: a
    // failed pass broke out before ticking its links so unflushed actions stay unacknowledged. Bounded: a window that
    // does not acknowledge within kLinkShutdownMs learns by the silence instead.
    const std::size_t told = status == 0 ? shutDownLinks() : 0;
    std::fprintf(stderr, "nexus-server: %s, %zu link%s open, %zu acknowledged its end\n",
        drained ? "drained" : "stopped", links_.size(), links_.size() == 1 ? "" : "s", told);
    return status != 0 ? status : (drained ? kDrainExit : 0);
}

// Starts `self serve --dir <dir>` as a server of its own: a new session (no terminal), standard input from /dev/null,
// standard output and error to the log, and no other file of this process open, so that ssh's channel closes when
// `new` exits.
bool startServer(const Paths& p, const std::string& self, pid_t* started, std::string* why) {
    posix_spawn_file_actions_t files;
    posix_spawn_file_actions_init(&files);
    posix_spawn_file_actions_addopen(&files, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&files, 1, p.log.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0600);
    posix_spawn_file_actions_adddup2(&files, 1, 2);
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    // No other file of this process: on macOS by POSIX_SPAWN_CLOEXEC_DEFAULT, on Linux by glibc's addclosefrom_np.
#if defined(__APPLE__)
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSID | POSIX_SPAWN_CLOEXEC_DEFAULT);
#else
    posix_spawn_file_actions_addclosefrom_np(&files, 3);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSID);
#endif
    std::vector<std::string> args = {self, "serve", "--dir", p.dir};
    std::vector<char*> argv;
    for (std::string& a : args)
        argv.push_back(a.data());
    argv.push_back(nullptr);
    pid_t pid = 0;
    const int rc = posix_spawn(&pid, self.c_str(), &files, &attr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&files);
    posix_spawnattr_destroy(&attr);
    if (rc != 0) {
        *why = "cannot start `" + self + " serve`: " + std::strerror(rc);
        return false;
    }
    *started = pid;
    return true;
}

// The request's reply line from the server on the socket, one try (`stop`, `config`).
bool askOnce(const Paths& p, const std::string& request, std::string* reply, std::string* why) {
    const int fd = connectControl(p.socket);
    if (fd < 0) {
        *why = errnoText("no server answers on " + p.socket);
        return false;
    }
    const bool ok = writeAll(fd, request + "\n") && readLine(fd, reply, kControlTimeoutMs * 2);
    close(fd);
    if (!ok) {
        *why = "the server on " + p.socket + " did not answer `" + request + "`; see " + p.log;
        return false;
    }
    return true;
}

// A service status response lists the children before the terminal OK or ERROR.
int printControlReport(const Paths& p, const std::string& request) {
    const int fd = connectControl(p.socket);
    if (fd < 0) {
        std::fprintf(stderr, "nexus-server: %s; start the server before requesting %s\n",
            errnoText("no server answers on " + p.socket).c_str(), request.c_str());
        return 1;
    }
    std::string line;
    bool complete = false;
    if (writeAll(fd, request + "\n")) {
        while (readLine(fd, &line, 30000)) {
            if (line == "OK" || line.starts_with("ERROR")) {
                complete = true;
                break;
            }
            std::puts(line.c_str());
        }
    }
    close(fd);
    if (complete && line == "OK") return 0;
    std::fprintf(stderr, "nexus-server: %s\n", complete ? line.c_str() : "the server did not finish its reply");
    return 1;
}

// The request's reply line from the running server, starting one (`self` serve) when none answers: for `new`. It waits
// on the outcomes a start reports (server/start_order.h), not a fixed time: a reply from a serving server; or the
// started server's exit, 0 when another said it serves (asked again), else its code and the log, said as they are. A
// connection closed unanswered is a server that has accepted `stop` (or ended): asked again, and once its socket goes a
// server is started, which waits for the store's lock. kNewLimitMs bounds it all, for a server neither serving nor
// exiting, as a last resort.
bool ask(const Paths& p, const std::string& request, const std::string& self, std::string* reply, std::string* why) {
    if (self.empty()) {
        return askOnce(p, request, reply, why);
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kNewLimitMs);
    pid_t child = -1;           // the server this `new` started and has not yet collected
    std::string last = "no server answered";
    for (;;) {
        const int fd = connectControl(p.socket);
        if (fd >= 0) {
            const bool ok = writeAll(fd, request + "\n") && readLine(fd, reply, kControlTimeoutMs * 2);
            close(fd);
            if (ok) {
                return true;
            }
            last = "the server on " + p.socket + " closed the request unanswered (stopping, or ended)";
        } else if (errno != ENOENT && errno != ECONNREFUSED) {
            *why = errnoText("no server answers on " + p.socket);
            return false;
        } else if (child < 0) {
            std::string dirWhy;
            if (!ensureHome(p, &dirWhy)) {
                *why = dirWhy;
                return false;
            }
            if (!startServer(p, self, &child, why)) {
                return false;
            }
            last = "the server started has not yet said it serves";
        }
        if (child >= 0) {
            int status = 0;
            const pid_t done = waitpid(child, &status, WNOHANG);
            if (done == child) {
                child = -1;
                const bool another = WIFEXITED(status) && WEXITSTATUS(status) == kRefusedExit;
                if (!another) {
                    *why = "the server started exited " + (WIFEXITED(status) ?
                        "with code " + std::to_string(WEXITSTATUS(status)) :
                        "on signal " + std::to_string(WTERMSIG(status))) + " without serving; see " + p.log;
                    return false;
                }
                last = "the server started found another serving, which then did not answer";
            }
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            *why = last + " within " + std::to_string(kNewLimitMs / 1000) + " s; see " + p.log;
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

} // namespace

bool paths(const std::string& dir, Paths* out, std::string* why) {
    // HOME is needed with --dir too: a friend's directory in the drivers file may start with ~, and a driver finds its
    // account under it.
    std::string defaultDir;
    if (!serverHome(std::getenv("HOME"), &out->home, &out->top, &defaultDir, why)) {
        return false;
    }
    std::string d = dir.empty() ? defaultDir : dir;
    while (d.size() > 1 && d.back() == '/') {
        d.pop_back();
    }
    if (d != defaultDir) {
        out->top.clear();
    }
    out->dir = d;
    out->config = d + "/config";
    out->socket = d + "/" + kSocketName;
    out->log = d + "/server.log";
    out->store = d + "/store";
    out->drivers = d + "/drivers";
    out->accounts = accountsDir(out->home);
    sockaddr_un addr;
    if (!socketAddress(out->socket, &addr)) {
        *why = out->socket + " is longer than a Unix socket's address holds (" + std::to_string(sizeof addr.sun_path - 1) +
            " bytes); give a shorter directory with --dir";
        return false;
    }
    return true;
}

int serve(const Paths& p) {
    umask(077);
    signal(SIGPIPE, SIG_IGN);
    struct sigaction sa{};
    sa.sa_handler = onSignal;
    sigaction(SIGTERM, &sa, nullptr);
    sigaction(SIGINT, &sa, nullptr);
    Server server(p);
    return server.run();
}

int servicesCommand(const Paths& p, const std::vector<std::string>& args) {
    if (args.size() == 1 && args[0] == "check") {
        std::vector<ManagedService> rows;
        std::string why;
        if (!readServiceManifest(p.dir + "/services.json", rows, why)) {
            std::fprintf(stderr, "nexus-server: services: %s\n", why.c_str());
            return 1;
        }
        std::printf("%zu managed service%s\n", rows.size(), rows.size() == 1 ? "" : "s");
        return 0;
    }
    if (args.size() == 1 && (args[0] == "status" || args[0] == "reload")) {
        signal(SIGPIPE, SIG_IGN);
        return printControlReport(p, "services " + args[0]);
    }
    std::fprintf(stderr, "usage: nexus-server services check|reload|status [--dir DIR]\n");
    return 2;
}

bool validClientId(const std::string& id) {
    if (id.empty() || id.size() > 64)
        return false;
    for (char c : id) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' ||
            c == '_';
        if (!ok)
            return false;
    }
    return true;
}

int config(const Paths& p, const std::vector<std::string>& args) {
    signal(SIGPIPE, SIG_IGN);
    ConfigRepo repo(p.dir + "/nexus-config");
    const std::string verb = args.empty() ? std::string() : args[0];
    auto fail = [](const std::string& why) {
        std::fprintf(stderr, "nexus-server: config: %s\n", why.c_str());
        return 1;
    };
    if (verb == "path" && args.size() == 1) {
        std::printf("%s\n%s\n%s\n", repo.dir().c_str(), repo.accountsPath().c_str(), repo.friendsPath().c_str());
        return 0;
    }
    if (verb == "sign-in" && args.size() == 2) {
        // Asks the running server once a second until the account is signed in or cannot be: one line out, `signed
        // in` or `refused: <why>`, and nothing of the sign-in itself. An AI asks this; the person signs in in the
        // browser from nexus's window. It gives up a minute past a sign-in's deadline.
        const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(sync::kSignInDeadlineMs) +
            std::chrono::minutes(1);
        for (;;) {
            const int fd = connectControl(p.socket);
            if (fd < 0) {
                std::printf("refused: no server answers on %s; `nexus-server new` starts it\n", p.socket.c_str());
                return 1;
            }
            std::string line;
            const bool asked = writeAll(fd, "config sign-in " + args[1] + "\n") && readLine(fd, &line, 30000);
            close(fd);
            if (!asked) {
                std::printf("refused: the server did not answer\n");
                return 1;
            }
            if (line == "OK signed in") {
                std::printf("signed in\n");
                return 0;
            }
            if (line.rfind("OK ", 0) != 0) {
                std::printf("refused: %s\n", line.rfind("ERROR ", 0) == 0 ? line.c_str() + 6 : line.c_str());
                return 1;
            }
            if (std::chrono::steady_clock::now() >= until) {
                std::printf("refused: %s was not signed in within %llu minutes\n", args[1].c_str(),
                    static_cast<unsigned long long>(sync::kSignInDeadlineMs / 60000 + 1));
                return 1;
            }
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }
    if (verb != "reload" && verb != "restore" && access((repo.dir() + "/.git").c_str(), F_OK) != 0) {
        return fail(repo.dir() + " is not made yet: nexus-server makes it when it starts (`nexus-server new` starts "
            "it); write accounts.json and friends.json there (docs/accounts.md), and the first start takes them");
    }
    if (verb == "check" && args.size() == 1) {
        accounts::Registry working;
        accounts::Registry head;
        std::vector<std::string> errors;
        if (!repo.working(&working, &errors)) {
            for (const std::string& e : errors) {
                std::printf("error: %s\n", e.c_str());
            }
            std::printf("ERROR %zu problem%s; nothing was changed\n", errors.size(), errors.size() == 1 ? "" : "s");
            return 1;
        }
        std::vector<std::string> headErrors;
        if (!repo.committed(&head, &headErrors, false)) {   // a check writes nothing, not even an upgrade
            head = accounts::Registry{};
        }
        const std::vector<std::string> lines = accounts::changes(head, working);
        for (const std::string& l : lines) {
            std::printf("would apply: %s\n", l.c_str());
        }
        std::printf("OK the files read and agree; %s\n", lines.empty() ? "they hold what the server last applied" :
            "`nexus-server config reload` applies them (sessions on accounts no longer linked move, and are told)");
        return 0;
    }
    if (verb == "history" && args.size() == 1) {
        std::vector<std::string> lines;
        std::string why;
        if (!repo.history(&lines, &why)) {
            return fail(why);
        }
        for (const std::string& l : lines) {
            std::printf("%s\n", l.c_str());
        }
        return 0;
    }
    if (verb == "remote" && args.size() <= 2) {
        std::string url;
        std::string why;
        if (args.size() == 1) {
            if (!repo.remote(&url, &why)) {
                return fail(why);
            }
            std::printf("%s\n", url.empty() ? "none" : url.c_str());
            return 0;
        }
        if (args[1] == "--clear") {
            return repo.clearRemote(&why) ? 0 : fail(why);
        }
        return repo.setRemote(args[1], &why) ? 0 : fail(why);
    }
    if ((verb == "reload" && args.size() == 1) || (verb == "restore" && args.size() == 2)) {
        const int fd = connectControl(p.socket);
        if (fd < 0) {
            return fail(errnoText("no server answers on " + p.socket) + "; `nexus-server new` starts it, and a "
                "server that starts applies what the repository's last commit holds");
        }
        const std::string request = verb == "reload" ? std::string("config reload") : "config restore " + args[1];
        if (!writeAll(fd, request + "\n")) {
            close(fd);
            return fail("the request could not be written to " + p.socket);
        }
        std::string line;
        int status = 1;
        while (readLine(fd, &line, 30000)) {
            if (line == "OK") {
                status = 0;
                break;
            }
            std::printf("%s\n", line.c_str());
            if (line.rfind("ERROR", 0) == 0) {
                break;
            }
        }
        close(fd);
        return status;
    }
    std::fprintf(stderr, "usage: nexus-server config path|check|reload|history [--dir DIR]\n"
                         "       nexus-server config restore <commit> [--dir DIR]\n"
                         "       nexus-server config remote [<url> | --clear] [--dir DIR]\n"
                         "       nexus-server config sign-in <account> [--dir DIR]\n"
                         "The accounts and friends are DIR/nexus-config/accounts.json and friends.json\n"
                         "(docs/accounts.md)\n");
    return 2;
}

int historyCommand(const Paths& p, const std::vector<std::string>& args) {
    if (args.size() != 3 || std::any_of(args.begin(), args.end(), [](const auto& arg) {
            return arg.empty() || arg.size() > 20 || arg.find_first_not_of("0123456789") != arg.npos;
        })) return 1;
    const int fd = connectControl(p.socket);
    if (fd < 0) return 1;
    bool ok = writeAll(fd, "history-page " + args[0] + " " + args[1] + " " + args[2] + "\n");
    char buf[8192];
    ssize_t n = 0;
    std::size_t bytes = 0;
    while (ok && (n = read(fd, buf, sizeof buf)) > 0) {
        bytes += static_cast<std::size_t>(n);
        ok = bytes <= 1024 * 1024 && std::fwrite(buf, 1, static_cast<std::size_t>(n), stdout) == static_cast<std::size_t>(n);
    }
    close(fd);
    return ok && n == 0 && bytes ? 0 : 1;
}

int storeCommand(const Paths& p, const std::vector<std::string>& args) {
    std::string forBuild;
    if (args.size() == 3 && args[0] == "check" && args[1] == "--for") {
        forBuild = args[2];
    } else if (args.size() != 1 || args[0] != "check") {
        std::fprintf(stderr, "usage: nexus-server store check [--for <build-hash>] [--dir DIR]\n"
                             "Reads DIR/store without changing it; --for checks whether placed build <build-hash>\n"
                             "can read DIR/store and DIR/nexus-config\n");
        return 2;
    }
    std::unique_ptr<store::Fs> fs = store::make_posix_fs();
    if (!forBuild.empty()) {
        if (!builds::isHash(forBuild)) {
            std::fprintf(stderr, "nexus-server: `%s` is not a build's hash (64 hex digits; `nexus-server builds list`)\n",
                         forBuild.c_str());
            return builds::kExitRefused;
        }
        builds::Manifest m;
        const store::Status ms = builds::checkBuild(p.dir + "/" + builds::kBuildsDir, forBuild, &m);
        if (ms.code() == store::Status::Code::NotFound) {
            std::fprintf(stderr, "nexus-server: build %s is not placed (`nexus-server builds list`)\n",
                         forBuild.c_str());
            return builds::kExitRefused;
        }
        if (!ms.is_ok()) {
            std::fprintf(stderr, "nexus-server: %s\n", ms.message().c_str());
            return 1;
        }
        builds::DiskFormats disk;
        const store::Status ds = migration::readDiskFormats(*fs, p.dir, &disk);
        if (!ds.is_ok()) {
            std::fprintf(stderr, "nexus-server: %s\n", ds.message().c_str());
            return 1;
        }
        const std::string ref = builds::diskFormatsRefusal(m.formats, forBuild, disk);
        if (!ref.empty()) {
            std::fprintf(stderr, "nexus-server: %s\n", ref.c_str());
            return builds::kExitRefused;
        }
        std::printf("OK store %s and config are readable by build %s (readers: store-record %u, store-snapshot %u, "
                    "config %u); nothing was changed\n",
                    p.store.c_str(), forBuild.c_str(), disk.storeRecordReader, disk.storeSnapshotReader,
                    disk.configVersion);
        return 0;
    }
    store::MemFs copy;
    std::unique_ptr<store::Store> s;
    store::OpenReport r;
    const store::Status st = store::Store::inspect(*fs, p.store, copy, s, &r);
    if (!st.is_ok()) {
        std::fprintf(stderr, "nexus-server: %s\n", st.message().c_str());
        return 1;
    }
    const std::string skippedText = r.skipped == 0 ? std::string() :
        ", " + std::to_string(r.skipped) + " later-format field" + (r.skipped == 1 ? "" : "s") + " skipped";
    std::printf("OK store %s: %s, %zu session%s, next session %llu, %llu record%s after the snapshot, %llu bytes of torn "
                "tail a server's open would cut%s; nothing was changed\n",
                p.store.c_str(), r.fresh ? "empty" : "read",
                s->sessions().size(), s->sessions().size() == 1 ? "" : "s",
                static_cast<unsigned long long>(s->next_session()), static_cast<unsigned long long>(r.records),
                r.records == 1 ? "" : "s", static_cast<unsigned long long>(r.cut_bytes), skippedText.c_str());
    return 0;
}

sync::Hello helloOf(const Client& client) {
    return client.hasHello ? client.hello : sync::statedFormat(client.format);
}

std::string newRequest(const Client& client) {
    std::string request = client.id.empty() ? std::string("new") :
        "new " + client.id + " " + std::to_string(client.next);
    if (client.format != 0) {
        request += " format " + std::to_string(client.format);
    }
    if (client.hasHello) {
        request += " hello " + sync::helloText(client.hello);
    }
    return request;
}

bool parseNewRequest(const std::string& request, Client* out) {
    std::vector<std::string> words;
    std::size_t at = 0;
    while (at <= request.size()) {
        const std::size_t space = request.find(' ', at);
        const std::size_t end = space == std::string::npos ? request.size() : space;
        words.push_back(request.substr(at, end - at));
        at = end + 1;
    }
    Client c;
    std::uint64_t number = 0;
    if (words.size() >= 2 && words[words.size() - 2] == "hello") {
        std::string why;
        if (!sync::readHello(words.back(), &c.hello, &why)) {
            return false;
        }
        c.hasHello = true;
        words.resize(words.size() - 2);
    }
    if (words.size() >= 2 && words[words.size() - 2] == "format") {
        if (!positive(words.back(), 9, &number)) {
            return false;
        }
        c.format = static_cast<std::uint32_t>(number);
        words.resize(words.size() - 2);
    }
    if (words.empty() || words[0] != "new" || (words.size() != 1 && words.size() != 3)) {
        return false;
    }
    if (words.size() == 3) {
        if (!validClientId(words[1]) || !positive(words[2], 19, &number)) {
            return false;
        }
        c.id = words[1];
        c.next = number;
    }
    *out = c;
    return true;
}

std::string formatRefusalReply(const Client& client, const sync::Hello& server) {
    const std::string why = sync::refusalOfWindow(helloOf(client), server);
    return why.empty() ? std::string() : "ERROR " + why;
}

int newSession(const Paths& p, const std::string& self, const Client& client) {
    umask(077);
    signal(SIGPIPE, SIG_IGN);
    std::string reply;
    std::string why;
    if (!ask(p, newRequest(client), self, &reply, &why)) {
        std::fprintf(stderr, "nexus-server: %s\n", why.c_str());
        return 1;
    }
    // A running server from before Hello refuses a request that states one, and one from before formats a request
    // that states a format: ask it again without the Hello, then without the format. Its line states what it speaks
    // (no format: format 1).
    Client unstated = client;
    while (reply.compare(0, 22, "ERROR unknown request;") == 0 && (unstated.hasHello || unstated.format != 0)) {
        if (unstated.hasHello) {
            unstated.hasHello = false;
        } else {
            unstated.format = 0;
        }
        if (!ask(p, newRequest(unstated), self, &reply, &why)) {
            std::fprintf(stderr, "nexus-server: %s\n", why.c_str());
            return 1;
        }
    }
    if (!link::parseConnect(reply, nullptr)) {
        // Not a CONNECT line, so it holds no key: say what the server said.
        std::fprintf(stderr, "nexus-server: %s\n", reply.compare(0, 6, "ERROR ") == 0 ? reply.c_str() + 6 :
            "the server's reply is not a CONNECT line");
        return 1;
    }
    std::printf("%s\n", reply.c_str());
    std::fflush(stdout);
    return 0;
}

int stop(const Paths& p) {
    signal(SIGPIPE, SIG_IGN);
    std::string reply;
    std::string why;
    if (!ask(p, "stop", std::string(), &reply, &why)) {
        std::fprintf(stderr, "nexus-server: %s\n", why.c_str());
        return 1;
    }
    if (reply != "OK") {
        std::fprintf(stderr, "nexus-server: the server answered `stop` with: %s\n", reply.c_str());
        return 1;
    }
    supervision::noteIntentionalStop(p.dir);
    // The server removes its socket as it ends: wait for that, so that a server started next does not find it.
    for (int i = 0; i < 250 && access(p.socket.c_str(), F_OK) == 0; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    return 0;
}

int drain(const Paths& p) {
    signal(SIGPIPE, SIG_IGN);
    std::string reply;
    std::string why;
    if (!ask(p, "drain", std::string(), &reply, &why)) {
        std::fprintf(stderr, "nexus-server: %s\n", why.c_str());
        return 1;
    }
    if (reply != "OK") {
        std::fprintf(stderr, "nexus-server: the server answered `drain` with: %s\n", reply.c_str());
        return 1;
    }
    return 0;
}

int status(const Paths& p) {
    signal(SIGPIPE, SIG_IGN);
    const int fd = connectControl(p.socket);
    if (fd < 0) {
        std::fprintf(stderr, "nexus-server: %s\n", errnoText("no server answers on " + p.socket).c_str());
        return 1;
    }
    std::string first;
    std::string second;
    const bool ok = writeAll(fd, std::string(kStatusRequest) + "\n") && readLine(fd, &first, kStatusReplyMs);
    if (ok && first == kServingReply && !readLine(fd, &second, kStatusReplyMs)) {
        second = "build unknown (a server from before builds were placed)";
    }
    close(fd);
    if (!ok) {
        std::fprintf(stderr, "nexus-server: the server on %s did not answer `status`; see %s\n", p.socket.c_str(),
                     p.log.c_str());
        return 1;
    }
    std::printf("%s\n", first.c_str());
    if (!second.empty()) {
        std::printf("%s\n", second.c_str());
    }
    return first == kServingReply ? 0 : 1;
}

} // namespace nexus::server
