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
#include "sync/hub.h"

extern char** environ;

namespace nexus::server {

namespace {

constexpr std::size_t kMaxSessions = 64;
constexpr int kControlTimeoutMs = 2000;     // for one request or reply on the control socket
constexpr int kStartTimeoutMs = 5000;       // for a server `new` started to answer
constexpr int kMaxWaitMs = 1000;

volatile sig_atomic_t gStop = 0;

void onSignal(int) {
    gStop = 1;
}

std::string errnoText(const std::string& what) {
    return what + ": " + std::strerror(errno);
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

struct Session {
    std::uint64_t id;
    std::unique_ptr<link::ServerSession> link;
};

class Server {
public:
    explicit Server(const Paths& p) : paths_(p), hub_(store_) {}
    int run();

private:
    bool listenControl(std::string* why);
    void control(int fd);
    Paths paths_;
    sync::MemoryStore store_;
    sync::Hub hub_;
    std::vector<Session> sessions_;
    std::uint64_t nextId_ = 1;
    int listen_ = -1;
    bool stopping_ = false;
};

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
    gid_t gid = 0;
    if (getpeereid(fd, &uid, &gid) != 0 || uid != getuid()) {
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
    if (request != "new") {
        writeAll(fd, "ERROR unknown request; the requests are new and stop\n");
        return;
    }
    if (sessions_.size() >= kMaxSessions) {
        writeAll(fd, "ERROR the server holds " + std::to_string(kMaxSessions) + " sessions, its limit\n");
        return;
    }
    try {
        Session s{nextId_++, std::make_unique<link::ServerSession>(hub_.state())};
        // The key goes on this one line to `nexus-server new`, and nowhere else.
        if (!writeAll(fd, link::connectLine(s.link->port(), s.link->key()) + "\n")) {
            std::fprintf(stderr, "nexus-server: session %llu: the CONNECT line could not be written; dropped\n",
                static_cast<unsigned long long>(s.id));
            return;
        }
        std::fprintf(stderr, "nexus-server: session %llu on UDP port %s\n", static_cast<unsigned long long>(s.id),
            s.link->port().c_str());
        sessions_.push_back(std::move(s));
    } catch (const std::exception& e) {
        writeAll(fd, std::string("ERROR ") + e.what() + "\n");
        std::fprintf(stderr, "nexus-server: %s\n", e.what());
    }
}

int Server::run() {
    std::string why;
    if (!ensureDir(paths_.dir, &why) || !listenControl(&why)) {
        std::fprintf(stderr, "nexus-server: %s\n", why.c_str());
        return 1;
    }
    std::fprintf(stderr, "nexus-server: serving on %s\n", paths_.socket.c_str());
    while (!stopping_ && !gStop) {
        link::freezeClock();
        for (Session& s : sessions_) {
            if (!s.link->tick(&why))
                std::fprintf(stderr, "nexus-server: session %llu: %s\n", static_cast<unsigned long long>(s.id),
                    why.c_str());
        }
        for (auto it = sessions_.begin(); it != sessions_.end();) {
            if (it->link->ended()) {
                std::fprintf(stderr, "nexus-server: session %llu ended by its client after %llu actions\n",
                    static_cast<unsigned long long>(it->id), static_cast<unsigned long long>(it->link->applied()));
                it = sessions_.erase(it);
            } else {
                ++it;
            }
        }
        std::vector<pollfd> fds{pollfd{listen_, POLLIN, 0}};
        std::vector<std::size_t> owner{0};
        int wait = kMaxWaitMs;
        for (std::size_t i = 0; i < sessions_.size(); ++i) {
            wait = std::min(wait, std::max(0, sessions_[i].link->waitMs()));
            for (int fd : sessions_[i].link->fds()) {
                fds.push_back(pollfd{fd, POLLIN, 0});
                owner.push_back(i);
            }
        }
        if (::poll(fds.data(), static_cast<nfds_t>(fds.size()), wait) < 0 && errno != EINTR) {
            std::fprintf(stderr, "nexus-server: %s\n", errnoText("poll").c_str());
            break;
        }
        link::freezeClock();
        if (fds[0].revents & POLLIN) {
            const int fd = accept(listen_, nullptr, nullptr);
            if (fd >= 0) {
                fcntl(fd, F_SETFL, 0);
                control(fd);
                close(fd);
            }
        }
        bool changed = false;
        for (std::size_t k = 1; k < fds.size(); ++k) {
            if (!(fds[k].revents & POLLIN))
                continue;
            Session& s = sessions_[owner[k]];
            if (!s.link->receive(&why)) {
                std::fprintf(stderr, "nexus-server: session %llu: %s\n", static_cast<unsigned long long>(s.id),
                    why.c_str());
                continue;
            }
            std::string problem;
            for (const sync::Action& a : s.link->take(&problem)) {
                std::string refused;
                hub_.apply(a, &refused);
                changed = true;
                if (!refused.empty())
                    std::fprintf(stderr, "nexus-server: session %llu, action %llu: %s\n",
                        static_cast<unsigned long long>(s.id), static_cast<unsigned long long>(a.num),
                        refused.c_str());
            }
            if (!problem.empty())
                std::fprintf(stderr, "nexus-server: session %llu: %s\n", static_cast<unsigned long long>(s.id),
                    problem.c_str());
        }
        if (changed) {
            for (Session& s : sessions_)
                s.link->setState(hub_.state());
        }
    }
    close(listen_);
    unlink(paths_.socket.c_str());
    std::fprintf(stderr, "nexus-server: stopped, %zu session%s open\n", sessions_.size(),
        sessions_.size() == 1 ? "" : "s");
    return 0;
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
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSID | POSIX_SPAWN_CLOEXEC_DEFAULT);
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
        if (!ensureDir(p.dir, &dirWhy)) {
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
    std::string d = dir;
    if (d.empty()) {
        const char* home = std::getenv("HOME");
        if (home == nullptr || *home == 0) {
            *why = "HOME is not set; give the directory with --dir";
            return false;
        }
        d = std::string(home) + "/.nexus";
    }
    while (d.size() > 1 && d.back() == '/')
        d.pop_back();
    out->dir = d;
    out->socket = d + "/server.sock";
    out->log = d + "/server.log";
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

int newSession(const Paths& p, const std::string& self) {
    umask(077);
    signal(SIGPIPE, SIG_IGN);
    std::string reply;
    std::string why;
    if (!ask(p, "new", self, &reply, &why)) {
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
