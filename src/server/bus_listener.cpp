#include "server/bus_listener.h"

#include "driver/protocol.h"
#include "server/bus_attachment.h"
#include "sync/bus_peer.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <climits>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <spawn.h>
#include <string_view>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>

namespace nexus::server {
namespace {

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
constexpr auto kRetry = 2s;
constexpr auto kDeliveryTimeout = 8s; // nova-bus's --exec deadline
// Any byte can occupy two bytes after TSV escaping; allow four separators and a final newline.
constexpr std::size_t kMaxFrame = 2 * (kBusAttachmentBytes + 2 * driver::kMaxLineBytes + 2 * 64) + 5;

void closeFd(int& fd) {
    if (fd >= 0) ::close(std::exchange(fd, -1));
}

bool prepareFd(int fd, bool nonblocking = true) {
    return ::fcntl(fd, F_SETFD, FD_CLOEXEC) == 0 &&
           (!nonblocking || ::fcntl(fd, F_SETFL, O_NONBLOCK) == 0);
}

bool socketAddress(const std::string& path, sockaddr_un* address) {
    if (path.empty() || path.size() >= sizeof address->sun_path || path.find('\0') != std::string::npos) return false;
    address->sun_family = AF_UNIX;
    std::memcpy(address->sun_path, path.c_str(), path.size() + 1);
    return true;
}

bool loopback(std::string_view address) {
    if (!address.starts_with("127.0.0.1:")) return false;
    address.remove_prefix(sizeof("127.0.0.1:") - 1);
    unsigned port = 0;
    const auto [end, ec] = std::from_chars(address.data(), address.data() + address.size(), port);
    return ec == std::errc() && end == address.data() + address.size() && port > 0 && port <= 65535;
}

bool busName(std::string_view s) {
    return !s.empty() && s.find_first_not_of(sync::kBusPeerCharacters) == s.npos;
}

bool streamId(std::string_view s) {
    const auto dash = s.find('-');
    return dash != std::string_view::npos && dash > 0 && dash + 1 < s.size() &&
        s.find_first_not_of("0123456789") == dash &&
        s.find_first_not_of("0123456789", dash + 1) == std::string_view::npos;
}

// Decode incrementally. Each connection holds only its decoded fields, including when
// a large body is entirely made of escaped tabs or the chunk ends in the middle of an escape.
struct Frame {
    BusMessage msg;
    bool ended = false;
    bool escaped = false;
    std::size_t field = 0;

    bool feed(std::string_view bytes) {
        const std::array fields{&msg.id, &msg.from, &msg.replyTo, &msg.subject, &msg.body};
        // Nova's bus can name non-friend publishers longer than the registry's 128-byte limit.
        // Preserve their full envelope for the private server inbox.
        constexpr std::array<std::size_t, 5> limits{64, driver::kMaxLineBytes, 64, driver::kMaxLineBytes, kBusAttachmentBytes};
        while (!bytes.empty()) {
            if (ended) return false;
            auto& out = *fields[field];
            if (escaped) {
                if (field == 1 || out.size() == limits[field]) return false; // bus names contain no escaped characters
                switch (bytes.front()) {
                case 't': out.push_back('\t'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case '\\': out.push_back('\\'); break;
                default: return false;
                }
                escaped = false;
                bytes.remove_prefix(1);
                continue;
            }
            const auto n = std::min(bytes.find_first_of("\\\t\r\n"), bytes.size());
            if (n > limits[field] - out.size()) return false;
            out.append(bytes.substr(0, n));
            bytes.remove_prefix(n);
            if (bytes.empty()) break;
            switch (bytes.front()) {
            case '\\': escaped = true; break;
            case '\t':
                if (++field == fields.size()) return false;
                break;
            case '\n':
                if (field + 1 != fields.size() || !streamId(msg.id) || !busName(msg.from) ||
                    (msg.replyTo != "-" && !streamId(msg.replyTo))) return false;
                if (msg.replyTo == "-") msg.replyTo.clear();
                ended = true;
                break;
            default: return false; // an unescaped carriage return
            }
            bytes.remove_prefix(1);
        }
        return true;
    }
};

bool ownedHelper(int fd, pid_t group) {
    pid_t pid = -1;
#ifdef __APPLE__
    socklen_t size = sizeof pid;
    if (::getsockopt(fd, SOL_LOCAL, LOCAL_PEERPID, &pid, &size) != 0 || size != sizeof pid) return false;
#else
    struct ucred peer {};
    socklen_t size = sizeof peer;
    if (::getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &peer, &size) != 0 || size != sizeof peer) return false;
    pid = peer.pid;
#endif
    return group > 0 && pid > 0 && ::getpgid(pid) == group;
}

bool safeWrites(int fd) {
#ifdef SO_NOSIGPIPE
    int on = 1;
    return ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof on) == 0;
#else
    (void)fd;
    return true;
#endif
}

ssize_t writeSocket(int fd, const char* bytes, std::size_t size) {
#ifdef MSG_NOSIGNAL
    return ::send(fd, bytes, size, MSG_NOSIGNAL);
#else
    return ::send(fd, bytes, size, 0);
#endif
}

std::string quote(std::string_view s) {
    std::string result{"'"};
    for (char c : s) {
        if (c == '\'') result += "'\\''";
        else result.push_back(c);
    }
    return result + "'";
}

} // namespace

