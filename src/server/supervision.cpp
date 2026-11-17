// Supervision (server/supervision.h): keeping nexus-server running across boots, drain handovers (exit 75),
// intentional stops (exit 0), and post-start build failures (models/supervision/Supervision.tla).

#include "server/supervision.h"

#include "server/migration.h"
#include "server/sha256.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sstream>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

extern char** environ;

namespace nexus::server::supervision {

using store::Status;

namespace {

constexpr int kSocketTimeoutMs = 4000;
constexpr std::size_t kMaxLineBytes = 4096;

volatile sig_atomic_t gSuperviseStop = 0;
void onSuperviseSignal(int) { gSuperviseStop = 1; }

std::uint64_t steadyMs() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}

void realSleep(std::uint64_t ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

int exitOf(const Status& s) {
    switch (s.code()) {
    case Status::Code::Ok:
        return kExitOk;
    case Status::Code::Refused:
        return kExitRefused;
    case Status::Code::Busy:
        return kExitBusy;
    case Status::Code::NotFound:
    case Status::Code::Failed:
        return kExitFailed;
    }
    return kExitFailed;
}

std::string fromBytes(const store::Bytes& b) {
    return std::string(reinterpret_cast<const char*>(b.data()), b.size());
}

std::string trimTrailingSlash(std::string s) {
    while (s.size() > 1 && s.back() == '/') {
        s.pop_back();
    }
    return s;
}

std::string xmlEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
        case '&':
            out += "&amp;";
            break;
        case '<':
            out += "&lt;";
            break;
        case '>':
            out += "&gt;";
            break;
        case '"':
            out += "&quot;";
            break;
        case '\'':
            out += "&apos;";
            break;
        default:
            out.push_back(c);
            break;
        }
    }
    return out;
}

std::string systemdEscapeQuoted(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == '\r' || c == '\n') {
            out.push_back(' ');
        } else if (c == '\\') {
            out += "\\\\";
        } else if (c == '"') {
            out += "\\\"";
        } else if (c == '%') {
            out += "%%";
        } else {
            out.push_back(c);
        }
    }
    return out;
}

std::string systemdEscapePlain(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == '\r' || c == '\n') {
            out.push_back(' ');
        } else if (c == '%') {
            out += "%%";
        } else {
            out.push_back(c);
        }
    }
    return out;
}

bool socketAddress(const std::string& path, sockaddr_un* addr) {
    std::memset(addr, 0, sizeof *addr);
    addr->sun_family = AF_UNIX;
    if (path.empty() || path.size() >= sizeof addr->sun_path) {
        return false;
    }
    std::memcpy(addr->sun_path, path.data(), path.size());
    return true;
}

int connectUnix(const std::string& path) {
    sockaddr_un addr;
    if (!socketAddress(path, &addr)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    ::fcntl(fd, F_SETFD, FD_CLOEXEC);
#ifdef SO_NOSIGPIPE
    const int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
    if (::connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof addr) != 0) {
        const int e = errno;
        ::close(fd);
        errno = e;
        return -1;
    }
    return fd;
}

bool sendAll(int fd, const std::string& text) {
    std::size_t off = 0;
    int flags = 0;
#ifdef MSG_NOSIGPIPE
    (void)0;
#endif
#ifdef MSG_NOSIGNAL
    flags |= MSG_NOSIGNAL;
#endif
    while (off < text.size()) {
        const ssize_t n = ::send(fd, text.data() + off, text.size() - off, flags);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            return false;
        }
        off += static_cast<std::size_t>(n);
    }
    return true;
}

struct LineReader {
    std::string buf;

    bool readLine(int fd, std::string* out, int timeoutMs) {
        out->clear();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        for (;;) {
            const std::size_t nl = buf.find('\n');
            if (nl != std::string::npos) {
                *out = buf.substr(0, nl);
                if (!out->empty() && out->back() == '\r') {
                    out->pop_back();
                }
                buf.erase(0, nl + 1);
                return true;
            }
            if (buf.size() > kMaxLineBytes) {
                return false;
            }
            const auto rem = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now());
            if (rem.count() <= 0) {
                return false;
            }
            pollfd pfd{fd, POLLIN, 0};
            const int pr = ::poll(&pfd, 1, static_cast<int>(rem.count()));
            if (pr < 0) {
                if (errno == EINTR) {
                    continue;
                }
                return false;
            }
            if (pr == 0) {
                return false;
            }
            char chunk[512];
            const ssize_t n = ::read(fd, chunk, sizeof chunk);
            if (n < 0) {
                if (errno == EINTR) {
                    continue;
                }
                return false;
            }
            if (n == 0) {
                return false;
            }
            buf.append(chunk, static_cast<std::size_t>(n));
        }
    }
};

Status probeControlSocket(const std::string& socketPath, ServerSeen* out) {
    *out = ServerSeen{};
    sockaddr_un addr;
    if (!socketAddress(socketPath, &addr)) {
        return Status::failed(socketPath + " is longer than a Unix socket's address holds");
    }
    const int fd = connectUnix(socketPath);
    if (fd < 0) {
        if (errno == ENOENT || errno == ECONNREFUSED || errno == ECONNRESET) {
            return Status::ok();
        }
        return Status::failed("connect " + socketPath + ": " + std::strerror(errno));
    }
    LineReader reader;
    std::string first;
    std::string second;
    const bool ok = sendAll(fd, "status\n") && reader.readLine(fd, &first, kSocketTimeoutMs);
    bool hasSecond = false;
    if (ok && first == "SERVING") {
        hasSecond = reader.readLine(fd, &second, kSocketTimeoutMs);
    }
    ::close(fd);
    if (!ok || first != "SERVING") {
        return Status::ok();
    }
    if (!parseStatusLines(first, second, hasSecond, out)) {
        return Status::failed("the server on " + socketPath + " answered `status` with malformed build line `" +
                              second + "`");
    }
    return Status::ok();
}

bool dirHasExclusiveFlock(const std::string& dirPath) {
    const int fd = ::open(dirPath.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) {
        return false;
    }
    bool held = false;
    if (::flock(fd, LOCK_SH | LOCK_NB) == 0) {
        (void)::flock(fd, LOCK_UN);
    } else if (errno == EWOULDBLOCK || errno == EAGAIN) {
        held = true;
    }
    ::close(fd);
    return held;
}

class PosixHandoverGate final : public HandoverGate {
public:
    PosixHandoverGate(std::string serverDir, std::function<std::uint64_t()> now_ms,
                      std::function<void(std::uint64_t)> sleep_ms)
        : serverDir_(std::move(serverDir)),
          handoverDir_(serverDir_ + "/" + handover::kHandoverDir),
          statePath_(handoverDir_ + "/" + handover::kStateFile),
          superviseDir_(serverDir_ + "/" + kSuperviseDir),
          now_(now_ms ? std::move(now_ms) : std::function<std::uint64_t()>(steadyMs)),
          sleep_(sleep_ms ? std::move(sleep_ms) : std::function<void(std::uint64_t)>(realSleep)) {
        store::PosixOptions fsOpts;
        fsOpts.now_ms = now_;
        fsOpts.sleep_ms = sleep_;
        fs_ = store::make_posix_fs(fsOpts);
    }

    bool isLockedByOther() override {
        return dirHasExclusiveFlock(handoverDir_);
    }

    Status tryLock(std::unique_ptr<store::FileLock>& out) override {
        const Status ds = fs_->private_dir(handoverDir_);
        if (!ds.is_ok()) {
            return ds;
        }
        return fs_->lock(handoverDir_, store::LockMode::Exclusive, out);
    }

    Status resumeIfNeeded(bool lockAlreadyHeld = false) override {
        resumedPid_ = -1;
        if (::access(statePath_.c_str(), F_OK) != 0) {
            return Status::ok();
        }
        handover::Options hOpts;
        hOpts.now_ms = now_;
        hOpts.sleep_ms = sleep_;
        hOpts.lock_wait_ms = handover::kLockWaitMs;
        hOpts.lock_already_held = lockAlreadyHeld;
        hOpts.adopted_pid_out = &resumedPid_;
        handover::Result hr;
        const int rc = handover::command(serverDir_, {"resume"}, hOpts, hr);
        if (rc == handover::kExitOk || rc == handover::kExitRolledBack) {
            return Status::ok();
        }
        if (!lockAlreadyHeld && rc == handover::kExitBusy) {
            return Status::busy("another handover holds the lock on " + handoverDir_);
        }
        if (::access(statePath_.c_str(), F_OK) != 0) {
            return Status::ok();
        }
        return Status::failed(hr.err.empty() ? ("cannot resume interrupted handover in " + statePath_) : hr.err.front());
    }

