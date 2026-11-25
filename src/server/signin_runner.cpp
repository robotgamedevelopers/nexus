#include "server/signin_runner.h"

#include "server/login_life.h"

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

int aboveNine(int fd) {
    if (fd >= 0 && fd < 10) {
        const int high = ::fcntl(fd, F_DUPFD_CLOEXEC, 10);
        ::close(fd);
        return high;
    }
    if (fd >= 0 && ::fcntl(fd, F_SETFD, FD_CLOEXEC) != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

} // namespace

// The first absolute `claude` on `path`, or empty.
std::string claudeOnPath(const std::string& path) {
    std::size_t at = 0;
    while (at <= path.size()) {
        std::size_t end = path.find(':', at);
        if (end == std::string::npos) {
            end = path.size();
        }
        const std::string dir = path.substr(at, end - at);
        if (!dir.empty() && dir.front() == '/') {
            const std::string candidate = dir + "/claude";
            if (::access(candidate.c_str(), X_OK) == 0) {
                return candidate;
            }
        }
        at = end + 1;
    }
    return std::string();
}

SignInRunner::SignInRunner(Options options, Events events) : options_(std::move(options)) {
    socket_ = options_.dir + "/signin.sock";
    bin_ = options_.dir + "/signin-bin";
    if (events.url) {
        events_.url = [this, f = std::move(events.url)](const std::string& a, std::uint64_t id,
                                                        const sync::SecretValue& u) { acted_ = true; f(a, id, u); };
    }
    if (events.codeWanted) {
        events_.codeWanted = [this, f = std::move(events.codeWanted)](const std::string& a, std::uint64_t id) {
            acted_ = true;
            f(a, id);
        };
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
    events_.enrolled = std::move(events.enrolled);
    events_.log = std::move(events.log);
}

SignInRunner::~SignInRunner() {
    for (auto& [account, p] : procs_) {
        if (p.pid > 0) {
            ::kill(-p.pid, SIGKILL);    // its group: the program and anything it started
            int status = 0;
            ::waitpid(p.pid, &status, 0);
        }
        if (p.master >= 0) {
            ::close(p.master);
        }
        if (p.lock >= 0) {
            ::close(p.lock);
        }
        sync::wipeBytes(p.codeOut);
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
        struct stat st{};
        // The account's entry itself first: a link, or anything else that is not a directory, is not an account's
        // directory the runner made, and nothing under it is removed.
        if (::lstat(entry.path().c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) {
            continue;
        }
        const std::string configDir = entry.path().string() + "/.signin";
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

void SignInRunner::start(std::uint64_t id, const std::string& account, const std::string& provider,
                         std::vector<std::string> command) {
    if (procs_.count(account) != 0) {
        stop(account);      // the desk starts one per account; one left over is ended first
        reap(account, procs_[account], true, std::nullopt);
    }
    auto fail = [&](const std::string& what) {
        if (events_.exited) {
            events_.exited(account, id, "its process did not start: " + what);
        }
    };
    const bool setupToken = provider == "claude" && command.empty();
    if (setupToken) {
        const std::string claude = !options_.claude.empty() ? options_.claude : claudeOnPath(options_.path);
        if (claude.empty()) {
            fail("no claude program on the server's PATH (set `claude <path>` in the server's config)");
            return;
        }
        command = {claude, "setup-token"};
    } else if (command.empty() || command.front().empty() || command.front().front() != '/') {
        fail("set a signin command with an absolute program path for " + provider + " in the server's drivers file");
        return;
    }
    const std::string& program = command.front();
    // Its own config directory, under the account's, so nothing of another login is read or written.
    const std::string accountDir = options_.accounts + "/" + account;
    const std::string configDir = accountDir + "/.signin";
    std::error_code ec;
    std::filesystem::create_directories(accountDir, ec);
    std::string problem;
    const int accountFd = openAccountDirectory(accountDir, problem);
    if (accountFd < 0) {
        fail(problem);
        return;
    }
    const bool privateAccount = ::fchmod(accountFd, 0700) == 0;
    ::close(accountFd);
    if (!privateAccount) {
        fail("cannot make its account directory private");
        return;
    }
    std::filesystem::remove_all(configDir, ec);
    if (::mkdir(configDir.c_str(), 0700) != 0) {
        fail("cannot make its config directory: " + std::string(std::strerror(errno)));
        return;
    }
    // The lock that marks the process as this server's (sweep()): locked here, close-on-exec so that no other child of
    // the server inherits it, and given to the process alone as its fd 3.
    // Kept above fd 9, so that the dup2 onto fd 3 is never of a descriptor onto itself (which would leave it
    // close-on-exec) and never lands on one of the terminal's.
    int lock = aboveNine(::open((configDir + "/lock").c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600));
    if (lock < 0 || ::flock(lock, LOCK_EX | LOCK_NB) != 0) {
        if (lock >= 0) {
            ::close(lock);
        }
        std::filesystem::remove_all(configDir, ec);
        fail("cannot lock its config directory: " + std::string(std::strerror(errno)));
        return;
    }
    int master = ::posix_openpt(O_RDWR | O_NOCTTY);
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
    const int slave = aboveNine(slaveName == nullptr ? -1 : ::open(slaveName, O_RDWR | O_NOCTTY | O_CLOEXEC));
    master = aboveNine(master);
    if (slave < 0 || master < 0 || !nonblocking(master)) {
        if (slave >= 0) {
            ::close(slave);
        }
        if (master >= 0) {
            ::close(master);
        }
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
        "PATH=" + bin_ + ":" + std::filesystem::path(program).parent_path().string() + ":" + options_.path,
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
    std::vector<char*> argv;
    for (std::string& arg : command) {
        argv.push_back(arg.data());
    }
    argv.push_back(nullptr);
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
    // No other file of this process: on macOS by POSIX_SPAWN_CLOEXEC_DEFAULT, on Linux by glibc's addclosefrom_np.
#if defined(__APPLE__)
    flags |= POSIX_SPAWN_CLOEXEC_DEFAULT;
#else
    posix_spawn_file_actions_addclosefrom_np(&files, 4);
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
    const int rc = ::posix_spawn(&pid, program.c_str(), &files, &attr, argv.data(), envp.data());
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
    auto [it, made] = procs_.try_emplace(account);
    (void)made;
    Proc& p = it->second;
    p.id = id;
    p.provider = provider;
    p.setupToken = setupToken;
    p.pid = pid;
    p.master = master;
    p.nonce = nonce;
    p.configDir = configDir;
    p.lock = lock;
    if (events_.log) {
        const std::string operation = setupToken ? "claude setup-token" : provider + " sign-in";
        events_.log("sign-in " + std::to_string(id) + ": " + account + ": " + operation + " started, process " +
                    std::to_string(pid));
    }
}

void SignInRunner::stop(const std::string& account) {
    auto it = procs_.find(account);
    if (it == procs_.end() || it->second.pid <= 0 || it->second.killAt != 0) {
        return;
    }
    ::kill(-it->second.pid, SIGTERM);   // its group (POSIX_SPAWN_SETSID: numbered by its pid)
    it->second.killAt = now() + kKillGraceMs;
}

// A connection to the replay's next address family, on loopback and the flow's own port alone; false when none is left
// (errno says why the last failed).
bool SignInRunner::connectReplay(Replay& r) {
    while (!r.families.empty()) {
        const int family = r.families.front();
        r.families.erase(r.families.begin());
        const int fd = ::socket(family, SOCK_STREAM, 0);
        if (fd < 0 || !nonblocking(fd)) {
            if (fd >= 0) {
                ::close(fd);
            }
            continue;
        }
#if defined(SO_NOSIGPIPE)
        // A send on a connection refused is an error, never a SIGPIPE (Linux: MSG_NOSIGNAL on the send).
        int on = 1;
        ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof on);
#endif
        int rc = -1;
        if (family == AF_INET6) {
            sockaddr_in6 addr{};
            addr.sin6_family = AF_INET6;
            addr.sin6_port = htons(r.port);
            addr.sin6_addr = in6addr_loopback;
            rc = ::connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof addr);
        } else {
            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_port = htons(r.port);
            addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            rc = ::connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof addr);
        }
        if (rc == 0 || errno == EINPROGRESS) {
            r.fd = fd;
            return true;
        }
        const int saved = errno;
        ::close(fd);
        errno = saved;
    }
    return false;
}

void SignInRunner::replay(std::uint64_t id, const std::string& account, const sync::SignInTarget& target,
                          const sync::SecretValue& query) {
    Replay r;
    r.port = options_.replayPort != 0 ? options_.replayPort : target.port;
    // The redirect's own host: localhost on IPv6 loopback first, then IPv4, as browsers try it; 127.0.0.1 on IPv4
    // alone. Loopback only, and the flow's own port only.
    r.families = target.host == "127.0.0.1" ? std::vector<int>{AF_INET} : std::vector<int>{AF_INET6, AF_INET};
    r.id = id;
    r.account = account;
    if (!connectReplay(r)) {
        if (events_.log) {
            events_.log("sign-in " + std::to_string(id) + ": " + account + ": the callback was not replayed: " +
                        std::strerror(errno));
        }
        return;
    }
    r.out = "GET " + target.path + "?" + query.bytes() + " HTTP/1.1\r\nHost: " +
        (target.host.empty() ? std::string("localhost") : target.host) + ":" + std::to_string(target.port) +
        "\r\nConnection: close\r\n\r\n";
    r.until = now() + kReplayMs;
    replays_.push_back(std::move(r));
}

void SignInRunner::code(std::uint64_t id, const std::string& account, const sync::SecretValue& code) {
    const auto it = procs_.find(account);
    if (it == procs_.end() || it->second.id != id || it->second.setupToken ||
            !sync::signsInByPastedCode(it->second.provider) || it->second.master < 0 || it->second.killAt != 0 ||
            !it->second.codeAsked || it->second.codeTaken || !sync::isSignInCode(code)) {
        if (events_.log) {
            events_.log("sign-in " + std::to_string(id) + ": " + account + ": a pasted code, dropped: its sign-in "
                        "process is not waiting for one");
        }
        return;
    }
    Proc& p = it->second;
    p.codeTaken = true;
    p.codeOut = code.bytes() + "\n";
    writeCode(p);
    if (events_.log) {
        events_.log("sign-in " + std::to_string(id) + ": " + account + ": code handed to its sign-in process");
    }
}

// What of the code's line is left goes to the helper's terminal, without waiting; the rest on a later serve().
void SignInRunner::writeCode(Proc& p) {
    while (!p.codeOut.empty() && p.master >= 0) {
        const ssize_t n = ::write(p.master, p.codeOut.data(), p.codeOut.size());
        if (n > 0) {
            std::fill_n(p.codeOut.begin(), static_cast<std::size_t>(n), '\0');
            p.codeOut.erase(0, static_cast<std::size_t>(n));
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n < 0 && errno == EAGAIN) {
            return;
        }
        break;      // its terminal is gone: the process ends, and its end says how the sign-in went
    }
    sync::wipeBytes(p.codeOut);
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
    const bool ending = std::any_of(procs_.begin(), procs_.end(), [](const auto& e) {
        return e.second.killAt != 0 || !e.second.codeOut.empty();
    });
    return ending || !replays_.empty() || !helpers_.empty() ? 100 : -1;
}

void SignInRunner::readTerminal(const std::string& account, Proc& p) {
    char buf[4096];
    for (;;) {
        const ssize_t n = ::read(p.master, buf, sizeof buf);
        if (n > 0) {
            const std::string_view bytes(buf, static_cast<std::size_t>(n));
            if (p.provider == "claude" && !p.urlSent && p.url.feed(bytes)) {
                p.urlSent = true;
                const sync::SecretValue url = p.url.take();
                if (events_.url) {
                    events_.url(account, p.id, url);
                }
            }
            if (p.setupToken) {
                p.token.feed(bytes);
            }
            std::memset(buf, 0, sizeof buf);
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n < 0 && errno == EAGAIN) {
            return;
        }
        // The end of its terminal (EIO on Linux once the program has exited, 0 elsewhere): everything it wrote is read.
        ::close(p.master);
        p.master = -1;
        return;
    }
}

// The process has ended (`status`, collected already) or is ended now. Before anything is closed, its terminal is read
// to the end, so that what the program wrote before it ended goes through the scanners; then its group is ended, so
// that nothing it started outlives it; then its directory is removed. The login is the last one it printed, handed
// over when it exited 0, or when the runner ended it (the flow ended first: a login printed meanwhile is stored late);
// otherwise its end is reported, unless the flow ended first (`reported`).
void SignInRunner::reap(const std::string& account, Proc& p, bool reported, std::optional<int> status) {
    if (p.master >= 0) {
        readTerminal(account, p);
    }
    int st = status.value_or(0);
    if (p.pid > 0) {
        // The group first, while the program's pid (its group's number) is still its own: collected, it could be
        // another's. The status collected is the program's own if it had ended by then.
        ::kill(-p.pid, SIGKILL);
        ::waitpid(p.pid, &st, 0);
        status = st;
        p.pid = -1;
    }
    if (p.master >= 0) {
        readTerminal(account, p);       // what it wrote as it ended
        if (p.master >= 0) {
            ::close(p.master);
            p.master = -1;
        }
    }
    if (p.lock >= 0) {
        ::close(p.lock);
        p.lock = -1;
    }
    sync::wipeBytes(p.codeOut);
    std::error_code ec;
    std::filesystem::remove_all(p.configDir, ec);
    const std::uint64_t id = p.id;
    const bool exitedZero = status.has_value() && WIFEXITED(st) && WEXITSTATUS(st) == 0;
    const bool endedByUs = p.killAt != 0;
    const std::string why = !status ? "it was ended" : WIFEXITED(st) ? "it exited " + std::to_string(WEXITSTATUS(st)) :
        WIFSIGNALED(st) ? "it ended on a signal" : "it ended";
    if (!p.setupToken) {
        const std::string provider = p.provider;
        procs_.erase(account);
        if (exitedZero && !endedByUs && !reported && events_.enrolled) {
            acted_ = true;
            events_.enrolled(account, provider, id);
        } else if (!reported && events_.exited) {
            std::string reason = why;
            if (provider == "claude" && status && WIFEXITED(st)) {
                switch (WEXITSTATUS(st)) {
                    case 11: reason = "the signed-in email differs from this account; choose the displayed email in the browser and try again"; break;
                    case 12: reason = "Claude did not confirm the email for this login; sign in again"; break;
                    case 13: reason = "Claude sign-in or private credential storage failed; sign in again"; break;
                }
            }
            events_.exited(account, id, reason);
        }
        return;
    }
    const bool found = p.token.finish();
    if (p.token.passedOver() != 0 && events_.log) {
        events_.log("sign-in " + std::to_string(id) + ": " + account + ": a login-shaped run of " +
                    std::to_string(p.token.passedOver()) + " bytes was passed over (a login is " +
                    std::to_string(sync::kSubscriptionTokenBytes) + ")");
    }
    sync::SecretValue token;
    if (found && (exitedZero || endedByUs)) {
        token = p.token.take();
    }
    procs_.erase(account);
    if (!token.empty()) {
        if (events_.printed) {
            events_.printed(account, token);
        }
        return;
    }
    if (!reported && events_.exited) {
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

// One line from the helper: "<flow>-<word> <url>", taken for the process it names, once; then, from a helper of a
// provider that signs in by a pasted code, "<flow>-<word> ?code", once. Any other is dropped.
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
        const std::string_view payload = std::string_view(h.bytes).substr(space + 1, end - space - 1);
        for (auto& [account, p] : procs_) {
            if (!sync::sameBytes(word, std::to_string(p.id) + "-" + p.nonce)) {
                continue;
            }
            if (payload == "?code") {
                if (p.urlSent && !p.codeAsked && !p.setupToken && sync::signsInByPastedCode(p.provider)) {
                    p.codeAsked = true;
                    if (events_.codeWanted) {
                        events_.codeWanted(account, p.id);
                    }
                }
                break;
            }
            if (!p.urlSent) {
                p.urlSent = true;
                const sync::SecretValue url(std::string_view(h.bytes).substr(space + 1, end - space - 1));
                if (events_.url) {
                    events_.url(account, p.id, url);
                }
            }
            break;
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
        if (!q.codeOut.empty()) {
            writeCode(q);
        }
        // Ended? Looked at without collecting it (WNOWAIT), so that its group is ended (reap) while its pid is still
        // its own; then collected.
        siginfo_t info{};
        const bool exited = q.pid > 0 && ::waitid(P_PID, static_cast<id_t>(q.pid), &info,
                                                  WEXITED | WNOHANG | WNOWAIT) == 0 && info.si_pid == q.pid;
        if (exited) {
            ::kill(-q.pid, SIGKILL);
            int status = 0;
            ::waitpid(q.pid, &status, 0);
            q.pid = -1;
            reap(account, q, q.killAt != 0, status);
            continue;
        }
        if (q.master < 0) {
            // Its terminal ended, the program not yet: it is ended now, and collected.
            reap(account, q, q.killAt != 0, std::nullopt);
            continue;
        }
        if (q.killAt != 0 && now() >= q.killAt) {
            reap(account, q, true, std::nullopt);     // past its grace: SIGKILL to its group
        }
    }
    for (Replay& r : replays_) {
        if (!r.out.empty()) {
#if defined(MSG_NOSIGNAL)
            const ssize_t n = ::send(r.fd, r.out.data(), r.out.size(), MSG_NOSIGNAL);
#else
            const ssize_t n = ::send(r.fd, r.out.data(), r.out.size(), 0);
#endif
            if (n > 0) {
                r.sent = true;
                std::fill_n(r.out.begin(), static_cast<std::size_t>(n), '\0');
                r.out.erase(0, static_cast<std::size_t>(n));
            } else if (n < 0 && errno != EAGAIN && errno != EINTR && errno != ENOTCONN) {
                // Refused before a byte went: the next family, if one is left (localhost on IPv4 after IPv6).
                ::close(r.fd);
                r.fd = -1;
                if (r.sent || !connectReplay(r)) {
                    if (events_.log) {
                        events_.log("sign-in " + std::to_string(r.id) + ": " + r.account +
                                    ": the callback was not replayed: " + std::strerror(errno));
                    }
                    sync::wipeBytes(r.out);
                    r.until = 0;
                    continue;
                }
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