struct BusListeners::Hook {
    std::string address, directory, path;
    Frame frame;
    int listenFd = -1, output = -1, connection = -1;
    pid_t pid = -1;
    bool complete = false, awaitingFlush = false, badFrameReported = false;
    Clock::time_point retryAt{}, deadline{};
    std::string lastFailure, statusPrefix;

    explicit Hook(std::string redis) : address(std::move(redis)) {}
    ~Hook() { stop(); }

    void disconnect() {
        closeFd(connection);
        frame = {};
        complete = false;
        awaitingFlush = false;
    }

    void closeInbox() {
        disconnect();
        statusPrefix.clear();
        closeFd(listenFd);
        if (!path.empty()) ::unlink(path.c_str());
        if (!directory.empty()) ::rmdir(directory.c_str());
        path.clear();
        directory.clear();
    }

    void retire() {
        closeInbox();
        // Revoke the socket at once, then let nova-bus release its 30s presence key.
        if (pid > 0) ::kill(-pid, SIGTERM);
        deadline = Clock::now() + 2500ms; // the hook bounds Redis release to 2s
    }

    void stop() {
        closeInbox();
        closeFd(output);
        if (pid > 0) {
            // The pid remains ours until reaped. Include any in-flight --exec helper.
            ::kill(-pid, SIGKILL);
            while (::waitpid(pid, nullptr, 0) < 0 && errno == EINTR) {}
            pid = -1;
        }
    }

    bool started() const {
        if (pid <= 0) return false;
        siginfo_t info{};
        int rc;
        do {
            rc = ::waitid(P_PID, static_cast<id_t>(pid), &info, WEXITED | WNOHANG | WNOWAIT);
        } while (rc < 0 && errno == EINTR);
        return rc == 0 && info.si_pid == 0;
    }

    void reportOnce(const std::string& name, const char* issue,
                    const std::function<void(const std::string&)>& report) {
        if (report && lastFailure != issue) report("message bus for " + name + ": " + issue);
        lastFailure = issue;
    }