    pid_t takeResumedPid() override {
        const pid_t pid = resumedPid_;
        resumedPid_ = -1;
        return pid;
    }

    bool hasExplicitPick(const std::string& build) override {
        return hasPickedBuild(*fs_, superviseDir_, build);
    }

    void clearExplicitPick(const std::string& build) override {
        (void)clearPickedBuild(*fs_, superviseDir_, build);
    }

private:
    std::string serverDir_;
    std::string handoverDir_;
    std::string statePath_;
    std::string superviseDir_;
    std::function<std::uint64_t()> now_;
    std::function<void(std::uint64_t)> sleep_;
    std::unique_ptr<store::Fs> fs_;
    pid_t resumedPid_ = -1;
};

class PosixChildPort final : public ChildPort {
public:
    PosixChildPort(std::string serverDir, std::function<std::uint64_t()> now_ms,
                   std::function<void(std::uint64_t)> sleep_ms)
        : serverDir_(std::move(serverDir)),
          socketPath_(serverDir_ + "/server.sock"),
          logPath_(serverDir_ + "/server.log"),
          storeDir_(serverDir_ + "/store"),
          handoverDir_(serverDir_ + "/" + handover::kHandoverDir),
          handoverStatePath_(handoverDir_ + "/" + handover::kStateFile),
          superviseDir_(serverDir_ + "/" + kSuperviseDir),
          programPath_(serverDir_ + "/" + builds::kStable + "/" + builds::kProgram),
          now_(now_ms ? std::move(now_ms) : std::function<std::uint64_t()>(steadyMs)),
          sleep_(sleep_ms ? std::move(sleep_ms) : std::function<void(std::uint64_t)>(realSleep)),
          fs_(store::make_posix_fs()) {}

    ~PosixChildPort() override {
        killDirectChild();
        reapStrayChildren();
    }

    void adoptSpawned(pid_t pid, const std::string& build) override {
        if (pid <= 0 || build.empty()) {
            return;
        }
        if (directPid_ > 0 && directPid_ != pid) {
            killDirectChild();
        }
        directPid_ = pid;
        activeBuild_ = build;
        adopted_ = false;
        adoptedSawDrain_ = false;
        hasExited_ = false;
        exitedBuild_.clear();
        lastExit_ = 0;
    }

    Status inspect(ChildObs* out) override {
        if (hasExited_) {
            *out = ChildObs{ChildState::Exited, exitedBuild_, lastExit_};
            return Status::ok();
        }
        if (directPid_ > 0) {
            int status = 0;
            const pid_t done = ::waitpid(directPid_, &status, WNOHANG);
            if (done == 0) {
                *out = ChildObs{ChildState::Serving, activeBuild_, 0};
                return Status::ok();
            }
            if (done == directPid_) {
                directPid_ = -1;
                const int code = WIFEXITED(status) ? WEXITSTATUS(status) : (128 + WTERMSIG(status));
                hasExited_ = true;
                exitedBuild_ = activeBuild_;
                lastExit_ = code;
                activeBuild_.clear();
                *out = ChildObs{ChildState::Exited, exitedBuild_, lastExit_};
                return Status::ok();
            }
            directPid_ = -1;
        }
        reapStrayChildren();
        if (adopted_) {
            ServerSeen seen;
            const Status ps = probeControlSocket(socketPath_, &seen);
            if (!ps.is_ok()) {
                return ps;
            }
            if (seen.serving && !seen.legacy) {
                if (seen.build == activeBuild_) {
                    if (seen.draining) {
                        adoptedSawDrain_ = true;
                    }
                    *out = ChildObs{ChildState::Serving, activeBuild_, 0};
                    return Status::ok();
                }
                // External handover already switched and started another build.
                adopted_ = false;
                adoptedSawDrain_ = false;
                hasExited_ = true;
                exitedBuild_ = activeBuild_;
                lastExit_ = kDrainExit;
                activeBuild_.clear();
                *out = ChildObs{ChildState::Exited, exitedBuild_, lastExit_};
                return Status::ok();
            }
            if (dirHasExclusiveFlock(storeDir_)) {
                // Adopted server closed its socket and is still finishing shutdown under the store lock.
                *out = ChildObs{ChildState::Serving, activeBuild_, 0};
                return Status::ok();
            }
            int code = 1;
            if (readStoppedFlag()) {
                code = 0;
            } else if (adoptedSawDrain_ || dirHasExclusiveFlock(handoverDir_) ||
                       ::access(handoverStatePath_.c_str(), F_OK) == 0) {
                code = kDrainExit;
            }
            adopted_ = false;
            adoptedSawDrain_ = false;
            hasExited_ = true;
            exitedBuild_ = activeBuild_;
            lastExit_ = code;
            activeBuild_.clear();
            *out = ChildObs{ChildState::Exited, exitedBuild_, lastExit_};
            return Status::ok();
        }

        ServerSeen seen;
        const Status ps = probeControlSocket(socketPath_, &seen);
        if (!ps.is_ok()) {
            return ps;
        }
        if (seen.serving && !seen.legacy && !seen.build.empty() && !seen.draining) {
            adopted_ = true;
            adoptedSawDrain_ = false;
            activeBuild_ = seen.build;
            *out = ChildObs{ChildState::Serving, activeBuild_, 0};
            return Status::ok();
        }
        *out = ChildObs{ChildState::Down, std::string(), 0};
        return Status::ok();
    }

    Status spawn(const std::string& expectedBuild, std::uint64_t limit_ms, ChildObs* out) override {
        killDirectChild();
        adopted_ = false;
        adoptedSawDrain_ = false;
        hasExited_ = false;
        activeBuild_ = expectedBuild;

        // Check if another process just brought `expectedBuild` to SERVING before we fork.
        ServerSeen already;
        Status ps = probeControlSocket(socketPath_, &already);
        if (!ps.is_ok()) {
            return ps;
        }
        if (already.serving && !already.legacy && already.build == expectedBuild && !already.draining) {
            adopted_ = true;
            *out = ChildObs{ChildState::Serving, expectedBuild, 0};
            return Status::ok();
        }

        std::string spawnWhy;
        if (!spawnDirect(&spawnWhy)) {
            hasExited_ = true;
            exitedBuild_ = expectedBuild;
            lastExit_ = 1;
            activeBuild_.clear();
            *out = ChildObs{ChildState::Exited, exitedBuild_, lastExit_};
            return Status::ok();
        }

        const std::uint64_t began = now_();
        for (;;) {
            ps = probeControlSocket(socketPath_, &already);
            if (!ps.is_ok()) {
                killDirectChild();
                return ps;
            }
            if (already.serving && !already.legacy && already.build == expectedBuild && !already.draining) {
                if (directPid_ > 0) {
                    int status = 0;
                    if (::waitpid(directPid_, &status, WNOHANG) == directPid_) {
                        directPid_ = -1;
                        adopted_ = true;
                    }
                } else {
                    adopted_ = true;
                }
                *out = ChildObs{ChildState::Serving, expectedBuild, 0};
                return Status::ok();
            }
            if (directPid_ > 0) {
                int status = 0;
                const pid_t done = ::waitpid(directPid_, &status, WNOHANG);
                if (done == directPid_) {
                    directPid_ = -1;
                    const bool exitedZero = WIFEXITED(status) && WEXITSTATUS(status) == 0;
                    const int rawCode = WIFEXITED(status) ? WEXITSTATUS(status) : (128 + WTERMSIG(status));
                    if (exitedZero) {
                        if (readStoppedFlag()) {
                            hasExited_ = true;
                            exitedBuild_ = expectedBuild;
                            lastExit_ = 0;
                            activeBuild_.clear();
                            *out = ChildObs{ChildState::Exited, exitedBuild_, lastExit_};
                            return Status::ok();
                        }
                        ServerSeen secondLook;
                        if (probeControlSocket(socketPath_, &secondLook).is_ok() && secondLook.serving &&
                            !secondLook.legacy && secondLook.build == expectedBuild && !secondLook.draining) {
                            adopted_ = true;
                            *out = ChildObs{ChildState::Serving, expectedBuild, 0};
                            return Status::ok();
                        }
                    }
                    if (rawCode == kDrainExit) {
                        hasExited_ = true;
                        exitedBuild_ = expectedBuild;
                        lastExit_ = kDrainExit;
                        activeBuild_.clear();
                        *out = ChildObs{ChildState::Exited, exitedBuild_, lastExit_};
                        return Status::ok();
                    }
                    const int crashCode = (rawCode == 0) ? 1 : rawCode;
                    hasExited_ = true;
                    exitedBuild_ = expectedBuild;
                    lastExit_ = crashCode;
                    activeBuild_.clear();
                    *out = ChildObs{ChildState::Exited, exitedBuild_, lastExit_};
                    return Status::ok();
                }
            }
            if (now_() - began >= limit_ms) {
                killDirectChild();
                hasExited_ = true;
                exitedBuild_ = expectedBuild;
                lastExit_ = 1;
                activeBuild_.clear();
                *out = ChildObs{ChildState::Exited, exitedBuild_, lastExit_};
                return Status::ok();
            }
            sleep_(kPollMs);
        }
    }

