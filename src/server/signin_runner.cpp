#include "server/signin_runner.h"

#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <spawn.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <random>

namespace nexus::server {

namespace {

// How long a process ended by stop() has between SIGTERM and SIGKILL, a helper to say its line, and a replay to be
// answered. Guesses, not measurements: each is many times what the step takes on loopback.
constexpr std::uint64_t kKillGraceMs = 2000;
constexpr std::uint64_t kHelperMs = 2000;
constexpr std::uint64_t kReplayMs = 5000;
// The program's terminal: wide enough that no line it prints (a URL is about 600 bytes) is wrapped.
constexpr unsigned short kColumns = 4000;
constexpr unsigned short kRows = 50;
// The names the helper answers to on the program's PATH.
const char* const kBrowserNames[] = {"open", "xdg-open", "wslview", "sensible-browser", "www-browser", "x-www-browser"};

std::string randomWord() {
    std::random_device rd;
    static const char kDigits[] = "abcdefghijklmnopqrstuvwxyz0123456789";
    std::string out;
    for (int i = 0; i < 32; ++i) {
        out.push_back(kDigits[rd() % (sizeof kDigits - 1)]);
    }
    return out;
}

bool nonblocking(int fd) {
    const int flags = fcntl(fd, F_GETFL, 0);
    return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0 && fcntl(fd, F_SETFD, FD_CLOEXEC) == 0;
}

// The first `claude` on `path`, or empty.
std::string onPath(const std::string& path) {
    std::size_t at = 0;
    while (at <= path.size()) {
        std::size_t end = path.find(':', at);
        if (end == std::string::npos) {
            end = path.size();
        }
        const std::string dir = path.substr(at, end - at);
        if (!dir.empty()) {
            const std::string candidate = dir + "/claude";
            if (::access(candidate.c_str(), X_OK) == 0) {
                return candidate;
            }
        }
        at = end + 1;
    }
    return std::string();
}

} // namespace

SignInRunner::SignInRunner(Options options, Events events) : options_(std::move(options)) {
    socket_ = options_.dir + "/signin.sock";
    bin_ = options_.dir + "/signin-bin";
    if (events.url) {
        events_.url = [this, f = std::move(events.url)](const std::string& a, std::uint64_t id,
                                                        const sync::SecretValue& u) { acted_ = true; f(a, id, u); };
    }
    if (events.printed) {
        events_.printed = [this, f = std::move(events.printed)](const std::string& a, const sync::SecretValue& t) {
            acted_ = true;
            f(a, t);
        };
    }
    if (events.exited) {
        events_.exited = [this, f = std::move(events.exited)](const std::string& a, std::uint64_t id,
                                                              const std::string& why) { acted_ = true; f(a, id, why); };
    }
    events_.log = std::move(events.log);
}

SignInRunner::~SignInRunner() {
    for (auto& [account, p] : procs_) {
        if (p.pid > 0) {
            ::kill(p.pid, SIGKILL);
            int status = 0;
            ::waitpid(p.pid, &status, 0);
        }
        if (p.master >= 0) {
            ::close(p.master);
        }
        if (p.lock >= 0) {
            ::close(p.lock);
        }
        std::error_code ec;
        std::filesystem::remove_all(p.configDir, ec);
    }
    for (Helper& h : helpers_) {
        ::close(h.fd);
        sync::wipeBytes(h.bytes);
    }
    for (Replay& r : replays_) {
        ::close(r.fd);
        sync::wipeBytes(r.out);
    }
    if (listen_ >= 0) {
        ::close(listen_);
        ::unlink(socket_.c_str());
    }
}

std::uint64_t SignInRunner::now() const {
    return options_.now != nullptr ? options_.now() : 0;
}

std::vector<std::string> SignInRunner::sweep() {
    std::vector<std::string> lines;
    std::error_code ec;
    std::filesystem::directory_iterator it(options_.accounts, ec);
    if (ec) {
        return lines;
    }
    for (const std::filesystem::directory_entry& entry : it) {
        const std::string account = entry.path().filename().string();
        const std::string configDir = entry.path().string() + "/.signin";
        struct stat st{};
        if (::lstat(configDir.c_str(), &st) != 0) {
            continue;
        }
        if (!S_ISDIR(st.st_mode)) {
            continue;       // not one the runner made: left alone
        }
        const char* what = "";
        const int lock = ::open((configDir + "/lock").c_str(), O_RDWR | O_CLOEXEC | O_NOFOLLOW);
        if (lock >= 0 && ::flock(lock, LOCK_EX | LOCK_NB) != 0 && errno == EWOULDBLOCK) {
            // Held: a process this server started still runs. Its group is in .signin/pid.
            pid_t group = -1;
            const int fd = ::open((configDir + "/pid").c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
            if (fd >= 0) {
                char buf[32] = {};
                const ssize_t n = ::read(fd, buf, sizeof buf - 1);
                ::close(fd);
                if (n > 0) {
                    char* end = nullptr;
                    const long v = std::strtol(buf, &end, 10);
                    if (end != buf && v > 1) {
                        group = static_cast<pid_t>(v);
                    }
                }
            }
            if (group > 1) {
                ::kill(-group, SIGTERM);
                // Ended once the lock comes free: every holder has gone. A grace, then SIGKILL.
                const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(kKillGraceMs);
                while (::flock(lock, LOCK_EX | LOCK_NB) != 0 && std::chrono::steady_clock::now() < until) {
                    ::usleep(20000);
                }
                if (::flock(lock, LOCK_EX | LOCK_NB) != 0) {
                    ::kill(-group, SIGKILL);
                }
                what = "its process ended, ";
            } else {
                what = "its process still runs with no group recorded and was left, ";
            }
        }
        if (lock >= 0) {
            ::close(lock);
        }
        std::filesystem::remove_all(configDir, ec);
        lines.push_back("sign-in: " + account + "'s sign-in left by an earlier server: " + what +
                        "its directory removed");
    }
    return lines;
}

bool SignInRunner::open(std::string* why) {
    for (const std::string& line : sweep()) {
        if (events_.log) {
            events_.log(line);
        }
    }
    // The bin directory: the helper under each name a program opens a browser by.
    std::error_code ec;
    std::filesystem::remove_all(bin_, ec);
    if (::mkdir(bin_.c_str(), 0700) != 0) {
        *why = "cannot make " + bin_ + ": " + std::strerror(errno);
        return false;
    }
    for (const char* name : kBrowserNames) {
        const std::string link = bin_ + "/" + name;
        if (::symlink(options_.helper.c_str(), link.c_str()) != 0) {
            *why = "cannot link " + link + ": " + std::strerror(errno);
            return false;
        }
    }
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (socket_.size() >= sizeof addr.sun_path) {
        *why = socket_ + " is too long for a Unix socket's address";
        return false;
    }
    std::memcpy(addr.sun_path, socket_.c_str(), socket_.size() + 1);
    ::unlink(socket_.c_str());
    listen_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (listen_ < 0 || ::bind(listen_, reinterpret_cast<const sockaddr*>(&addr), sizeof addr) != 0 ||
            ::chmod(socket_.c_str(), 0600) != 0 || ::listen(listen_, 8) != 0 || !nonblocking(listen_)) {
        *why = "cannot listen on " + socket_ + ": " + std::strerror(errno);
        return false;
    }
    return true;
}

void SignInRunner::start(std::uint64_t id, const std::string& account) {
    if (procs_.count(account) != 0) {
        stop(account);      // the desk starts one per account; one left over is ended first
        reap(account, procs_[account], true);
    }
    auto fail = [&](const std::string& what) {
        if (events_.exited) {
            events_.exited(account, id, "its process did not start: " + what);
        }
    };
    const std::string claude = !options_.claude.empty() ? options_.claude : onPath(options_.path);
    if (claude.empty()) {
        fail("no claude program on the server's PATH (set `claude <path>` in the server's config)");
        return;
    }
    // Its own config directory, under the account's, so nothing of another login is read or written.
    const std::string accountDir = options_.accounts + "/" + account;
    const std::string configDir = accountDir + "/.signin";
    std::error_code ec;
    std::filesystem::create_directories(accountDir, ec);
    ::chmod(accountDir.c_str(), 0700);
    std::filesystem::remove_all(configDir, ec);
    if (::mkdir(configDir.c_str(), 0700) != 0) {
        fail("cannot make its config directory: " + std::string(std::strerror(errno)));
        return;
    }
    // The lock that marks the process as this server's (sweep()): locked here, close-on-exec so that no other child of
    // the server inherits it, and given to the process alone as its fd 3.
    // Kept above fd 9, so that the dup2 onto fd 3 is never of a descriptor onto itself (which would leave it
    // close-on-exec) and never lands on one of the terminal's.
    int lock = ::open((configDir + "/lock").c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (lock >= 0 && lock < 10) {
        const int high = ::fcntl(lock, F_DUPFD_CLOEXEC, 10);
        ::close(lock);
        lock = high;
    }
    if (lock < 0 || ::flock(lock, LOCK_EX | LOCK_NB) != 0) {
        if (lock >= 0) {
            ::close(lock);
        }
        std::filesystem::remove_all(configDir, ec);
        fail("cannot lock its config directory: " + std::string(std::strerror(errno)));
        return;
    }
    const int master = ::posix_openpt(O_RDWR | O_NOCTTY);
    if (master < 0 || ::grantpt(master) != 0 || ::unlockpt(master) != 0) {
        if (master >= 0) {
            ::close(master);
        }
        ::close(lock);
        std::filesystem::remove_all(configDir, ec);
        fail("no pseudo-terminal: " + std::string(std::strerror(errno)));
        return;
    }
    const char* slaveName = ::ptsname(master);
    const int slave = slaveName == nullptr ? -1 : ::open(slaveName, O_RDWR | O_NOCTTY);
    if (slave < 0) {
        ::close(master);
        ::close(lock);
        std::filesystem::remove_all(configDir, ec);
        fail("its pseudo-terminal did not open");
        return;
    }
    winsize size{};
    size.ws_col = kColumns;
    size.ws_row = kRows;
    ::ioctl(slave, TIOCSWINSZ, &size);
    const std::string nonce = randomWord();
    // The environment, from nothing.
    std::vector<std::string> env{
        "HOME=" + options_.home,
        "PATH=" + bin_ + ":" + std::filesystem::path(claude).parent_path().string() + ":" + options_.path,
        "TERM=xterm-256color",
        "LANG=en_US.UTF-8",
        "CLAUDE_CONFIG_DIR=" + configDir,
        "BROWSER=" + bin_ + "/open",
        "NEXUS_SIGNIN_SOCKET=" + socket_,
        "NEXUS_SIGNIN_FLOW=" + std::to_string(id) + "-" + nonce,
    };
    std::vector<char*> envp;
    for (std::string& e : env) {
        envp.push_back(e.data());
    }
    envp.push_back(nullptr);
    std::string program = claude;
    std::string verb = "setup-token";
    char* argv[] = {program.data(), verb.data(), nullptr};
    posix_spawn_file_actions_t files;
    posix_spawn_file_actions_init(&files);
    posix_spawn_file_actions_adddup2(&files, slave, 0);
    posix_spawn_file_actions_adddup2(&files, slave, 1);
    posix_spawn_file_actions_adddup2(&files, slave, 2);
    posix_spawn_file_actions_adddup2(&files, lock, 3);
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    short flags = POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF;
#ifdef POSIX_SPAWN_SETSID
    flags |= POSIX_SPAWN_SETSID;
#else
    flags |= POSIX_SPAWN_SETPGROUP;     // its own group, numbered by its pid
    posix_spawnattr_setpgroup(&attr, 0);
#endif
    posix_spawnattr_setflags(&attr, flags);
    sigset_t none;
    sigemptyset(&none);
    posix_spawnattr_setsigmask(&attr, &none);
    sigset_t all;
    sigfillset(&all);
    sigdelset(&all, SIGKILL);
    sigdelset(&all, SIGSTOP);
    posix_spawnattr_setsigdefault(&attr, &all);
    pid_t pid = -1;
    const int rc = ::posix_spawn(&pid, program.c_str(), &files, &attr, argv, envp.data());
    posix_spawn_file_actions_destroy(&files);
    posix_spawnattr_destroy(&attr);
    ::close(slave);
    for (std::string& e : env) {
        sync::wipeBytes(e);
    }
    if (rc != 0) {
        ::close(master);
        ::close(lock);
        std::filesystem::remove_all(configDir, ec);
        fail(std::string(std::strerror(rc)));
        return;
    }
    // Its process group (POSIX_SPAWN_SETSID: its own pid), for sweep() after a crash of this server.
    {
        const std::string pidFile = configDir + "/pid";
        const int fd = ::open(pidFile.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (fd >= 0) {
            const std::string text = std::to_string(pid) + "\n";
            const ssize_t n = ::write(fd, text.data(), text.size());
            (void)n;
            ::close(fd);
        }
    }
    nonblocking(master);
    auto [it, made] = procs_.try_emplace(account);
    (void)made;
    Proc& p = it->second;
    p.id = id;
    p.pid = pid;
    p.master = master;
    p.nonce = nonce;
    p.configDir = configDir;
    p.lock = lock;
    if (events_.log) {
        events_.log("sign-in " + std::to_string(id) + ": " + account + ": claude setup-token started, process " +
                    std::to_string(pid));
    }
}

void SignInRunner::stop(const std::string& account) {
    auto it = procs_.find(account);
    if (it == procs_.end() || it->second.pid <= 0 || it->second.killAt != 0) {
        return;
    }
    ::kill(it->second.pid, SIGTERM);
    it->second.killAt = now() + kKillGraceMs;
}

void SignInRunner::replay(std::uint64_t id, const std::string& account, const sync::SignInTarget& target,
                          const sync::SecretValue& query) {
    const std::uint16_t port = options_.replayPort != 0 ? options_.replayPort : target.port;
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (fd < 0 || !nonblocking(fd) ||
            (::connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof addr) != 0 && errno != EINPROGRESS)) {
        if (fd >= 0) {
            ::close(fd);
        }
        if (events_.log) {
            events_.log("sign-in " + std::to_string(id) + ": " + account + ": the callback was not replayed: " +
                        std::strerror(errno));
        }
        return;
    }
    Replay r;
    r.fd = fd;
    r.out = "GET " + target.path + "?" + query.bytes() + " HTTP/1.1\r\nHost: localhost:" + std::to_string(target.port) +
        "\r\nConnection: close\r\n\r\n";
    r.until = now() + kReplayMs;
    replays_.push_back(std::move(r));
}

std::vector<pollfd> SignInRunner::fds() const {
    std::vector<pollfd> out;
    if (listen_ >= 0) {
        out.push_back(pollfd{listen_, POLLIN, 0});
    }
    for (const auto& [account, p] : procs_) {
        if (p.master >= 0) {
            out.push_back(pollfd{p.master, POLLIN, 0});
        }
    }
    for (const Helper& h : helpers_) {
        out.push_back(pollfd{h.fd, POLLIN, 0});
    }
    for (const Replay& r : replays_) {
        out.push_back(pollfd{r.fd, static_cast<short>(r.out.empty() ? POLLIN : POLLOUT), 0});
    }
    return out;
}

int SignInRunner::waitMs() const {
    const bool ending = std::any_of(procs_.begin(), procs_.end(), [](const auto& e) { return e.second.killAt != 0; });
    return ending || !replays_.empty() || !helpers_.empty() ? 100 : -1;
}

void SignInRunner::readTerminal(const std::string& account, Proc& p) {
    char buf[4096];
    for (;;) {
        const ssize_t n = ::read(p.master, buf, sizeof buf);
        if (n > 0) {
            const std::string_view bytes(buf, static_cast<std::size_t>(n));
            if (!p.urlSent && p.url.feed(bytes)) {
                p.urlSent = true;
                const sync::SecretValue url = p.url.take();
                if (events_.url) {
                    events_.url(account, p.id, url);
                }
            }
            if (!p.printed && p.token.feed(bytes)) {
                p.printed = true;
                const sync::SecretValue token = p.token.take();
                std::memset(buf, 0, sizeof buf);
                if (events_.printed) {
                    events_.printed(account, token);
                }
                return;     // the hub may have stopped it; the next serve reads on
            }
            std::memset(buf, 0, sizeof buf);
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EINTR)) {
            return;
        }
        // The end of its terminal (EIO on Linux once the program has exited): collect it.
        ::close(p.master);
        p.master = -1;
        return;
    }
}

// The process has ended, or is ended now: collected, its config directory removed, and its end reported unless the
// flow ended first (`reported`).
void SignInRunner::reap(const std::string& account, Proc& p, bool reported) {
    int status = 0;
    if (p.pid > 0) {
        if (::waitpid(p.pid, &status, WNOHANG) == 0) {
            ::kill(p.pid, SIGKILL);
            ::waitpid(p.pid, &status, 0);
        }
    }
    if (p.master >= 0) {
        ::close(p.master);
    }
    if (p.lock >= 0) {
        ::close(p.lock);
        p.lock = -1;
    }
    std::error_code ec;
    std::filesystem::remove_all(p.configDir, ec);
    const std::uint64_t id = p.id;
    std::string why = WIFEXITED(status) ? "it exited " + std::to_string(WEXITSTATUS(status)) :
        WIFSIGNALED(status) ? "it ended on signal " + std::to_string(WTERMSIG(status)) : "it ended";
    const bool tell = !reported && !p.printed;
    procs_.erase(account);
    if (tell && events_.exited) {
        events_.exited(account, id, why);
    }
}

void SignInRunner::acceptHelper() {
    for (;;) {
        const int fd = ::accept(listen_, nullptr, nullptr);
        if (fd < 0) {
            return;
        }
        nonblocking(fd);
        helpers_.push_back(Helper{fd, std::string(), now() + kHelperMs});
    }
}

// One line from the helper: "<flow>-<word> <url>". Taken for the process it names, once; any other is dropped.
void SignInRunner::readHelper(Helper& h) {
    char buf[1024];
    for (;;) {
        const ssize_t n = ::read(h.fd, buf, sizeof buf);
        if (n > 0) {
            h.bytes.append(buf, static_cast<std::size_t>(n));
            std::memset(buf, 0, sizeof buf);
            if (h.bytes.size() > sync::kSignInUrlBytes + 128) {
                break;
            }
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EINTR)) {
            return;
        }
        break;
    }
    const std::size_t end = h.bytes.find('\n');
    const std::size_t space = h.bytes.find(' ');
    if (end != std::string::npos && space != std::string::npos && space < end) {
        const std::string word = h.bytes.substr(0, space);
        for (auto& [account, p] : procs_) {
            if (!p.urlSent && sync::sameBytes(word, std::to_string(p.id) + "-" + p.nonce)) {
                p.urlSent = true;
                const sync::SecretValue url(std::string_view(h.bytes).substr(space + 1, end - space - 1));
                if (events_.url) {
                    events_.url(account, p.id, url);
                }
                break;
            }
        }
    }
    sync::wipeBytes(h.bytes);
    ::close(h.fd);
    h.fd = -1;
}

bool SignInRunner::serve() {
    acted_ = false;
    if (listen_ >= 0) {
        acceptHelper();
    }
    for (Helper& h : helpers_) {
        readHelper(h);
        if (h.fd >= 0 && now() >= h.until) {
            sync::wipeBytes(h.bytes);
            ::close(h.fd);
            h.fd = -1;
        }
    }
    helpers_.erase(std::remove_if(helpers_.begin(), helpers_.end(), [](const Helper& h) { return h.fd < 0; }),
                   helpers_.end());
    std::vector<std::string> accounts;
    for (const auto& [account, p] : procs_) {
        accounts.push_back(account);
    }
    for (const std::string& account : accounts) {
        auto it = procs_.find(account);
        if (it == procs_.end()) {
            continue;
        }
        Proc& p = it->second;
        if (p.master >= 0) {
            readTerminal(account, p);
        }
        it = procs_.find(account);
        if (it == procs_.end()) {
            continue;
        }
        Proc& q = it->second;
        int status = 0;
        const bool ended = q.master < 0 || (q.pid > 0 && ::waitpid(q.pid, &status, WNOHANG) == q.pid);
        if (ended) {
            if (q.master < 0 || q.pid <= 0) {
                reap(account, q, q.killAt != 0);
            } else {
                // Collected above: its status is this one.
                q.pid = -1;
                ::close(q.master);
                q.master = -1;
                if (q.lock >= 0) {
                    ::close(q.lock);
                    q.lock = -1;
                }
                std::error_code ec;
                std::filesystem::remove_all(q.configDir, ec);
                const std::uint64_t id = q.id;
                const bool tell = q.killAt == 0 && !q.printed;
                procs_.erase(account);
                if (tell && events_.exited) {
                    events_.exited(account, id, WIFEXITED(status) ?
                        "it exited " + std::to_string(WEXITSTATUS(status)) : "it ended on a signal");
                }
            }
            continue;
        }
        if (q.killAt != 0 && now() >= q.killAt) {
            ::kill(q.pid, SIGKILL);
            reap(account, q, true);
        }
    }
    for (Replay& r : replays_) {
        if (!r.out.empty()) {
            const ssize_t n = ::send(r.fd, r.out.data(), r.out.size(), 0);
            if (n > 0) {
                std::fill_n(r.out.begin(), static_cast<std::size_t>(n), '\0');
                r.out.erase(0, static_cast<std::size_t>(n));
            } else if (n < 0 && errno != EAGAIN && errno != EINTR && errno != ENOTCONN) {
                sync::wipeBytes(r.out);
                r.until = 0;
            }
        } else {
            // The listener's answer: read and dropped, until it closes.
            char buf[1024];
            const ssize_t n = ::recv(r.fd, buf, sizeof buf, 0);
            if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR)) {
                r.until = 0;
            }
        }
        if (now() >= r.until) {
            sync::wipeBytes(r.out);
            ::close(r.fd);
            r.fd = -1;
        }
    }
    replays_.erase(std::remove_if(replays_.begin(), replays_.end(), [](const Replay& r) { return r.fd < 0; }),
                   replays_.end());
    return acted_;
}

} // namespace nexus::server