    // The hook retries a lost Redis connection without exiting. Recognize its fixed status
    // prefixes so that outage is visible even if no message arrives. Never log its output.
    void drainStatus(const std::string& name, const std::function<void(const std::string&)>& report) {
        char bytes[4096];
        for (int i = 0; i < 4; ++i) {
            const ssize_t n = ::read(output, bytes, sizeof bytes);
            if (n <= 0) return;
            for (char c : std::string_view(bytes, static_cast<std::size_t>(n))) {
                if (c != '\n') {
                    if (statusPrefix.size() < 32) statusPrefix += c;
                    continue;
                }
                if (statusPrefix.starts_with("HOOK WAIT name=")) {
                    reportOnce(name, "another hook holds this inbox; waiting for it to release", report);
                } else if (statusPrefix.starts_with("nova-bus hook: redis") ||
                           statusPrefix.starts_with("nova-bus hook: cannot reach")) {
                    reportOnce(name, "the bus is unavailable; check its Redis endpoint and credentials", report);
                } else if (statusPrefix.starts_with("HOOK OK name=") || statusPrefix.starts_with("HOOK RECONNECTED name=")) {
                    lastFailure.clear();
                }
                statusPrefix.clear();
            }
        }
    }

    std::string start(const std::string& home, const std::string& self, const std::string& name) {
        if (!home.starts_with('/') || !self.starts_with('/')) return "the home and server executable must be absolute paths";
        char temp[] = "/tmp/nexus-bus-XXXXXX";
        if (!::mkdtemp(temp)) return "cannot create a private directory for delivery";
        directory = temp;
        path = directory + "/receive";
        sockaddr_un local{};
        listenFd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (!socketAddress(path, &local) || listenFd < 0 || !prepareFd(listenFd) ||
            ::bind(listenFd, reinterpret_cast<const sockaddr*>(&local), sizeof local) != 0 ||
            ::chmod(path.c_str(), 0600) != 0 || ::listen(listenFd, 4) != 0) return "cannot open the inbox's private socket";

        // The hook detects its reader's disappearance if nexus-server crashes. Never route
        // its stdout or stderr into the transcript or copy their possibly sensitive contents.
        int pipes[2];
        if (::pipe(pipes) < 0) return "cannot open a pipe for the hook";
        output = pipes[0];
        int writer = pipes[1];
        if (!prepareFd(output) || !prepareFd(writer, false)) {
            closeFd(writer);
            return "cannot configure the hook's pipe";
        }

        const std::string broker = home + "/.local/bin/nova-secrets", bus = home + "/.local/bin/nova-bus";
        std::string command = "exec " + quote(self) + " bus-deliver " + quote(path);
        std::vector<std::string> args{broker, "exec", "--only", "NOVA_BUS_PASSWORD", "--", bus,
                                     "hook", "--as", name, "--exec", std::move(command)};
        std::array<std::string, 4> env{"HOME=" + home, "PATH=/opt/homebrew/bin:/usr/local/bin:/usr/bin:/bin",
                                       "NOVA_BUS_REDIS=" + address, "NOVA_BUS_USER=bus"};
        std::vector<char*> argv, envp;
        for (auto& arg : args) argv.push_back(arg.data());
        for (auto& item : env) envp.push_back(item.data());
        argv.push_back(nullptr);
        envp.push_back(nullptr);

        posix_spawn_file_actions_t files;
        posix_spawnattr_t attr;
        int rc = posix_spawn_file_actions_init(&files);
        if (rc == 0) {
            rc = posix_spawnattr_init(&attr);
            if (rc == 0) {
                // A separate session also prevents a same-uid sibling from joining this hook's group.
                short flags = POSIX_SPAWN_SETSID | POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF;
                sigset_t empty, defaults;
                sigemptyset(&empty);
                sigemptyset(&defaults);
                for (int s : {SIGPIPE, SIGTERM, SIGHUP, SIGINT, SIGCHLD}) sigaddset(&defaults, s);
                rc = posix_spawn_file_actions_addopen(&files, 0, "/dev/null", O_RDONLY, 0);
                if (!rc) rc = posix_spawn_file_actions_adddup2(&files, writer, 1);
                if (!rc) rc = posix_spawn_file_actions_adddup2(&files, writer, 2);
#ifdef __APPLE__
                flags |= POSIX_SPAWN_CLOEXEC_DEFAULT;
#else
                if (!rc) rc = posix_spawn_file_actions_addclosefrom_np(&files, 3);
#endif
                if (!rc) rc = posix_spawnattr_setflags(&attr, flags);
                if (!rc) rc = posix_spawnattr_setsigmask(&attr, &empty);
                if (!rc) rc = posix_spawnattr_setsigdefault(&attr, &defaults);
                pid_t child = -1;
                if (!rc) rc = ::posix_spawn(&child, broker.c_str(), &files, &attr, argv.data(), envp.data());
                if (rc == 0) pid = child;
                posix_spawnattr_destroy(&attr);
            }
            posix_spawn_file_actions_destroy(&files);
        }
        closeFd(writer);
        return rc ? "cannot start the authenticated bus hook: " + std::string(std::strerror(rc)) : "";
    }