    Status drainToExit() override {
        const std::string buildBefore = activeBuild_;
        const int fd = connectUnix(socketPath_);
        if (fd >= 0) {
            LineReader reader;
            std::string reply;
            const bool ok = sendAll(fd, "drain\n") && reader.readLine(fd, &reply, kSocketTimeoutMs);
            ::close(fd);
            if (ok && reply.rfind("ERROR unknown request;", 0) == 0) {
                const int sfd = connectUnix(socketPath_);
                if (sfd >= 0) {
                    std::string sreply;
                    (void)(sendAll(sfd, "stop\n") && reader.readLine(sfd, &sreply, kSocketTimeoutMs));
                    ::close(sfd);
                }
            }
        }
        adoptedSawDrain_ = true;
        const std::uint64_t began = now_();
        const std::uint64_t limit = kDrainMs + 5000;
        while (now_() - began < limit) {
            ChildObs obs;
            const Status s = inspect(&obs);
            if (!s.is_ok()) {
                return s;
            }
            if (obs.state == ChildState::Exited) {
                lastExit_ = kDrainExit;
                return Status::ok();
            }
            if (obs.state == ChildState::Down) {
                hasExited_ = true;
                exitedBuild_ = buildBefore;
                lastExit_ = kDrainExit;
                return Status::ok();
            }
            sleep_(kPollMs);
        }
        killDirectChild();
        adopted_ = false;
        hasExited_ = true;
        exitedBuild_ = buildBefore;
        lastExit_ = kDrainExit;
        activeBuild_.clear();
        return Status::ok();
    }

    Status stopToExit() override {
        const std::string buildBefore = activeBuild_;
        const bool wasStopped = readStoppedFlag();
        if (directPid_ > 0) {
            ::kill(directPid_, SIGTERM);
            const std::uint64_t began = now_();
            while (now_() - began < 5000) {
                int status = 0;
                if (::waitpid(directPid_, &status, WNOHANG) == directPid_) {
                    directPid_ = -1;
                    break;
                }
                sleep_(kPollMs);
            }
            killDirectChild();
        } else if (adopted_) {
            const int fd = connectUnix(socketPath_);
            if (fd >= 0) {
                LineReader reader;
                std::string reply;
                (void)(sendAll(fd, "stop\n") && reader.readLine(fd, &reply, kSocketTimeoutMs));
                ::close(fd);
            }
            const std::uint64_t began = now_();
            while (now_() - began < 5000) {
                ServerSeen seen;
                if (probeControlSocket(socketPath_, &seen).is_ok() && !seen.serving && !dirHasExclusiveFlock(storeDir_)) {
                    break;
                }
                sleep_(kPollMs);
            }
            adopted_ = false;
            if (!wasStopped) {
                restoreUnstoppedFlag();
            }
        }
        reapStrayChildren();
        activeBuild_.clear();
        hasExited_ = true;
        exitedBuild_ = buildBefore;
        lastExit_ = 0;
        return Status::ok();
    }

    int consumeExit() override {
        const int code = lastExit_;
        hasExited_ = false;
        exitedBuild_.clear();
        lastExit_ = 0;
        return code;
    }

private:
    bool spawnDirect(std::string* why) {
        posix_spawn_file_actions_t files;
        posix_spawn_file_actions_init(&files);
        posix_spawn_file_actions_addopen(&files, 0, "/dev/null", O_RDONLY, 0);
        posix_spawn_file_actions_addopen(&files, 1, logPath_.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0600);
        posix_spawn_file_actions_adddup2(&files, 1, 2);
        posix_spawnattr_t attr;
        posix_spawnattr_init(&attr);
        posix_spawnattr_setpgroup(&attr, 0);
#if defined(__APPLE__)
        posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_CLOEXEC_DEFAULT);
#else
        posix_spawn_file_actions_addclosefrom_np(&files, 3);
        posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP);
#endif
        std::vector<std::string> args = {programPath_, "serve", "--dir", serverDir_};
        std::vector<char*> argv;
        for (std::string& a : args) {
            argv.push_back(a.data());
        }
        argv.push_back(nullptr);
        pid_t pid = -1;
        const int rc = ::posix_spawn(&pid, programPath_.c_str(), &files, &attr, argv.data(), environ);
        posix_spawn_file_actions_destroy(&files);
        posix_spawnattr_destroy(&attr);
        if (rc != 0) {
            *why = "cannot start `" + programPath_ + " serve`: " + std::strerror(rc);
            return false;
        }
        directPid_ = pid;
        return true;
    }

    void killDirectChild() {
        if (directPid_ > 0) {
            int status = 0;
            if (::waitpid(directPid_, &status, WNOHANG) == 0) {
                ::kill(-directPid_, SIGKILL);
                ::kill(directPid_, SIGKILL);
                while (::waitpid(directPid_, &status, 0) < 0 && errno == EINTR) {
                }
            }
            directPid_ = -1;
        }
    }

    void reapStrayChildren() {
        if (directPid_ <= 0) {
            int status = 0;
            while (::waitpid(-1, &status, WNOHANG) > 0) {
            }
        }
    }

    bool readStoppedFlag() const {
        SuperviseState st;
        return loadEffectiveState(*fs_, superviseDir_, &st).is_ok() && st.stopped;
    }

    void restoreUnstoppedFlag() {
        (void)clearStoppedMarker(*fs_, superviseDir_);
        store::Bytes raw;
        if (!fs_->read_all(superviseDir_ + "/" + kStateFile, raw).is_ok()) {
            return;
        }
        SuperviseState st;
        std::string why;
        if (readState(fromBytes(raw), &st, &why) && st.stopped) {
            st.stopped = false;
            (void)writeStateFile(*fs_, superviseDir_, st);
        }
    }

    std::string serverDir_;
    std::string socketPath_;
    std::string logPath_;
    std::string storeDir_;
    std::string handoverDir_;
    std::string handoverStatePath_;
    std::string superviseDir_;
    std::string programPath_;
    std::function<std::uint64_t()> now_;
    std::function<void(std::uint64_t)> sleep_;
    std::unique_ptr<store::Fs> fs_;
    pid_t directPid_ = -1;
    bool adopted_ = false;
    bool adoptedSawDrain_ = false;
    std::string activeBuild_;
    bool hasExited_ = false;
    std::string exitedBuild_;
    int lastExit_ = 0;
};

} // namespace

