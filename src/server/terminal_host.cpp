#include "server/terminal_host.h"

#include <fcntl.h>
#include <pwd.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <util.h>
#else
#include <pty.h>
#endif

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <random>

#include "pr/vterm.h"

namespace nexus::server {

struct TerminalHost::Running {
    int master = -1;
    pid_t pid = -1;
    PrVterm* vterm = nullptr;
    TerminalModes modes;
    std::string pending;            // input the pseudoterminal has not taken yet, in order
    bool outputEnded = false;
    bool reaped = false;
    std::uint32_t columns = 0;
    std::uint32_t rows = 0;
    std::vector<std::pair<std::uint64_t, sync::LineRef>> lines;  // each row's lib/vterm revision and painted text
    sync::TerminalRef shown;        // the view last given, while nothing it shows changed
    std::uint64_t shownRevision = ~std::uint64_t{};
    std::string shownModes;

    ~Running() {
        prVtermDestroy(vterm);
        if (master >= 0) {
            ::close(master);
        }
    }
};

namespace {

std::string named(TerminalId id) {
    return "terminal " + std::to_string(id);
}

bool isDirectory(const std::string& path) {
    struct stat st{};
    return !path.empty() && ::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

void setNonBlocking(int fd) {
    const int flags = ::fcntl(fd, F_GETFL);
    if (flags >= 0) {
        ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }
    ::fcntl(fd, F_SETFD, FD_CLOEXEC);
}

} // namespace

ShellLaunch loginShell(char** environ) {
    ShellLaunch out;
    const passwd* pw = ::getpwuid(::getuid());
    std::string shell;
    std::string home;
    for (char** e = environ; e != nullptr && *e != nullptr; ++e) {
        const std::string var(*e);
        if (var.rfind("NEXUS_", 0) == 0 || var.rfind("TERM=", 0) == 0 || var.rfind("COLORTERM=", 0) == 0) {
            continue;
        }
        if (var.rfind("SHELL=", 0) == 0) shell = var.substr(6);
        if (var.rfind("HOME=", 0) == 0) home = var.substr(5);
        out.environment.push_back(var);
    }
    if (shell.empty() && pw != nullptr && pw->pw_shell != nullptr && pw->pw_shell[0] == '/') shell = pw->pw_shell;
    if (shell.empty()) shell = "/bin/sh";
    if (home.empty() && pw != nullptr && pw->pw_dir != nullptr) {
        home = pw->pw_dir;
        out.environment.push_back("HOME=" + home);
    }
    out.environment.push_back("TERM=xterm-256color");
    out.environment.push_back("COLORTERM=truecolor");
    out.program = shell;
    out.arguments = {"-l"};
    out.home = home;
    return out;
}

std::uint32_t newEpoch() {
    std::uint32_t epoch = 0;
    std::random_device random;
    while (epoch == 0) {
        epoch = static_cast<std::uint32_t>(random());
    }
    return epoch;
}

TerminalHost::TerminalHost(std::uint32_t epoch, ShellLaunch launch) : book_(epoch), launch_(std::move(launch)) {}

TerminalHost::~TerminalHost() {
    for (auto& [id, t] : running_) {
        if (!t->reaped && t->pid > 0) {
            closing_.push_back(t->pid);
            ::kill(-t->pid, SIGHUP);
            ::kill(t->pid, SIGHUP);
        }
    }
    running_.clear();   // closes every pseudoterminal: a shell still reading it gets its hang-up
    for (pid_t pid : closing_) {
        int status = 0;
        bool gone = false;
        for (int i = 0; i < 20 && !gone; ++i) {
            gone = ::waitpid(pid, &status, WNOHANG) == pid;
            if (!gone) {
                ::usleep(10 * 1000);
            }
        }
        if (!gone) {
            ::kill(-pid, SIGKILL);
            ::kill(pid, SIGKILL);
            while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
            }
        }
    }
}

std::string TerminalHost::apply(const sync::Action& action, bool owner) {
    TerminalOutcome o = book_.apply(action, owner);
    if (!o.refused.empty()) {
        return o.refused;
    }
    const auto it = running_.find(o.effect.id);
    switch (o.effect.kind) {
        case TerminalEffect::Kind::None:
            break;
        case TerminalEffect::Kind::Spawn:
            spawn(o.effect);
            break;
        case TerminalEffect::Kind::Write:
            if (it != running_.end()) {
                if (it->second->pending.size() + action.secret.size() > kPendingLimit) {
                    return named(o.effect.id) + "'s shell is not reading its input; " +
                        std::to_string(action.secret.size()) + " bytes were not sent";
                }
                write(*it->second, action.secret.bytes());
            }
            break;
        case TerminalEffect::Kind::Resize:
            if (it != running_.end()) {
                Running& t = *it->second;
                t.columns = o.effect.columns;
                t.rows = o.effect.rows;
                prVtermResize(t.vterm, PrUVec2{t.columns, t.rows});
                if (!t.reaped) {
                    winsize ws{};
                    ws.ws_col = static_cast<unsigned short>(t.columns);
                    ws.ws_row = static_cast<unsigned short>(t.rows);
                    ::ioctl(t.master, TIOCSWINSZ, &ws);
                }
                changed_ = true;
            }
            break;
        case TerminalEffect::Kind::Kill:
            if (it != running_.end()) {
                kill(*it->second);
            }
            break;
    }
    // A close removes the terminal from the book; its screen goes with it.
    if (action.kind == sync::ActionKind::TerminalClose) {
        running_.erase(action.terminal);
        changed_ = true;
    }
    return {};
}

void TerminalHost::spawn(const TerminalEffect& e) {
    auto t = std::make_unique<Running>();
    t->columns = e.columns;
    t->rows = e.rows;
    PrVtermDesc desc{};
    desc.extent = PrUVec2{e.columns, e.rows};
    desc.scrollback = 0;                // the screen only: the window shows the screen (docs/terminals.md)
    t->vterm = prVtermCreate(&desc);
    changed_ = true;

    // Everything the child needs, made before the fork: after it the child calls only what is safe there.
    const std::string dir = isDirectory(e.directory) ? e.directory : launch_.home;
    std::vector<std::string> args{launch_.program};
    args.insert(args.end(), launch_.arguments.begin(), launch_.arguments.end());
    std::vector<char*> argv;
    for (std::string& a : args) argv.push_back(a.data());
    argv.push_back(nullptr);
    std::vector<std::string> env = launch_.environment;
    std::vector<char*> envp;
    for (std::string& v : env) envp.push_back(v.data());
    envp.push_back(nullptr);
    const long most = std::min<long>(::sysconf(_SC_OPEN_MAX) > 0 ? ::sysconf(_SC_OPEN_MAX) : 1024, 65536);

    winsize ws{};
    ws.ws_col = static_cast<unsigned short>(e.columns);
    ws.ws_row = static_cast<unsigned short>(e.rows);
    int master = -1;
    const pid_t pid = ::forkpty(&master, nullptr, nullptr, &ws);
    if (pid == 0) {
        sigset_t none;
        sigemptyset(&none);
        ::sigprocmask(SIG_SETMASK, &none, nullptr);
        for (int s : {SIGPIPE, SIGTERM, SIGINT, SIGHUP, SIGCHLD, SIGQUIT, SIGTSTP}) {
            ::signal(s, SIG_DFL);
        }
        for (long fd = 3; fd < most; ++fd) {
            ::close(static_cast<int>(fd));
        }
        if (dir.empty() || ::chdir(dir.c_str()) != 0) {
            if (::chdir("/") != 0) {
                ::_exit(126);
            }
        }
        ::execve(argv[0], argv.data(), envp.data());
        static const char kFailed[] = "nexus-server: the shell did not start\r\n";
        if (::write(STDERR_FILENO, kFailed, sizeof kFailed - 1) < 0) {
            ::_exit(127);
        }
        ::_exit(127);
    }
    if (pid < 0) {
        const std::string why = std::string("could not start: ") + std::strerror(errno);
        running_[e.id] = std::move(t);
        end(*running_[e.id], why);
        running_[e.id]->reaped = true;
        return;
    }
    setNonBlocking(master);
    t->master = master;
    t->pid = pid;
    running_[e.id] = std::move(t);
}

void TerminalHost::write(Running& t, const std::string& bytes) {
    if (t.reaped || t.master < 0) {
        return;
    }
    t.pending += bytes;
    flush(t);
}

void TerminalHost::flush(Running& t) {
    while (!t.pending.empty() && t.master >= 0) {
        const ssize_t n = ::write(t.master, t.pending.data(), t.pending.size());
        if (n > 0) {
            // The input's bytes leave memory as they go: what the shell took is overwritten, not just dropped.
            std::memset(t.pending.data(), 0, static_cast<std::size_t>(n));
            t.pending.erase(0, static_cast<std::size_t>(n));
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else {
            break;      // EAGAIN: the rest waits for POLLOUT; an error: the read sees the end
        }
    }
}

void TerminalHost::end(Running& t, std::string how) {
    const std::string line = "\r\n\x1b[0;2m[" + how + "]\x1b[0m";
    prVtermWrite(t.vterm, PrByteView{reinterpret_cast<const std::uint8_t*>(line.data()), line.size()});
    std::memset(t.pending.data(), 0, t.pending.size());
    t.pending.clear();
    changed_ = true;
    for (auto& [id, r] : running_) {
        if (r.get() == &t) {
            book_.ended(id, how);
        }
    }
}

void TerminalHost::kill(Running& t) {
    if (!t.reaped && t.pid > 0) {
        ::kill(-t.pid, SIGHUP);
        ::kill(t.pid, SIGHUP);
        closing_.push_back(t.pid);
        t.reaped = true;
    }
}

std::vector<pollfd> TerminalHost::fds() const {
    std::vector<pollfd> out;
    for (const auto& [id, t] : running_) {
        if (t->master >= 0 && !t->outputEnded) {
            out.push_back(pollfd{t->master, short(POLLIN | (t->pending.empty() ? 0 : POLLOUT)), 0});
        }
    }
    return out;
}

int TerminalHost::waitMs() const {
    for (const auto& [id, t] : running_) {
        if (t->outputEnded && !t->reaped) {
            return 50;
        }
    }
    return closing_.empty() ? -1 : 50;
}

bool TerminalHost::serve() {
    for (auto& [id, tp] : running_) {
        Running& t = *tp;
        if (t.master >= 0 && !t.outputEnded) {
            flush(t);
            std::uint8_t buffer[16384];
            std::size_t taken = 0;
            while (taken < kReadBudget) {
                const ssize_t n = ::read(t.master, buffer, sizeof buffer);
                if (n > 0) {
                    taken += static_cast<std::size_t>(n);
                    prVtermWrite(t.vterm, PrByteView{buffer, static_cast<std::size_t>(n)});
                    t.modes.feed(std::string_view(reinterpret_cast<const char*>(buffer), static_cast<std::size_t>(n)));
                    changed_ = true;
                } else if (n < 0 && errno == EINTR) {
                    continue;
                } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                    break;
                } else {
                    t.outputEnded = true;   // 0, or EIO once every slave descriptor is closed (Linux)
                    break;
                }
            }
            std::memset(buffer, 0, sizeof buffer);
            // The emulator's answers to the program (a cursor report, a device attributes reply) go back to it.
            const PrByteView answer = prVtermOutput(t.vterm);
            if (answer.size != 0) {
                t.pending.append(reinterpret_cast<const char*>(answer.data), answer.size);
                prVtermOutputConsume(t.vterm, answer.size);
                flush(t);
            }
            (void)prVtermTakeEvents(t.vterm);
        }
        if (!t.reaped && t.pid > 0) {
            int status = 0;
            const pid_t done = ::waitpid(t.pid, &status, WNOHANG);
            if (done == t.pid) {
                t.reaped = true;
                end(t, WIFSIGNALED(status) ? endedLine(true, WTERMSIG(status)) :
                    endedLine(false, WIFEXITED(status) ? WEXITSTATUS(status) : -1));
            }
        }
    }
    for (auto it = closing_.begin(); it != closing_.end();) {
        int status = 0;
        const pid_t done = ::waitpid(*it, &status, WNOHANG);
        it = done == *it || (done < 0 && errno == ECHILD) ? closing_.erase(it) : it + 1;
    }
    const bool changed = changed_;
    return changed;
}

sync::TerminalRef TerminalHost::view(TerminalId id, Running& t) {
    const TerminalBook::Entry* entry = book_.find(id);
    const std::string ended = entry != nullptr && entry->state == TerminalBook::State::Ended ? entry->ended : "";
    const std::string modes = t.modes.text();
    const std::uint64_t revision = prVtermRevision(t.vterm);
    if (t.shown && t.shownRevision == revision && t.shownModes == modes && t.shown->ended == ended) {
        return t.shown;
    }
    auto v = std::make_shared<sync::TerminalView>();
    v->id = id;
    v->opener = entry != nullptr ? entry->opener : "";
    const PrStringView title = prVtermTitle(t.vterm);
    v->title.assign(title.data != nullptr ? title.data : "", title.size);
    const PrUVec2 extent = prVtermExtent(t.vterm);
    v->columns = extent.x;
    v->rows = extent.y;
    const PrVtermCursor cursor = prVtermCursor(t.vterm);
    v->cursorColumn = cursor.position.x;
    v->cursorRow = cursor.position.y;
    v->cursorHidden = !cursor.visible;
    v->modes = modes;
    v->ended = ended;
    t.lines.resize(extent.y);
    const std::uint32_t scrollback = prVtermScrollbackCount(t.vterm);
    v->lines.reserve(extent.y);
    for (std::uint32_t r = 0; r < extent.y; ++r) {
        const std::uint64_t rev = prVtermLine(t.vterm, scrollback + r).revision;
        auto& [held, line] = t.lines[r];
        if (!line || held != rev) {
            line = std::make_shared<const std::string>(paintRow(t.vterm, r));
            held = rev;
        }
        v->lines.push_back(line);
    }
    t.shown = v;
    t.shownRevision = revision;
    t.shownModes = modes;
    return v;
}

std::vector<sync::TerminalRef> TerminalHost::views() {
    changed_ = false;
    std::vector<sync::TerminalRef> out;
    for (auto& [id, t] : running_) {
        out.push_back(view(id, *t));
    }
    return out;
}

} // namespace nexus::server