    bool receive(const std::string& name, const Accept& accept,
                 const std::function<void(const std::string&)>& report) {
        if (connection < 0) {
            const int fd = ::accept(listenFd, nullptr, nullptr);
            if (fd < 0) return false;
            if (!prepareFd(fd) || !safeWrites(fd) || !ownedHelper(fd, pid)) {
                ::close(fd);
                return false;
            }
            connection = fd;
            deadline = Clock::now() + kDeliveryTimeout;
        }
        if (awaitingFlush) return false;
        if (Clock::now() >= deadline) {
            disconnect();
            return false;
        }
        char bytes[8192];
        // Share a serving pass fairly among large incoming messages and the rest of the server.
        for (int read = 0; !complete && read < 32; ++read) {
            const ssize_t n = ::recv(connection, bytes, sizeof bytes, 0);
            if (n == 0) {
                if (!frame.ended) { disconnect(); return false; }
                complete = true; // the helper half-closed after forwarding one complete message
            } else if (n < 0) {
                if (errno == EINTR) continue;
                if (errno != EAGAIN && errno != EWOULDBLOCK) disconnect();
                return false;
            } else if (!frame.feed({bytes, static_cast<std::size_t>(n)})) {
                if (report && !badFrameReported) {
                    report("message bus for " + name + ": the pending message has an invalid envelope or exceeds the "
                           "32 MiB body or 1 MiB subject limit; correct the message on the bus");
                    badFrameReported = true;
                }
                disconnect();
                return false;
            }
        }
        if (!complete) return false;
        switch (accept(name, frame.msg)) {
        case BusAccept::Admit: awaitingFlush = true; return true;
        case BusAccept::Retry: disconnect(); return false;
        case BusAccept::Wait: return false;
        }
        return false;
    }
};

BusListeners::BusListeners(std::string home, std::string self, std::function<void(const std::string&)> report)
    : home_(std::move(home)), self_(std::move(self)), report_(std::move(report)) {}
BusListeners::~BusListeners() {
    configure({});
    while (!stopping_.empty()) {
        reapStopped();
        if (!stopping_.empty()) ::poll(nullptr, 0, 10);
    }
}

void BusListeners::reapStopped() {
    std::erase_if(stopping_, [](const auto& hook) {
        return !hook->started() || Clock::now() >= hook->deadline;
    });
}

void BusListeners::configure(const std::map<std::string, std::string>& addresses) {
    for (auto it = hooks_.begin(); it != hooks_.end();) {
        const auto wanted = addresses.find(it->first);
        if (wanted != addresses.end() && wanted->second == it->second->address) { ++it; continue; }
        it->second->retire();
        stopping_.push_back(std::move(it->second));
        it = hooks_.erase(it);
    }
    for (const auto& [name, address] : addresses) {
        if (hooks_.contains(name)) continue;
        if (!busName(name) || !loopback(address)) {
            if (report_) report_("invalid message bus grant; use an inbox name and a loopback Redis port");
            continue;
        }
        hooks_.emplace(name, std::make_unique<Hook>(address));
    }
}