ServiceUnitPaths serviceUnitPaths(const std::string& serverDir, const std::string& home,
                                  const std::string& defaultServerDir) {
    const std::string normDir = trimTrailingSlash(serverDir);
    const std::string normHome = trimTrailingSlash(home);
    const std::string normDefault = trimTrailingSlash(defaultServerDir);

    ServiceUnitPaths out;
    if (normDefault.empty() || normDir == normDefault) {
        out.launchdLabel = kDefaultLaunchdLabel;
        out.systemdUnitName = kDefaultSystemdUnit;
    } else {
        const std::string suffix = sha256Hex(normDir).substr(0, 12);
        out.launchdLabel = std::string(kDefaultLaunchdLabel) + "." + suffix;
        out.systemdUnitName = std::string(kDefaultSystemdUnit) + "-" + suffix;
    }
    out.launchdPlistPath = normHome + "/Library/LaunchAgents/" + out.launchdLabel + ".plist";
    out.systemdUnitPath = normHome + "/.config/systemd/user/" + out.systemdUnitName + ".service";
    out.programPath = normDir + "/" + builds::kStable + "/" + builds::kProgram;
    out.logPath = normDir + "/server.log";
    return out;
}

std::string launchdPlist(const std::string& label, const std::string& program, const std::string& serverDir,
                         const std::string& log, const std::string& home, const std::string& path) {
    std::string out;
    out += "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    out += "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
           "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n";
    out += "<plist version=\"1.0\">\n";
    out += "<dict>\n";
    out += "    <key>Label</key>\n";
    out += "    <string>" + xmlEscape(label) + "</string>\n";
    out += "    <key>ProgramArguments</key>\n";
    out += "    <array>\n";
    out += "        <string>" + xmlEscape(program) + "</string>\n";
    out += "        <string>supervise</string>\n";
    out += "        <string>--launchd</string>\n";
    out += "        <string>--dir</string>\n";
    out += "        <string>" + xmlEscape(serverDir) + "</string>\n";
    out += "    </array>\n";
    out += "    <key>WorkingDirectory</key>\n";
    out += "    <string>" + xmlEscape(serverDir) + "</string>\n";
    if (!home.empty() || !path.empty()) {
        out += "    <key>EnvironmentVariables</key>\n";
        out += "    <dict>\n";
        if (!home.empty()) {
            out += "        <key>HOME</key>\n";
            out += "        <string>" + xmlEscape(home) + "</string>\n";
        }
        if (!path.empty()) {
            out += "        <key>PATH</key>\n";
            out += "        <string>" + xmlEscape(path) + "</string>\n";
        }
        out += "    </dict>\n";
    }
    out += "    <key>RunAtLoad</key>\n";
    out += "    <true/>\n";
    out += "    <key>KeepAlive</key>\n";
    out += "    <dict>\n";
    out += "        <key>SuccessfulExit</key>\n";
    out += "        <false/>\n";
    out += "    </dict>\n";
    out += "    <key>ThrottleInterval</key>\n";
    out += "    <integer>10</integer>\n";
    out += "    <key>StandardOutPath</key>\n";
    out += "    <string>" + xmlEscape(log) + "</string>\n";
    out += "    <key>StandardErrorPath</key>\n";
    out += "    <string>" + xmlEscape(log) + "</string>\n";
    out += "</dict>\n";
    out += "</plist>\n";
    return out;
}

std::string systemdUnit(const std::string& program, const std::string& serverDir, const std::string& home,
                        const std::string& path) {
    std::string out;
    out += "[Unit]\n";
    out += "Description=nexus-server supervisor (" + systemdEscapePlain(serverDir) + ")\n\n";
    out += "[Service]\n";
    out += "Type=simple\n";
    out += "WorkingDirectory=" + systemdEscapePlain(serverDir) + "\n";
    if (!home.empty()) {
        out += "Environment=\"HOME=" + systemdEscapeQuoted(home) + "\"\n";
    }
    if (!path.empty()) {
        out += "Environment=\"PATH=" + systemdEscapeQuoted(path) + "\"\n";
    }
    out += "ExecStart=\"" + systemdEscapeQuoted(program) + "\" supervise --dir \"" +
           systemdEscapeQuoted(serverDir) + "\"\n";
    out += "KillMode=process\n";
    out += "Restart=on-failure\n";
    out += "RestartPreventExitStatus=3 78\n";
    out += "RestartSec=2\n";
    out += "TimeoutStopSec=30\n\n";
    out += "[Install]\n";
    out += "WantedBy=default.target\n";
    return out;
}

struct SupervisorLoop::Run {
    SupervisorLoop& sup;
    Result& r;
    std::unique_ptr<store::FileLock> hold;
    std::unique_ptr<store::FileLock> rbHold;
    bool pastLock = false;

    Run(SupervisorLoop& s, Result& res) : sup(s), r(res) {}

    ~Run() {
        rbHold.reset();
        sup.rbLocked_ = false;
        hold.reset();
        sup.locked_ = false;
        sup.phase_ = "idle";
    }

    bool fault(Fault f) const { return sup.options_.fault == f; }
    std::uint64_t now() const { return sup.options_.now_ms ? sup.options_.now_ms() : steadyMs(); }
    void pause(std::uint64_t ms) const {
        if (sup.options_.sleep_ms) {
            sup.options_.sleep_ms(ms);
        } else {
            realSleep(ms);
        }
    }
    bool stopRequested() const {
        return sup.options_.stop_requested ? sup.options_.stop_requested() : false;
    }

    void at(const char* step) {
        sup.phase_ = step;
        if (sup.options_.hook) {
            sup.options_.hook(step);
        }
    }

    void markStopped(bool v) {
        if (sup.options_.service_stopped) {
            *sup.options_.service_stopped = v;
        }
    }

    void markHalted(bool v) {
        if (sup.options_.service_halted) {
            *sup.options_.service_halted = v;
        }
    }

    int finish(const Status& s, bool explicitExit = false, int code = kExitOk) {
        if (rbHold || sup.rbLocked_) {
            rbHold.reset();
            sup.rbLocked_ = false;
        }
        if (pastLock) {
            at("unlock");
            hold.reset();
            sup.locked_ = false;
            pastLock = false;
        }
        sup.phase_ = "idle";
        if (sup.options_.hook) {
            sup.options_.hook("idle");
        }
        if (!s.is_ok()) {
            r.err.push_back(s.message());
        }
        r.exit = explicitExit ? code : exitOf(s);
        return r.exit;
    }

    Status settleDir() {
        Status s = sup.fs_.sync_dir(sup.superviseDir_);
        if (!s.is_ok()) {
            return s;
        }
        std::vector<std::string> entries;
        s = sup.fs_.list(sup.superviseDir_, entries);
        if (!s.is_ok()) {
            return s;
        }
        std::vector<std::string> leftovers;
        for (const std::string& n : entries) {
            if (n.rfind(kStateTmpPrefix, 0) == 0 || n.rfind(kStoppedTmpPrefix, 0) == 0 ||
                n.rfind(kPickedTmpPrefix, 0) == 0) {
                leftovers.push_back(n);
            }
        }
        std::sort(leftovers.begin(), leftovers.end());
        for (const std::string& n : leftovers) {
            s = sup.fs_.remove(sup.superviseDir_ + "/" + n);
            if (!s.is_ok() && s.code() != Status::Code::NotFound) {
                return s;
            }
        }
        return Status::ok();
    }

    Status readDisk(SuperviseState* out) {
        return loadEffectiveState(sup.fs_, sup.superviseDir_, out);
    }

    Status persistDisk(const std::string& b, std::uint32_t crashes, const std::set<std::string>& shunned, bool halted,
                       bool stopped) {
        if (fault(Fault::InMemoryState)) {
            return Status::ok();
        }
        if (stopped) {
            const Status ms = writeStoppedMarker(sup.fs_, sup.superviseDir_);
            if (!ms.is_ok()) {
                return ms;
            }
        } else if (fault(Fault::StopRestarts)) {
            (void)clearStoppedMarker(sup.fs_, sup.superviseDir_);
        }
        SuperviseState st;
        st.build = b;
        st.crashes = crashes;
        st.halted = halted;
        st.stopped = stopped;
        st.shunned = shunned;
        return writeStateFile(sup.fs_, sup.superviseDir_, st);
    }

