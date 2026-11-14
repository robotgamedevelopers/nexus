#include "server/server.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>

#include "link/bootstrap.h"
#include "link/mosh_link.h"
#include "server/config_repo.h"
#include "server/drivers.h"
#include "server/durable.h"
#include "server/home.h"
#include "store/fs.h"
#include "sync/hub.h"

extern char** environ;

namespace nexus::server {

namespace {

constexpr std::size_t kMaxLinks = 64;      // windows' links (mosh sessions) at once
constexpr int kControlTimeoutMs = 2000;     // for one request or reply on the control socket
constexpr int kStartTimeoutMs = 5000;       // for a server `new` started to answer
constexpr int kMaxWaitMs = 1000;

volatile sig_atomic_t gStop = 0;

void onSignal(int) {
    gStop = 1;
}

// Milliseconds on the steady clock: the drivers' deadlines.
Millis nowMs() {
    return static_cast<Millis>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

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
bool readConfig(const std::string& path, std::string* why) {
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
    const bool ok = readServerConfig(text, path, &warnings, why);
    for (const std::string& w : warnings) {
        std::fprintf(stderr, "nexus-server: %s\n", w.c_str());
    }
    return ok;
}

// "new", or "new <client> <next>" with a valid id and a number from 1.
bool parseNew(const std::string& request, Client* out) {
    if (request == "new")
        return true;
    if (request.compare(0, 4, "new ") != 0)
        return false;
    const std::size_t space = request.find(' ', 4);
    if (space == std::string::npos)
        return false;
    const std::string id = request.substr(4, space - 4);
    const std::string num = request.substr(space + 1);
    if (!validClientId(id) || num.empty() || num.size() > 19 ||
            num.find_first_not_of("0123456789") != std::string::npos)
        return false;
    const std::uint64_t next = std::strtoull(num.c_str(), nullptr, 10);
    if (next == 0)
        return false;
    out->id = id;
    out->next = next;
    return true;
}

// One window's link: a mosh session, with the client it named. (The sessions with friends are the hub's.)
struct Link {
    std::uint64_t id;
    std::string client;     // empty: a session that did not name its client
    std::unique_ptr<link::ServerSession> link;
};

class Server {
public:
    explicit Server(const Paths& p) : paths_(p) {}
    int run();

private:
    bool openStore(std::string* why);
    bool listenControl(std::string* why);
    void control(int fd);
    bool endDrivers();
    void configApply(int fd, const std::string& request);
    Paths paths_;
    std::unique_ptr<store::Fs> fs_;
    DurableStore store_;
    std::unique_ptr<Drivers> drivers_;
    std::unique_ptr<sync::Hub> hub_;
    std::unique_ptr<ConfigRepo> config_;
    std::vector<Link> links_;
    std::uint64_t nextId_ = 1;
    int listen_ = -1;
    bool stopping_ = false;
};

bool Server::openStore(std::string* why) {
    fs_ = store::make_posix_fs();
    if (!store_.open(*fs_, paths_.store, why))
        return false;
    drivers_ = std::make_unique<Drivers>(store_, Drivers::Options{paths_.drivers, paths_.home, Timing{}, nowMs});
    hub_ = std::make_unique<sync::Hub>(store_, sync::Limits{}, drivers_.get());
    drivers_->attach(*hub_);
    // The accounts and friends: what the config repository's HEAD holds (server/config_repo.h). The sessions are
    // settled against them before any driver starts.
    config_ = std::make_unique<ConfigRepo>(paths_.dir + "/nexus-config");
    std::string configWhy;
    accounts::Registry registry;
    std::vector<std::string> report;
    if (!config_->open(&configWhy)) {
        std::fprintf(stderr, "nexus-server: config: %s; the server holds no accounts\n", configWhy.c_str());
    } else if (!config_->committed(&registry, &report)) {
        for (const std::string& l : report)
            std::fprintf(stderr, "nexus-server: config: %s\n", l.c_str());
        std::fprintf(stderr, "nexus-server: config: the committed accounts and friends do not read, so the server "
            "holds none until `nexus-server config reload` applies good ones\n");
        hub_->keepWith(config_.get());
    } else {
        for (const std::string& l : report)
            std::fprintf(stderr, "nexus-server: config: %s\n", l.c_str());
        for (const std::string& l : hub_->useAccounts(registry, config_.get()))
            std::fprintf(stderr, "nexus-server: config: %s\n", l.c_str());
        std::fprintf(stderr, "nexus-server: config: %zu account%s and %zu friend%s from %s\n", registry.accounts.size(),
            registry.accounts.size() == 1 ? "" : "s", registry.friends.size(), registry.friends.size() == 1 ? "" : "s",
            config_->dir().c_str());
    }
    const store::OpenReport& r = store_.report();
    std::fprintf(stderr, "nexus-server: store %s: %s, %zu session%s, %llu record%s replayed, %llu bytes of torn tail "
        "cut\n", paths_.store.c_str(), r.fresh ? "new" : "opened", store_.size(), store_.size() == 1 ? "" : "s",
        static_cast<unsigned long long>(r.records), r.records == 1 ? "" : "s",
        static_cast<unsigned long long>(r.cut_bytes));
    if (r.migrated > 0) {
        std::fprintf(stderr, "nexus-server: store %s: its %llu friend tab%s read as session%s, one each, and "
            "written in the sessions format\n", paths_.store.c_str(), static_cast<unsigned long long>(r.migrated),
            r.migrated == 1 ? "" : "s", r.migrated == 1 ? "" : "s");
    }
    if (!drivers_->start(nowMs())) {
        *why = "the store failed while the friends' drivers were restored; restart the server";
        return false;
    }
    return true;
}

bool Server::listenControl(std::string* why) {
    sockaddr_un addr;
    if (!socketAddress(paths_.socket, &addr)) {
        *why = paths_.socket + " is too long for a Unix socket's address";
        return false;
    }
    const int other = connectControl(paths_.socket);
    if (other >= 0) {
        close(other);
        *why = "a server already answers on " + paths_.socket + "; `nexus-server stop` stops it";
        return false;
    }
    unlink(paths_.socket.c_str());   // a socket left by a server that ended without removing it
    listen_ = socket(AF_UNIX, SOCK_STREAM, 0);
    if (listen_ < 0) {
        *why = errnoText("socket");
        return false;
    }
    if (bind(listen_, reinterpret_cast<const sockaddr*>(&addr), sizeof addr) != 0) {
        *why = errnoText("cannot bind " + paths_.socket);
        return false;
    }
    if (chmod(paths_.socket.c_str(), 0600) != 0 || listen(listen_, 16) != 0) {
        *why = errnoText("cannot listen on " + paths_.socket);
        return false;
    }
    fcntl(listen_, F_SETFL, O_NONBLOCK);
    return true;
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
    if (request == "stop") {
        writeAll(fd, "OK\n");
        stopping_ = true;
        return;
    }
    if (request == "config reload" || request.rfind("config restore ", 0) == 0) {
        configApply(fd, request);
        return;
    }
    Client client;
    if (!parseNew(request, &client)) {
        writeAll(fd, "ERROR unknown request; the requests are `new`, `new <client> <next>`, `stop`, `config reload` "
            "and `config restore <commit>`\n");
        return;
    }
    // A client's new session ends its earlier one: the window has left it.
    if (!client.id.empty()) {
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
        Link s{nextId_++, client.id,
            std::make_unique<link::ServerSession>(hub_->state(), applied, client.next)};
        // The key goes on this one line to `nexus-server new`, and nowhere else.
        if (!writeAll(fd, link::connectLine(s.link->port(), s.link->key()) + "\n")) {
            std::fprintf(stderr, "nexus-server: link %llu: the CONNECT line could not be written; dropped\n",
                static_cast<unsigned long long>(s.id));
            return;
        }
        std::fprintf(stderr, "nexus-server: link %llu on UDP port %s%s%s, from action %llu (applied %llu)\n",
            static_cast<unsigned long long>(s.id), s.link->port().c_str(), s.client.empty() ? "" : ", client ",
            s.client.c_str(), static_cast<unsigned long long>(client.next), static_cast<unsigned long long>(applied));
        links_.push_back(std::move(s));
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
        ok = config_->at(commit, &next, &lines);
        message = "restore: the files as commit " + commit + " held them, applied as a new version";
    }
    if (ok) {
        config_->applying(true);
        ok = hub_->applyAccounts(next, message, &lines);
        config_->applying(false);
        for (const std::string& n : config_->notes())
            lines.push_back(n);
        for (Link& s : links_)
            s.link->setState(hub_->state());
    }
    if (ok && lines.empty())
        lines.push_back("nothing changed: the files already said what the server holds");
    std::string reply;
    for (const std::string& l : lines) {
        reply += (ok ? "applied: " : "refused: ") + l + "\n";
        std::fprintf(stderr, "nexus-server: config %s: %s%s\n", request.c_str() + 7, ok ? "applied: " : "refused: ",
            l.c_str());
    }
    reply += ok ? "OK\n" : "ERROR nothing was applied; the server keeps the accounts and friends it had\n";
    writeAll(fd, reply);
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

int Server::run() {
    std::string why;
    // The store opens before the control socket listens, so no session is made on a state not yet read.
    if (!ensureHome(paths_, &why) || !readConfig(paths_.config, &why) || !openStore(&why) || !listenControl(&why)) {
        std::fprintf(stderr, "nexus-server: %s\n", why.c_str());
        return 1;
    }
    std::fprintf(stderr, "nexus-server: serving on %s\n", paths_.socket.c_str());
    int status = 0;
    while (!stopping_ && !gStop) {
        link::freezeClock();
        for (Link& s : links_) {
            if (!s.link->tick(&why))
                std::fprintf(stderr, "nexus-server: link %llu: %s\n", static_cast<unsigned long long>(s.id),
                    why.c_str());
        }
        for (auto it = links_.begin(); it != links_.end();) {
            if (it->link->ended()) {
                std::fprintf(stderr, "nexus-server: link %llu ended by its client after %llu actions\n",
                    static_cast<unsigned long long>(it->id), static_cast<unsigned long long>(it->link->applied()));
                it = links_.erase(it);
            } else {
                ++it;
            }
        }
        std::vector<pollfd> fds{pollfd{listen_, POLLIN, 0}};
        std::vector<std::size_t> owner{0};
        int wait = std::min(kMaxWaitMs, drivers_->waitMs(nowMs()));
        for (std::size_t i = 0; i < links_.size(); ++i) {
            wait = std::min(wait, std::max(0, links_[i].link->waitMs()));
            for (int fd : links_[i].link->fds()) {
                fds.push_back(pollfd{fd, POLLIN, 0});
                owner.push_back(i);
            }
        }
        // The drivers' pipes last: serve() below looks at every driver, so their events need no owner.
        const std::size_t linkFds = fds.size();
        for (const pollfd& p : drivers_->fds())
            fds.push_back(p);
        if (::poll(fds.data(), static_cast<nfds_t>(fds.size()), wait) < 0 && errno != EINTR) {
            std::fprintf(stderr, "nexus-server: %s\n", errnoText("poll").c_str());
            break;
        }
        link::freezeClock();
        bool changed = false;
        if (!drivers_->serve(nowMs(), &changed)) {
            std::fprintf(stderr, "nexus-server: the store failed; stopping without acknowledging the actions since\n");
            status = 1;
            break;
        }
        for (std::size_t k = 1; k < linkFds; ++k) {
            if (!(fds[k].revents & POLLIN))
                continue;
            Link& s = links_[owner[k]];
            if (!s.link->receive(&why)) {
                std::fprintf(stderr, "nexus-server: link %llu: %s\n", static_cast<unsigned long long>(s.id),
                    why.c_str());
                continue;
            }
            std::string problem;
            // Each action is durable (with the client's counter) before the next tick can acknowledge it.
            for (const sync::Action& a : s.link->take(&problem)) {
                std::string refused;
                hub_->apply(a, &refused, s.client);
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
            // What reached the disk is unknown: stop before acknowledging anything more. A restart reopens the store
            // from what is durable, and the windows reconnect.
            std::fprintf(stderr, "nexus-server: the store failed; stopping without acknowledging the actions since\n");
            status = 1;
            break;
        }
        if (changed) {
            for (Link& s : links_) {
                s.link->setState(hub_->state());
            }
        }
        // The control socket last: a new link may end an earlier one, and the links' fds above are by index.
        if (fds[0].revents & POLLIN) {
            const int fd = accept(listen_, nullptr, nullptr);
            if (fd >= 0) {
                fcntl(fd, F_SETFL, 0);
                control(fd);
                close(fd);
            }
        }
    }
    close(listen_);
    unlink(paths_.socket.c_str());
    // After the control socket closes, so no window sends a prompt to a driver being ended.
    if (!endDrivers() && status == 0) {
        std::fprintf(stderr, "nexus-server: the store failed while the drivers were ended; their turns are cut when "
            "the server next starts\n");
        status = 1;
    }
    std::fprintf(stderr, "nexus-server: stopped, %zu link%s open\n", links_.size(),
        links_.size() == 1 ? "" : "s");
    return status;
}

// Starts `self serve --dir <dir>` as a server of its own: a new session (no terminal), standard input from /dev/null,
// standard output and error to the log, and no other file of this process open, so that ssh's channel closes when
// `new` exits.
bool startServer(const Paths& p, const std::string& self, std::string* why) {
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
    return true;
}

// The request's reply line from the running server, starting one if `self` is given and none answers.
bool ask(const Paths& p, const std::string& request, const std::string& self, std::string* reply, std::string* why) {
    int fd = connectControl(p.socket);
    if (fd < 0 && !self.empty() && (errno == ENOENT || errno == ECONNREFUSED)) {
        std::string dirWhy;
        if (!ensureHome(p, &dirWhy)) {
            *why = dirWhy;
            return false;
        }
        if (!startServer(p, self, why))
            return false;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kStartTimeoutMs);
        while (fd < 0 && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            fd = connectControl(p.socket);
        }
        if (fd < 0) {
            *why = "the server started did not answer on " + p.socket + " within 5 s; see " + p.log;
            return false;
        }
    }
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

} // namespace

bool paths(const std::string& dir, Paths* out, std::string* why) {
    // HOME is needed with --dir too: a friend's directory in the drivers file may start with ~, and a driver finds its
    // account under it.
    std::string d;
    if (!serverHome(std::getenv("HOME"), &out->home, &out->top, &d, why)) {
        return false;
    }
    if (!dir.empty()) {
        d = dir;
        out->top.clear();
    }
    while (d.size() > 1 && d.back() == '/')
        d.pop_back();
    out->dir = d;
    out->config = d + "/config";
    out->socket = d + "/server.sock";
    out->log = d + "/server.log";
    out->store = d + "/store";
    out->drivers = d + "/drivers";
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
    if (verb != "reload" && verb != "restore" && access((repo.dir() + "/.git").c_str(), F_OK) != 0) {
        return fail(repo.dir() + " is not made yet: nexus-server makes it when it starts (`nexus-server new` starts "
            "it); write accounts.json and friends.json there (docs/accounts.md), and the first start takes them");
    }
    if (verb == "check" && args.size() == 1) {
        accounts::Registry working;
        accounts::Registry head;
        std::vector<std::string> errors;
        if (!repo.working(&working, &errors)) {
            for (const std::string& e : errors)
                std::printf("error: %s\n", e.c_str());
            std::printf("ERROR %zu problem%s; nothing was changed\n", errors.size(), errors.size() == 1 ? "" : "s");
            return 1;
        }
        std::vector<std::string> headErrors;
        if (!repo.committed(&head, &headErrors))
            head = accounts::Registry{};
        const std::vector<std::string> lines = accounts::changes(head, working);
        for (const std::string& l : lines)
            std::printf("would apply: %s\n", l.c_str());
        std::printf("OK the files read and agree; %s\n", lines.empty() ? "they hold what the server last applied" :
            "`nexus-server config reload` applies them (sessions on accounts no longer linked move, and are told)");
        return 0;
    }
    if (verb == "history" && args.size() == 1) {
        std::vector<std::string> lines;
        std::string why;
        if (!repo.history(&lines, &why))
            return fail(why);
        for (const std::string& l : lines)
            std::printf("%s\n", l.c_str());
        return 0;
    }
    if (verb == "remote" && args.size() <= 2) {
        std::string url;
        std::string why;
        if (args.size() == 1) {
            if (!repo.remote(&url, &why))
                return fail(why);
            std::printf("%s\n", url.empty() ? "none" : url.c_str());
            return 0;
        }
        if (args[1] == "--clear")
            return repo.clearRemote(&why) ? 0 : fail(why);
        return repo.setRemote(args[1], &why) ? 0 : fail(why);
    }
    if ((verb == "reload" && args.size() == 1) || (verb == "restore" && args.size() == 2)) {
        const int fd = connectControl(p.socket);
        if (fd < 0)
            return fail(errnoText("no server answers on " + p.socket) + "; `nexus-server new` starts it, and a "
                "server that starts applies what the repository's last commit holds");
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
            if (line.rfind("ERROR", 0) == 0)
                break;
        }
        close(fd);
        return status;
    }
    std::fprintf(stderr, "usage: nexus-server config path|check|reload|history [--dir DIR]\n"
                         "       nexus-server config restore <commit> [--dir DIR]\n"
                         "       nexus-server config remote [<url> | --clear] [--dir DIR]\n"
                         "The accounts and friends are DIR/nexus-config/accounts.json and friends.json\n"
                         "(docs/accounts.md)\n");
    return 2;
}

int newSession(const Paths& p, const std::string& self, const Client& client) {
    umask(077);
    signal(SIGPIPE, SIG_IGN);
    std::string reply;
    std::string why;
    const std::string request = client.id.empty() ? std::string("new") :
        "new " + client.id + " " + std::to_string(client.next);
    if (!ask(p, request, self, &reply, &why)) {
        std::fprintf(stderr, "nexus-server: %s\n", why.c_str());
        return 1;
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
    // The server removes its socket as it ends: wait for that, so that a server started next does not find it.
    for (int i = 0; i < 250 && access(p.socket.c_str(), F_OK) == 0; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    return 0;
}

} // namespace nexus::server