std::vector<pollfd> BusListeners::fds() const {
    std::vector<pollfd> result;
    for (const auto& [name, hook] : hooks_) {
        (void)name;
        for (int fd : {hook->listenFd, hook->output, hook->complete ? -1 : hook->connection}) {
            if (fd >= 0) result.push_back({fd, POLLIN, 0});
        }
    }
    return result;
}

int BusListeners::waitMs() const {
    int wait = stopping_.empty() ? INT_MAX : 25;
    const auto now = Clock::now();
    for (const auto& [name, hook] : hooks_) {
        (void)name;
        if (hook->pid > 0) {
            // A child exit or new hook traffic wakes poll. Recheck deadlines even if a pipe stays held.
            wait = std::min(wait, hook->complete ? 25 : 1000);
        } else if (hook->retryAt <= now) {
            return 0;
        } else {
            wait = std::min(wait, static_cast<int>(std::chrono::ceil<std::chrono::milliseconds>(hook->retryAt - now).count()));
        }
    }
    return wait;
}

bool BusListeners::serve(const Accept& accept) {
    reapStopped();
    bool accepted = false;
    for (auto& [name, hook] : hooks_) {
        std::string failure;
        if (hook->pid > 0) {
            hook->drainStatus(name, report_);
            if (!hook->started()) failure = "hook exited; check the Redis endpoint, bus credentials and inbox ownership";
        } else if (Clock::now() >= hook->retryAt) {
            failure = hook->start(home_, self_, name);
        }
        if (!failure.empty()) {
            if (report_ && failure != hook->lastFailure) report_("message bus for " + name + ": " + failure + "; retrying");
            hook->lastFailure = failure;
            hook->stop();
            hook->retryAt = Clock::now() + kRetry;
        } else if (hook->pid > 0 && hook->receive(name, accept, report_)) {
            accepted = true;
        }
    }
    return accepted;
}

void BusListeners::flushed() {
    for (auto& [name, hook] : hooks_) {
        (void)name;
        if (!hook->awaitingFlush) continue;
        // A missing response leaves the delivery pending for replay; the durable record already exists.
        ssize_t written;
        do {
            written = writeSocket(hook->connection, "K", 1);
        } while (written < 0 && errno == EINTR);
        hook->disconnect();
        hook->lastFailure.clear();
        hook->badFrameReported = false;
    }
}

int busDeliver(const std::string& path) {
    sockaddr_un local{};
    if (!socketAddress(path, &local)) return 1;
    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return 1;
    // nova-bus also enforces its --exec deadline from outside this process.
    timeval timeout{8, 0};
    bool ok = safeWrites(fd) && ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout) == 0 &&
        ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof timeout) == 0 &&
        ::connect(fd, reinterpret_cast<const sockaddr*>(&local), sizeof local) == 0;
    std::size_t total = 0;
    char bytes[8192];
    while (ok) {
        const ssize_t n = ::read(STDIN_FILENO, bytes, sizeof bytes);
        if (n < 0 && errno == EINTR) continue;
        if (n == 0) break;
        if (n < 0 || static_cast<std::size_t>(n) > kMaxFrame - total) { ok = false; break; }
        total += static_cast<std::size_t>(n);
        std::size_t sent = 0;
        while (sent < static_cast<std::size_t>(n)) {
            const ssize_t out = writeSocket(fd, bytes + sent, static_cast<std::size_t>(n) - sent);
            if (out < 0 && errno == EINTR) continue;
            if (out <= 0) { ok = false; break; }
            sent += static_cast<std::size_t>(out);
        }
    }
    char receipt = 0;
    if (ok && ::shutdown(fd, SHUT_WR) == 0) {
        ssize_t n;
        do { n = ::recv(fd, &receipt, 1, 0); } while (n < 0 && errno == EINTR);
        ok = n == 1 && receipt == 'K';
    } else ok = false;
    closeFd(fd);
    return ok ? 0 : 1;
}

} // namespace nexus::server