    int execute() {
        r = Result{};
        Status s = sup.fs_.private_dir(sup.superviseDir_);
        if (!s.is_ok()) {
            return finish(s);
        }

        const std::uint64_t lockBegan = now();
        for (;;) {
            at("lock");
            if (fault(Fault::NoSupLock)) {
                break;
            }
            s = sup.fs_.lock(sup.superviseDir_, store::LockMode::Exclusive, hold);
            if (s.is_ok()) {
                sup.locked_ = true;
                break;
            }
            if (s.code() != Status::Code::Busy) {
                return finish(s);
            }
            if (now() - lockBegan >= sup.options_.lock_wait_ms) {
                return finish(Status::busy("another supervisor holds the lock on " + sup.superviseDir_ +
                                           " (`nexus-server supervise status`)"));
            }
            pause(kLockRetryMs);
        }
        pastLock = true;

        enum class Step {
            Load,
            Check,
            Spawn,
            Watch,
            Reap,
            Backoff,
            RbLock,
            Rollback,
            RbUnlock,
            Unlock,
        };

        Step next = Step::Load;
        std::string sbuild;
        std::uint32_t scrashes = 0;
        std::set<std::string> sshunned;
        std::uint64_t servingSince = 0;
        bool stoppedOutcome = false;
        bool haltedOutcome = false;
        bool refusedOutcome = false;
        Status outcome = Status::ok();

        auto failWithBuilds = [&](const builds::Result& br) {
            for (const std::string& e : br.err) {
                r.err.push_back(e);
            }
            if (br.exit == builds::kExitRefused) {
                refusedOutcome = true;
            } else {
                outcome = Status::failed(br.err.empty() ? "builds error" : br.err.front());
            }
            next = Step::Unlock;
        };

        while (next != Step::Unlock) {
            switch (next) {
            case Step::Load: {
                at("load");
                s = settleDir();
                if (!s.is_ok()) {
                    outcome = s;
                    next = Step::Unlock;
                    break;
                }
                SuperviseState disk;
                s = readDisk(&disk);
                if (!s.is_ok()) {
                    outcome = s;
                    next = Step::Unlock;
                    break;
                }
                if (sup.options_.clear_stopped_on_start && (disk.stopped || disk.halted)) {
                    if (!fault(Fault::InMemoryState)) {
                        s = clearStoppedMarker(sup.fs_, sup.superviseDir_);
                        if (!s.is_ok()) {
                            outcome = s;
                            next = Step::Unlock;
                            break;
                        }
                    }
                    disk.stopped = false;
                    disk.halted = false;
                    disk.crashes = 0;
                    s = persistDisk(disk.build, disk.crashes, disk.shunned, false, false);
                    if (!s.is_ok()) {
                        outcome = s;
                        next = Step::Unlock;
                        break;
                    }
                }
                sbuild = disk.build;
                scrashes = disk.crashes;
                sshunned = disk.shunned;

                std::string cur;
                std::string prev;
                builds::Result br;
                const bool haveActive = (sup.placement_.active(&cur, &prev, br) == builds::kExitOk);
                const bool pickedCur =
                    !cur.empty() && (sup.handoverGate_.hasExplicitPick(cur) || disk.picked == cur);

                if (disk.stopped && !fault(Fault::StopRestarts)) {
                    stoppedOutcome = true;
                    markStopped(true);
                    r.out.push_back("stopped");
                    next = Step::Unlock;
                } else if (disk.halted && haveActive && !cur.empty() && disk.build == cur && !pickedCur &&
                           !fault(Fault::NoHaltBound)) {
                    haltedOutcome = true;
                    markHalted(true);
                    r.err.push_back("supervision previously halted on build " + cur +
                                    " after exhausting restart bounds; place and hand over to a healthy build or run "
                                    "`nexus-server supervise --start`");
                    next = Step::Unlock;
                } else {
                    next = Step::Check;
                }
                break;
            }

            case Step::Check: {
                at("check");
                if (stopRequested()) {
                    (void)sup.child_.stopToExit();
                    (void)sup.child_.consumeExit();
                    stoppedOutcome = true;
                    next = Step::Unlock;
                    break;
                }
                if (sup.handoverGate_.isLockedByOther()) {
                    pause(sup.options_.poll_ms);
                    break;
                }
                s = sup.handoverGate_.resumeIfNeeded(false);
                if (s.code() == Status::Code::Busy) {
                    pause(sup.options_.poll_ms);
                    break;
                }
                if (!s.is_ok()) {
                    outcome = s;
                    next = Step::Unlock;
                    break;
                }
                if (const pid_t rpid = sup.handoverGate_.takeResumedPid(); rpid > 0) {
                    std::string rcur;
                    std::string rprev;
                    builds::Result rbr;
                    if (sup.placement_.active(&rcur, &rprev, rbr) == builds::kExitOk && !rcur.empty()) {
                        sup.child_.adoptSpawned(rpid, rcur);
                    }
                }

                SuperviseState diskNow;
                s = readDisk(&diskNow);
                if (!s.is_ok()) {
                    outcome = s;
                    next = Step::Unlock;
                    break;
                }
                if (diskNow.stopped && !fault(Fault::StopRestarts)) {
                    stoppedOutcome = true;
                    markStopped(true);
                    r.out.push_back("stopped");
                    next = Step::Unlock;
                    break;
                }

                std::string cur;
                std::string prev;
                builds::Result br;
                if (sup.placement_.active(&cur, &prev, br) != builds::kExitOk) {
                    failWithBuilds(br);
                    break;
                }
                if (cur.empty()) {
                    if (sup.options_.launchd_mode) {
                        (void)persistDisk(std::string(), scrashes, sshunned, true, false);
                        markHalted(true);
                    }
                    refusedOutcome = true;
                    r.err.push_back("no build is current (`nexus-server builds current <hash>` chooses one)");
                    next = Step::Unlock;
                    break;
                }

                const bool pickedCur = sup.handoverGate_.hasExplicitPick(cur) || (diskNow.picked == cur);
                if (diskNow.halted && diskNow.build == cur && !pickedCur && !fault(Fault::NoHaltBound)) {
                    haltedOutcome = true;
                    markHalted(true);
                    r.err.push_back("supervision halted on build " + cur);
                    next = Step::Unlock;
                    break;
                }

                builds::Result cr;
                if (sup.placement_.checkSwitch(cur, cr, fault(Fault::ServeUntested)) != builds::kExitOk) {
                    s = persistDisk(cur, scrashes, sshunned, true, false);
                    if (!s.is_ok()) {
                        outcome = s;
                    } else {
                        haltedOutcome = true;
                        markHalted(true);
                        refusedOutcome = (cr.exit == builds::kExitRefused);
                        for (const std::string& e : cr.err) {
                            r.err.push_back(e);
                        }
                    }
                    next = Step::Unlock;
                    break;
                }

                std::set<std::string> newShun = sshunned;
                if (pickedCur) {
                    newShun.erase(cur);
                }
                const std::uint32_t newCrashes = (sbuild == cur && !pickedCur) ? scrashes : 0;
                sbuild = cur;
                scrashes = newCrashes;
                sshunned = newShun;
                if (pickedCur) {
                    sup.handoverGate_.clearExplicitPick(cur);
                }

                ChildObs obs;
                s = sup.child_.inspect(&obs);
                if (!s.is_ok()) {
                    outcome = s;
                    next = Step::Unlock;
                    break;
                }

                const bool eligRb = !fault(Fault::NoPostStartRollback) &&
                                    newCrashes >= sup.options_.max_crashes_before_rollback && !prev.empty() &&
                                    (fault(Fault::PingPongRollback) || newShun.count(prev) == 0);
                const bool mustHalt = !fault(Fault::NoHaltBound) && newCrashes >= sup.options_.max_restarts;

                if (obs.state == ChildState::Serving) {
                    s = persistDisk(cur, newCrashes, newShun, false, false);
                    if (!s.is_ok()) {
                        outcome = s;
                        next = Step::Unlock;
                        break;
                    }
                    haltedOutcome = false;
                    markHalted(false);
                    servingSince = now();
                    next = Step::Watch;
                } else if (obs.state == ChildState::Exited) {
                    s = persistDisk(cur, newCrashes, newShun, false, false);
                    if (!s.is_ok()) {
                        outcome = s;
                        next = Step::Unlock;
                        break;
                    }
                    haltedOutcome = false;
                    markHalted(false);
                    next = Step::Reap;
                } else if (eligRb) {
                    s = persistDisk(cur, newCrashes, newShun, false, false);
                    if (!s.is_ok()) {
                        outcome = s;
                        next = Step::Unlock;
                        break;
                    }
                    haltedOutcome = false;
                    markHalted(false);
                    next = Step::RbLock;
                } else if (mustHalt) {
                    s = persistDisk(cur, newCrashes, newShun, true, false);
                    if (!s.is_ok()) {
                        outcome = s;
                    } else {
                        haltedOutcome = true;
                        markHalted(true);
                        r.err.push_back("build " + cur + " crashed " + std::to_string(newCrashes) +
                                        " times with no unshunned previous build; halting supervision");
                    }
                    next = Step::Unlock;
                } else {
                    s = persistDisk(cur, newCrashes, newShun, false, false);
                    if (!s.is_ok()) {
                        outcome = s;
                        next = Step::Unlock;
                        break;
                    }
                    haltedOutcome = false;
                    markHalted(false);
                    next = Step::Spawn;
                }
                break;
            }

            case Step::Spawn: {
                at("spawn");
                if (stopRequested()) {
                    stoppedOutcome = true;
                    next = Step::Unlock;
                    break;
                }
                SuperviseState diskNow;
                s = readDisk(&diskNow);
                if (!s.is_ok()) {
                    outcome = s;
                    next = Step::Unlock;
                    break;
                }
                std::string curNow;
                std::string prevNow;
                builds::Result br;
                if (sup.placement_.active(&curNow, &prevNow, br) != builds::kExitOk) {
                    failWithBuilds(br);
                    break;
                }
                if (sup.handoverGate_.isLockedByOther() || curNow != sbuild ||
                    (diskNow.stopped && !fault(Fault::StopRestarts))) {
                    next = Step::Check;
                    break;
                }

                ChildObs obs;
                s = sup.child_.spawn(sbuild, sup.options_.start_wait_ms, &obs);
                if (!s.is_ok()) {
                    outcome = s;
                    next = Step::Unlock;
                    break;
                }
                if (obs.state == ChildState::Serving) {
                    servingSince = now();
                    next = Step::Watch;
                } else {
                    next = Step::Reap;
                }
                break;
            }

            case Step::Watch: {
                for (;;) {
                    at("watch");
                    if (stopRequested()) {
                        (void)sup.child_.stopToExit();
                        (void)sup.child_.consumeExit();
                        stoppedOutcome = true;
                        next = Step::Unlock;
                        break;
                    }
                    ChildObs obs;
                    s = sup.child_.inspect(&obs);
                    if (!s.is_ok()) {
                        outcome = s;
                        next = Step::Unlock;
                        break;
                    }
                    if (obs.state == ChildState::Exited) {
                        next = Step::Reap;
                        break;
                    }
                    if (obs.state == ChildState::Down) {
                        next = Step::Check;
                        break;
                    }
                    if (!sup.handoverGate_.isLockedByOther()) {
                        std::string curNow;
                        std::string prevNow;
                        builds::Result br;
                        if (sup.placement_.active(&curNow, &prevNow, br) == builds::kExitOk && !curNow.empty() &&
                            obs.state == ChildState::Serving && obs.build != curNow) {
                            s = sup.child_.drainToExit();
                            if (!s.is_ok()) {
                                outcome = s;
                                next = Step::Unlock;
                            } else {
                                next = Step::Reap;
                            }
                            break;
                        }
                    }
                    if (!sup.handoverGate_.isLockedByOther() && obs.state == ChildState::Serving && scrashes > 0 &&
                        now() - servingSince >= sup.options_.stable_ms) {
                        scrashes = 0;
                        s = persistDisk(sbuild, 0, sshunned, false, false);
                        if (!s.is_ok()) {
                            outcome = s;
                            next = Step::Unlock;
                            break;
                        }
                        continue;
                    }
                    pause(sup.options_.poll_ms);
                }
                break;
            }

            case Step::Reap: {
                at("reap");
                const int code = sup.child_.consumeExit();
                if (code == 0) {
                    if (fault(Fault::StopRestarts)) {
                        s = persistDisk(sbuild, 0, sshunned, false, false);
                        if (!s.is_ok()) {
                            outcome = s;
                            next = Step::Unlock;
                        } else {
                            next = Step::Check;
                        }
                    } else {
                        scrashes = 0;
                        s = persistDisk(sbuild, 0, sshunned, false, true);
                        if (!s.is_ok()) {
                            outcome = s;
                        } else {
                            stoppedOutcome = true;
                            haltedOutcome = false;
                            markStopped(true);
                            markHalted(false);
                            r.out.push_back("stopped");
                        }
                        next = Step::Unlock;
                    }
                    break;
                }
                if (code == kDrainExit) {
                    if (fault(Fault::DrainStops)) {
                        s = persistDisk(sbuild, 0, sshunned, false, true);
                        if (!s.is_ok()) {
                            outcome = s;
                        } else {
                            stoppedOutcome = true;
                            markStopped(true);
                        }
                        next = Step::Unlock;
                    } else {
                        scrashes = 0;
                        s = persistDisk(sbuild, 0, sshunned, false, false);
                        if (!s.is_ok()) {
                            outcome = s;
                            next = Step::Unlock;
                        } else {
                            next = Step::Check;
                        }
                    }
                    break;
                }

                const std::uint32_t rawC = scrashes + 1;
                const std::uint32_t nextC =
                    (fault(Fault::NoHaltBound) && rawC > sup.options_.max_restarts) ? (sup.options_.max_restarts + 1)
                                                                                    : rawC;
                scrashes = nextC;

                std::string curNow;
                std::string prevNow;
                builds::Result br;
                if (sup.placement_.active(&curNow, &prevNow, br) != builds::kExitOk) {
                    failWithBuilds(br);
                    break;
                }

                const bool eligRb = !fault(Fault::NoPostStartRollback) &&
                                    nextC >= sup.options_.max_crashes_before_rollback && !prevNow.empty() &&
                                    (fault(Fault::PingPongRollback) || sshunned.count(prevNow) == 0);
                const bool mustHalt = !fault(Fault::NoHaltBound) && nextC >= sup.options_.max_restarts;

                if (eligRb) {
                    s = persistDisk(sbuild, nextC, sshunned, false, false);
                    if (!s.is_ok()) {
                        outcome = s;
                        next = Step::Unlock;
                    } else {
                        next = Step::RbLock;
                    }
                } else if (mustHalt) {
                    s = persistDisk(sbuild, nextC, sshunned, true, false);
                    if (!s.is_ok()) {
                        outcome = s;
                    } else {
                        haltedOutcome = true;
                        markHalted(true);
                        r.err.push_back("build " + sbuild + " crashed " + std::to_string(nextC) +
                                        " times with no eligible rollback build; halting supervision");
                    }
                    next = Step::Unlock;
                } else {
                    s = persistDisk(sbuild, nextC, sshunned, false, false);
                    if (!s.is_ok()) {
                        outcome = s;
                        next = Step::Unlock;
                    } else {
                        next = Step::Backoff;
                    }
                }
                break;
            }

            case Step::Backoff: {
                at("backoff");
                pause(sup.options_.backoff_ms);
                next = Step::Check;
                break;
            }

            case Step::RbLock: {
                const std::uint64_t rbBegan = now();
                for (;;) {
                    at("rb_lock");
                    if (stopRequested()) {
                        stoppedOutcome = true;
                        next = Step::Unlock;
                        break;
                    }
                    if (fault(Fault::NoHandoverLock)) {
                        next = Step::Rollback;
                        break;
                    }
                    s = sup.handoverGate_.tryLock(rbHold);
                    if (s.is_ok()) {
                        sup.rbLocked_ = true;
                        next = Step::Rollback;
                        break;
                    }
                    if (s.code() != Status::Code::Busy) {
                        outcome = s;
                        next = Step::Unlock;
                        break;
                    }
                    if (now() - rbBegan >= handover::kLockWaitMs) {
                        outcome = Status::busy("another handover held the lock while supervisor waited to roll back");
                        next = Step::Unlock;
                        break;
                    }
                    pause(kLockRetryMs);
                }
                break;
            }

            case Step::Rollback: {
                at("rollback");
                s = sup.handoverGate_.resumeIfNeeded(!fault(Fault::NoHandoverLock));
                if (!s.is_ok()) {
                    outcome = s;
                    next = Step::RbUnlock;
                    break;
                }
                if (const pid_t rpid = sup.handoverGate_.takeResumedPid(); rpid > 0) {
                    std::string rcur;
                    std::string rprev;
                    builds::Result rbr;
                    if (sup.placement_.active(&rcur, &rprev, rbr) == builds::kExitOk && !rcur.empty()) {
                        sup.child_.adoptSpawned(rpid, rcur);
                    }
                }

                std::string curNow;
                std::string prevNow;
                builds::Result br;
                if (sup.placement_.active(&curNow, &prevNow, br) != builds::kExitOk) {
                    for (const std::string& e : br.err) {
                        r.err.push_back(e);
                    }
                    outcome = Status::failed(br.err.empty() ? "builds error" : br.err.front());
                    next = Step::RbUnlock;
                    break;
                }

                const bool eligible =
                    !prevNow.empty() && (fault(Fault::PingPongRollback) || sshunned.count(prevNow) == 0);

                std::set<std::string> newShun = sshunned;
                if (!fault(Fault::PingPongRollback) && !curNow.empty()) {
                    newShun.insert(curNow);
                }

                if (eligible) {
                    builds::Result mr;
                    const int mrc = sup.placement_.rollback(prevNow, mr);
                    if (mrc == builds::kExitOk) {
                        r.out.push_back("rolled back from " + curNow + " to " + prevNow);
                        sbuild = prevNow;
                        scrashes = 0;
                        sshunned = newShun;
                        s = persistDisk(prevNow, 0, newShun, false, false);
                        if (!s.is_ok()) {
                            outcome = s;
                        }
                        next = Step::RbUnlock;
                    } else if (mrc == builds::kExitRefused) {
                        for (const std::string& e : mr.err) {
                            r.err.push_back(e);
                        }
                        if (!fault(Fault::SpinOnFormatRefusal)) {
                            sshunned.insert(prevNow);
                        }
                        if (!fault(Fault::NoHaltBound) && scrashes >= sup.options_.max_restarts) {
                            s = persistDisk(sbuild, scrashes, sshunned, true, false);
                            if (!s.is_ok()) {
                                outcome = s;
                            } else {
                                haltedOutcome = true;
                                markHalted(true);
                                r.err.push_back("cannot roll back from " + curNow + "; halting supervision");
                            }
                        } else {
                            s = persistDisk(sbuild, scrashes, sshunned, false, false);
                            if (!s.is_ok()) {
                                outcome = s;
                            }
                        }
                        next = Step::RbUnlock;
                    } else {
                        for (const std::string& e : mr.err) {
                            r.err.push_back(e);
                        }
                        outcome = Status::failed(mr.err.empty() ? "cannot switch to previous build" : mr.err.front());
                        next = Step::RbUnlock;
                    }
                } else if (!fault(Fault::NoHaltBound) && scrashes >= sup.options_.max_restarts) {
                    s = persistDisk(sbuild, scrashes, sshunned, true, false);
                    if (!s.is_ok()) {
                        outcome = s;
                    } else {
                        haltedOutcome = true;
                        markHalted(true);
                        r.err.push_back("cannot roll back from " + curNow + "; halting supervision");
                    }
                    next = Step::RbUnlock;
                } else {
                    next = Step::RbUnlock;
                }
                break;
            }

            case Step::RbUnlock: {
                at("rb_unlock");
                rbHold.reset();
                sup.rbLocked_ = false;
                if (!outcome.is_ok() || haltedOutcome) {
                    next = Step::Unlock;
                } else {
                    next = Step::Check;
                }
                break;
            }

            case Step::Unlock:
                break;
            }
        }

        if (!outcome.is_ok()) {
            return finish(outcome);
        }
        if (stoppedOutcome) {
            return finish(Status::ok(), true, kExitOk);
        }
        if (refusedOutcome) {
            return finish(Status::ok(), true, sup.options_.launchd_mode ? kExitOk : kExitRefused);
        }
        if (haltedOutcome) {
            return finish(Status::ok(), true, sup.options_.launchd_mode ? kExitOk : kExitHalted);
        }
        return finish(Status::ok());
    }
};

SupervisorLoop::SupervisorLoop(store::Fs& fs, handover::PlacementPort& placement, HandoverGate& handoverGate,
                               ChildPort& child, std::string superviseDir, Options options)
    : fs_(fs),
      placement_(placement),
      handoverGate_(handoverGate),
      child_(child),
      superviseDir_(std::move(superviseDir)),
      options_(std::move(options)) {}

SupervisorLoop::~SupervisorLoop() = default;

int SupervisorLoop::run(Result& r) {
    Run runner(*this, r);
    return runner.execute();
}

int SupervisorLoop::status(Result& r) {
    r = Result{};
    std::string cur;
    std::string prev;
    builds::Result br;
    if (placement_.active(&cur, &prev, br) != builds::kExitOk) {
        r.err = br.err;
        r.exit = br.exit;
        return r.exit;
    }

    ChildObs obs;
    const Status cs = child_.inspect(&obs);
    if (!cs.is_ok()) {
        r.err.push_back(cs.message());
        r.exit = exitOf(cs);
        return r.exit;
    }

    std::unique_ptr<store::FileLock> sharedHold;
    const Status ls = fs_.lock(superviseDir_, store::LockMode::Shared, sharedHold);
    const bool lockBusy = (ls.code() == Status::Code::Busy || ls.code() == Status::Code::Refused);
    if (!ls.is_ok() && ls.code() != Status::Code::NotFound && !lockBusy) {
        r.err.push_back(ls.message());
        r.exit = exitOf(ls);
        return r.exit;
    }

    SuperviseState disk;
    const Status rs = loadEffectiveState(fs_, superviseDir_, &disk);
    if (!rs.is_ok()) {
        if (!lockBusy) {
            r.err.push_back(rs.message());
            r.exit = exitOf(rs);
            return r.exit;
        }
        disk = SuperviseState{};
        if (hasStoppedMarker(fs_, superviseDir_)) {
            disk.stopped = true;
        }
    }

    std::string supState = "idle";
    if (lockBusy) {
        supState = "running";
    } else if (disk.stopped) {
        supState = "stopped";
    } else if (disk.halted) {
        supState = "halted";
    }

    r.out.push_back("supervise " + supState + " build " + (disk.build.empty() ? std::string("none") : disk.build) +
                    " crashes " + std::to_string(disk.crashes) + " shunned " + std::to_string(disk.shunned.size()));
    r.out.push_back("current " + (cur.empty() ? std::string("none") : cur) + " previous " +
                    (prev.empty() ? std::string("none") : prev));
    if (obs.state == ChildState::Serving) {
        r.out.push_back("server serving " + (obs.build.empty() ? std::string("none") : obs.build));
    } else {
        r.out.push_back("server stopped");
    }
    r.exit = kExitOk;
    return r.exit;
}

std::unique_ptr<HandoverGate> makePosixHandoverGate(const std::string& serverDir,
                                                    const std::function<std::uint64_t()>& now_ms,
                                                    const std::function<void(std::uint64_t)>& sleep_ms) {
    return std::make_unique<PosixHandoverGate>(serverDir, now_ms, sleep_ms);
}

std::unique_ptr<ChildPort> makePosixChildPort(const std::string& serverDir,
                                              const std::function<std::uint64_t()>& now_ms,
                                              const std::function<void(std::uint64_t)>& sleep_ms) {
    return std::make_unique<PosixChildPort>(serverDir, now_ms, sleep_ms);
}

std::string help(const std::string& verb) {
    if (verb.empty() || verb == "help") {
        return "nexus-server supervise: keep the current placed build serving, restarting and rolling back\n"
               "  supervise [--start] [--launchd]\n"
               "                        run the supervisor loop under DIR/supervise (--start clears stopped/halted)\n"
               "  supervise status      print DIR/supervise/state, current/previous, and the running server\n"
               "  supervise unit [--kind systemd|launchd] [--home HOME] [--path PATH]\n"
               "                        print the deterministic systemd user unit or launchd LaunchAgent plist\n"
               "  supervise unit-path [--kind systemd|launchd] [--home HOME]\n"
               "                        print the unit file path, unit/label name, program path and log path\n"
               "  supervise help [<verb>]\n"
               "exit statuses: 0 stopped cleanly, 1 failed, 2 usage, 3 refused, 4 busy, 78 halted\n";
    }
    if (verb == "unit" || verb == "unit-path") {
        return "nexus-server supervise unit [--kind systemd|launchd] [--home HOME] [--path PATH] [--dir DIR]\n"
               "  Generates the OS service file pointing at DIR/current/bin/nexus-server supervise --dir DIR,\n"
               "  without invoking systemctl or launchctl.\n";
    }
    if (verb == "status") {
        return "nexus-server supervise status [--dir DIR]\n"
               "  Reads DIR/supervise/state, DIR/builds/active and DIR/server.sock without changing anything.\n";
    }
    return std::string();
}

int command(const std::string& serverDir, const std::string& home, const std::vector<std::string>& args,
            const Options& options, Result& r) {
    r = Result{};
    ::signal(SIGPIPE, SIG_IGN);

    if (!args.empty() && args[0] == "help") {
        if (args.size() > 2) {
            r.err.push_back("usage: nexus-server supervise help [<verb>]");
            r.exit = kExitUsage;
            return r.exit;
        }
        const std::string text = help(args.size() == 2 ? args[1] : std::string());
        if (text.empty()) {
            r.err.push_back("unknown supervise verb `" + args[1] + "` (`nexus-server supervise help`)");
            r.exit = kExitUsage;
            return r.exit;
        }
        std::istringstream in(text);
        std::string line;
        while (std::getline(in, line)) {
            r.out.push_back(line);
        }
        r.exit = kExitOk;
        return r.exit;
    }

    if (!args.empty() && (args[0] == "unit" || args[0] == "unit-path")) {
        const bool pathsOnly = (args[0] == "unit-path");
#if defined(__APPLE__)
        std::string kind = "launchd";
#else
        std::string kind = "systemd";
#endif
        std::string useHome = home;
        const char* envPath = std::getenv("PATH");
        std::string usePath = (envPath && *envPath) ? std::string(envPath) : "/usr/local/bin:/usr/bin:/bin";
        for (std::size_t i = 1; i < args.size(); ++i) {
            if (args[i] == "--kind" && i + 1 < args.size()) {
                kind = args[++i];
            } else if (args[i] == "--home" && i + 1 < args.size()) {
                useHome = args[++i];
            } else if (!pathsOnly && args[i] == "--path" && i + 1 < args.size()) {
                usePath = args[++i];
            } else {
                r.err.push_back("usage: nexus-server supervise " + args[0] +
                                " [--kind systemd|launchd] [--home HOME]" +
                                (pathsOnly ? "" : " [--path PATH]") + " [--dir DIR]");
                r.exit = kExitUsage;
                return r.exit;
            }
        }
        if (kind != "systemd" && kind != "launchd") {
            r.err.push_back("unknown service unit kind `" + kind + "` (expected `systemd` or `launchd`)");
            r.exit = kExitUsage;
            return r.exit;
        }
        if (useHome.empty()) {
            r.err.push_back("HOME is empty; pass `--home <dir>` or set HOME");
            r.exit = kExitFailed;
            return r.exit;
        }
        const std::string defaultDir = trimTrailingSlash(useHome) + "/.nexus/server";
        const ServiceUnitPaths up = serviceUnitPaths(serverDir, useHome, defaultDir);
        if (pathsOnly) {
            if (kind == "launchd") {
                r.out.push_back(up.launchdPlistPath);
                r.out.push_back(up.launchdLabel);
            } else {
                r.out.push_back(up.systemdUnitPath);
                r.out.push_back(up.systemdUnitName + ".service");
            }
            r.out.push_back(up.programPath);
            r.out.push_back(up.logPath);
            r.exit = kExitOk;
            return r.exit;
        }
        const std::string rendered =
            (kind == "launchd")
                ? launchdPlist(up.launchdLabel, up.programPath, trimTrailingSlash(serverDir), up.logPath, useHome,
                               usePath)
                : systemdUnit(up.programPath, trimTrailingSlash(serverDir), useHome, usePath);
        std::istringstream in(rendered);
        std::string line;
        while (std::getline(in, line)) {
            r.out.push_back(line);
        }
        r.exit = kExitOk;
        return r.exit;
    }

    bool statusOnly = false;
    Options runOpts = options;
    if (args.size() == 1 && args[0] == "status") {
        statusOnly = true;
    } else {
        for (const std::string& arg : args) {
            if (arg == "--start") {
                runOpts.clear_stopped_on_start = true;
            } else if (arg == "--launchd") {
                runOpts.launchd_mode = true;
            } else {
                r.err.push_back(
                    "usage: nexus-server supervise [--start] [--launchd] | status | unit ... | help [--dir DIR]");
                r.exit = kExitUsage;
                return r.exit;
            }
        }
    }

    store::PosixOptions fsOpts;
    fsOpts.now_ms = runOpts.now_ms;
    fsOpts.sleep_ms = runOpts.sleep_ms;
    fsOpts.test_fs_type = runOpts.test_fs_type;
    fsOpts.test_locks_ignored = runOpts.test_locks_ignored;
    std::unique_ptr<store::Fs> fs = store::make_posix_fs(fsOpts);

    if (!statusOnly && !home.empty()) {
        const std::string top = trimTrailingSlash(home) + "/.nexus";
        const std::string sdir = trimTrailingSlash(serverDir);
        ServerSeen liveSeen;
        const bool newServerServing = probeControlSocket(sdir + "/server.sock", &liveSeen).is_ok() && liveSeen.serving;
        if (sdir == top + "/server" && !newServerServing) {
            migration::Options mopts;
            mopts.old_server_live = [&] { return migration::probeOldServerSocket(top + "/server.sock"); };
            migration::Report rep;
            const Status ms = migration::migrateOldLayout(*fs, top, sdir, mopts, &rep);
            if (!ms.is_ok()) {
                r.err.push_back(ms.message());
                if (rep.conflict) {
                    r.exit = runOpts.launchd_mode ? kExitOk : kExitRefused;
                } else if (ms.code() == Status::Code::Busy) {
                    r.exit = kExitBusy;
                } else {
                    r.exit = kExitFailed;
                }
                return r.exit;
            }
        }
    }

    builds::Options bOpts;
    bOpts.now_ms = runOpts.now_ms;
    bOpts.sleep_ms = runOpts.sleep_ms;
    bOpts.lock_wait_ms = runOpts.lock_wait_ms;
    bOpts.test_fs_type = runOpts.test_fs_type;
    bOpts.test_locks_ignored = runOpts.test_locks_ignored;
    std::unique_ptr<handover::PlacementPort> placement = handover::makeBuildsPlacement(serverDir, bOpts);
    std::unique_ptr<HandoverGate> gate = makePosixHandoverGate(serverDir, runOpts.now_ms, runOpts.sleep_ms);
    std::unique_ptr<ChildPort> child = makePosixChildPort(serverDir, runOpts.now_ms, runOpts.sleep_ms);

    if (!statusOnly && !runOpts.stop_requested) {
        gSuperviseStop = 0;
        struct sigaction sa{};
        sa.sa_handler = onSuperviseSignal;
        ::sigaction(SIGTERM, &sa, nullptr);
        ::sigaction(SIGINT, &sa, nullptr);
        runOpts.stop_requested = [] { return gSuperviseStop != 0; };
    }

    SupervisorLoop loop(*fs, *placement, *gate, *child, serverDir + "/" + kSuperviseDir, runOpts);
    if (statusOnly) {
        return loop.status(r);
    }
    return loop.run(r);
}

} // namespace nexus::server::supervision
