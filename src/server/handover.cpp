// Handover (server/handover.h): durable, resumable handover state machine under an OS lock
// (models/handover/Handover.tla).

#include "server/handover.h"
#include "server/migration.h"
#include "server/supervise_state.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sstream>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

extern char** environ;

namespace nexus::server::handover {

using store::Status;

namespace {

constexpr const char* kStateHeader = "nexus-handover 1";
constexpr int kSocketTimeoutMs = 4000;
constexpr std::size_t kMaxLineBytes = 4096;

std::uint64_t steadyMs() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}

void realSleep(std::uint64_t ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

std::string uniqueSuffix() {
    static std::atomic<std::uint64_t> counter{0};
    return std::to_string(::getpid()) + "-" + std::to_string(++counter);
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

store::Bytes toBytes(const std::string& s) {
    return store::Bytes(reinterpret_cast<const std::uint8_t*>(s.data()),
                        reinterpret_cast<const std::uint8_t*>(s.data()) + s.size());
}

std::string fromBytes(const store::Bytes& b) {
    return std::string(reinterpret_cast<const char*>(b.data()), b.size());
}

std::string siblingSuperviseDir(const std::string& handoverDir) {
    const std::size_t slash = handoverDir.rfind('/');
    if (slash == std::string::npos) {
        return supervision::kSuperviseDir;
    }
    return handoverDir.substr(0, slash + 1) + supervision::kSuperviseDir;
}

class BuildsPlacement final : public PlacementPort {
public:
    BuildsPlacement(std::string serverDir, builds::Options options)
        : serverDir_(std::move(serverDir)), options_(std::move(options)) {}

    int active(std::string* current, std::string* previous, builds::Result& r) override {
        builds::Builds b(serverDir_, options_);
        return b.active(current, previous, r);
    }

    int checkSwitch(const std::string& hash, builds::Result& r, bool allowUntested) override {
        builds::Builds b(serverDir_, options_);
        return b.checkSwitch(hash, r, allowUntested);
    }

    int makeCurrent(const std::string& hash, builds::Result& r, bool allowUntested) override {
        builds::Builds b(serverDir_, options_);
        return b.makeCurrent(hash, r, allowUntested);
    }

    int rollback(const std::string& hash, builds::Result& r) override {
        builds::Builds b(serverDir_, options_);
        return b.rollbackTo(hash, r);
    }

private:
    std::string serverDir_;
    builds::Options options_;
};

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

class PosixServerPort final : public ServerPort {
public:
    PosixServerPort(std::string serverDir, std::function<std::uint64_t()> now_ms,
                    std::function<void(std::uint64_t)> sleep_ms, pid_t* adopted_pid_out)
        : serverDir_(std::move(serverDir)),
          socketPath_(serverDir_ + "/server.sock"),
          logPath_(serverDir_ + "/server.log"),
          programPath_(serverDir_ + "/" + builds::kStable + "/" + builds::kProgram),
          now_(now_ms ? std::move(now_ms) : std::function<std::uint64_t()>(steadyMs)),
          sleep_(sleep_ms ? std::move(sleep_ms) : std::function<void(std::uint64_t)>(realSleep)),
          adoptedOut_(adopted_pid_out) {}

    ~PosixServerPort() override {
        abortStarted();
    }

    Status probe(ServerSeen* out) override {
        *out = ServerSeen{};
        sockaddr_un addr;
        if (!socketAddress(socketPath_, &addr)) {
            return Status::failed(socketPath_ + " is longer than a Unix socket's address holds");
        }
        const int fd = connectUnix(socketPath_);
        if (fd < 0) {
            if (errno == ENOENT || errno == ECONNREFUSED || errno == ECONNRESET) {
                return Status::ok();
            }
            return Status::failed("connect " + socketPath_ + ": " + std::strerror(errno));
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
        if (!ok) {
            // A server closing its socket (stopping or ended) drops queued connections unanswered.
            return Status::ok();
        }
        if (first != "SERVING") {
            return Status::ok();
        }
        if (!parseStatusLines(first, second, hasSecond, out)) {
            return Status::failed("the server on " + socketPath_ + " answered `status` with malformed build line `" +
                                  second + "`");
        }
        return Status::ok();
    }

    DrainReply requestDrain(std::string* why) override {
        const int fd = connectUnix(socketPath_);
        if (fd < 0) {
            if (errno == ENOENT || errno == ECONNREFUSED || errno == ECONNRESET) {
                return DrainReply::Closed;
            }
            *why = std::string("connect ") + socketPath_ + ": " + std::strerror(errno);
            return DrainReply::Failed;
        }
        LineReader reader;
        std::string reply;
        const bool ok = sendAll(fd, "drain\n") && reader.readLine(fd, &reply, kSocketTimeoutMs);
        ::close(fd);
        if (!ok) {
            return DrainReply::Closed;
        }
        if (reply == "OK") {
            return DrainReply::Accepted;
        }
        if (reply.rfind("ERROR unknown request;", 0) == 0) {
            return DrainReply::UnknownRequest;
        }
        *why = "the server on " + socketPath_ + " answered `drain` with: " + reply;
        return DrainReply::Failed;
    }

    Status stop() override {
        const int fd = connectUnix(socketPath_);
        if (fd < 0) {
            if (errno == ENOENT || errno == ECONNREFUSED || errno == ECONNRESET) {
                return Status::ok();
            }
            return Status::failed("connect " + socketPath_ + ": " + std::strerror(errno));
        }
        LineReader reader;
        std::string reply;
        const bool ok = sendAll(fd, "stop\n") && reader.readLine(fd, &reply, kSocketTimeoutMs);
        ::close(fd);
        if (ok && reply != "OK") {
            return Status::failed("the server on " + socketPath_ + " answered `stop` with: " + reply);
        }
        const std::uint64_t began = now_();
        while (now_() - began < 5000) {
            ServerSeen seen;
            const Status ps = probe(&seen);
            if (!ps.is_ok()) {
                return ps;
            }
            if (!seen.serving && ::access(socketPath_.c_str(), F_OK) != 0) {
                return Status::ok();
            }
            sleep_(kPollMs);
        }
        ServerSeen still;
        if (probe(&still).is_ok() && still.serving) {
            return Status::failed("the server on " + socketPath_ + " did not stop within 5 s");
        }
        return Status::ok();
    }

    StartReply stepStart(const std::string& expectedBuild, std::uint64_t elapsed_ms, std::uint64_t limit_ms,
                         std::string* why) override {
        if (!spawned_) {
            std::string spawnWhy;
            if (!spawnServer(&spawnWhy)) {
                *why = spawnWhy;
                return StartReply::Failed;
            }
            spawned_ = true;
        }
        if (child_ > 0) {
            int status = 0;
            const pid_t done = ::waitpid(child_, &status, WNOHANG);
            if (done == child_) {
                child_ = -1;
                // Exit 0 on `serve` means another server already answered SERVING (start_order.h kRefusedExit); the
                // next probe checks that server's build. Any other exit means the started build failed before serving.
                const bool anotherServing = WIFEXITED(status) && WEXITSTATUS(status) == 0;
                if (!anotherServing) {
                    *why = "build " + expectedBuild + " exited " +
                           (WIFEXITED(status) ? "with code " + std::to_string(WEXITSTATUS(status))
                                              : "on signal " + std::to_string(WTERMSIG(status))) +
                           " without serving; see " + logPath_;
                    return StartReply::Failed;
                }
            }
        }
        if (elapsed_ms >= limit_ms) {
            *why = "build " + expectedBuild + " did not answer SERVING on " + socketPath_ + " within " +
                   std::to_string(limit_ms) + " ms; see " + logPath_;
            return StartReply::Failed;
        }
        return StartReply::Waiting;
    }

    void keepStarted() override {
        if (adoptedOut_ != nullptr && child_ > 0) {
            *adoptedOut_ = child_;
        }
        child_ = -1;
        spawned_ = false;
    }

    void abortStarted() override {
        if (child_ > 0) {
            int status = 0;
            if (::waitpid(child_, &status, WNOHANG) == 0) {
                ::kill(-child_, SIGKILL);
                ::kill(child_, SIGKILL);
                while (::waitpid(child_, &status, 0) < 0 && errno == EINTR) {
                }
            }
            child_ = -1;
        }
        spawned_ = false;
    }

private:
    bool spawnServer(std::string* why) {
        posix_spawn_file_actions_t files;
        posix_spawn_file_actions_init(&files);
        posix_spawn_file_actions_addopen(&files, 0, "/dev/null", O_RDONLY, 0);
        posix_spawn_file_actions_addopen(&files, 1, logPath_.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0600);
        posix_spawn_file_actions_adddup2(&files, 1, 2);
        posix_spawnattr_t attr;
        posix_spawnattr_init(&attr);
#if defined(__APPLE__)
        posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSID | POSIX_SPAWN_CLOEXEC_DEFAULT);
#else
        posix_spawn_file_actions_addclosefrom_np(&files, 3);
        posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSID);
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
        child_ = pid;
        return true;
    }

    std::string serverDir_;
    std::string socketPath_;
    std::string logPath_;
    std::string programPath_;
    std::function<std::uint64_t()> now_;
    std::function<void(std::uint64_t)> sleep_;
    pid_t* adoptedOut_ = nullptr;
    pid_t child_ = -1;
    bool spawned_ = false;
};

} // namespace

const char* stageName(Stage s) {
    switch (s) {
    case Stage::Switching:
        return "switching";
    case Stage::Draining:
        return "draining";
    case Stage::Starting:
        return "starting";
    case Stage::RollingBack:
        return "rolling-back";
    }
    return "switching";
}

bool parseStage(const std::string& text, Stage* out) {
    if (text == "switching") {
        *out = Stage::Switching;
        return true;
    }
    if (text == "draining") {
        *out = Stage::Draining;
        return true;
    }
    if (text == "starting") {
        *out = Stage::Starting;
        return true;
    }
    if (text == "rolling-back") {
        *out = Stage::RollingBack;
        return true;
    }
    return false;
}

std::string formatState(const StateRecord& rec) {
    return std::string(kStateHeader) + "\n" +
           "target " + rec.target + "\n" +
           "previous " + (rec.previous.empty() ? std::string("none") : rec.previous) + "\n" +
           "stage " + stageName(rec.stage) + "\n" +
           "end\n";
}

bool readState(const std::string& text, StateRecord* out, std::string* why) {
    *out = StateRecord{};
    std::istringstream in(text);
    std::string line;
    if (!std::getline(in, line)) {
        *why = "the handover state file is empty";
        return false;
    }
    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }
    if (line != kStateHeader) {
        *why = "expected header `" + std::string(kStateHeader) + "`, found `" + line + "`";
        return false;
    }
    bool haveTarget = false;
    bool havePrevious = false;
    bool haveStage = false;
    bool haveEnd = false;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (haveEnd) {
            if (!line.empty()) {
                *why = "unexpected line after `end`";
                return false;
            }
            continue;
        }
        if (line == "end") {
            haveEnd = true;
            continue;
        }
        if (line.rfind("target ", 0) == 0) {
            if (haveTarget) {
                *why = "duplicate `target` line";
                return false;
            }
            const std::string h = line.substr(7);
            if (!builds::isHash(h)) {
                *why = "invalid target hash `" + h + "`";
                return false;
            }
            out->target = h;
            haveTarget = true;
        } else if (line.rfind("previous ", 0) == 0) {
            if (havePrevious) {
                *why = "duplicate `previous` line";
                return false;
            }
            const std::string p = line.substr(9);
            if (p != "none" && !builds::isHash(p)) {
                *why = "invalid previous hash `" + p + "`";
                return false;
            }
            out->previous = (p == "none") ? std::string() : p;
            havePrevious = true;
        } else if (line.rfind("stage ", 0) == 0) {
            if (haveStage) {
                *why = "duplicate `stage` line";
                return false;
            }
            const std::string s = line.substr(6);
            if (!parseStage(s, &out->stage)) {
                *why = "unknown stage `" + s + "`";
                return false;
            }
            haveStage = true;
        } else {
            *why = "unknown line `" + line + "`";
            return false;
        }
    }
    if (!haveEnd) {
        *why = "missing `end` line (torn write)";
        return false;
    }
    if (!haveTarget || !havePrevious || !haveStage) {
        *why = "missing required field (`target`, `previous` or `stage`)";
        return false;
    }
    return true;
}

struct Handover::Run {
    Handover& h;
    Result& r;
    std::string statePath;
    std::unique_ptr<store::FileLock> hold;
    bool pastLock = false;

    Run(Handover& handover, Result& result)
        : h(handover), r(result), statePath(handover.handoverDir_ + "/" + kStateFile) {}

    ~Run() {
        h.server_.abortStarted();
        hold.reset();
        h.locked_ = false;
        h.phase_ = "idle";
    }

    bool fault(Fault f) const { return h.options_.fault == f; }
    std::uint64_t now() const { return h.options_.now_ms ? h.options_.now_ms() : steadyMs(); }
    void pause(std::uint64_t ms) const {
        if (h.options_.sleep_ms) {
            h.options_.sleep_ms(ms);
        } else {
            realSleep(ms);
        }
    }

    void at(const char* step) {
        h.phase_ = step;
        if (h.options_.hook) {
            h.options_.hook(step);
        }
    }

    int finish(const Status& s, bool explicitExit = false, int code = kExitOk) {
        if (pastLock) {
            at("unlock");
            hold.reset();
            h.locked_ = false;
            pastLock = false;
        }
        h.phase_ = "idle";
        if (h.options_.hook) {
            h.options_.hook("idle");
        }
        if (explicitExit) {
            if (!s.is_ok()) {
                r.err.push_back(s.message());
            }
            r.exit = code;
            return r.exit;
        }
        if (!s.is_ok()) {
            r.err.push_back(s.message());
        }
        r.exit = exitOf(s);
        return r.exit;
    }

    Status writeStage(const char* wpc, Stage st, const char* spc, const std::string& rtgt, const std::string& rprev) {
        at(wpc);
        if (fault(Fault::InMemoryOnly)) {
            return Status::ok();
        }
        const store::Bytes bytes = toBytes(formatState(StateRecord{rtgt, rprev, st}));
        const std::string tmp = h.handoverDir_ + "/" + kStateTmpPrefix + uniqueSuffix();
        Status s;
        if (fault(Fault::NoSync)) {
            s = h.fs_.create(tmp);
            if (s.is_ok()) {
                s = h.fs_.append(tmp, bytes);
            }
        } else {
            s = h.fs_.write_new(tmp, bytes);
        }
        if (!s.is_ok()) {
            (void)h.fs_.remove(tmp);
            return s;
        }
        s = h.fs_.rename(tmp, statePath);
        if (!s.is_ok()) {
            (void)h.fs_.remove(tmp);
            return s;
        }
        if (!fault(Fault::NoSync)) {
            at(spc);
            s = h.fs_.sync_dir(h.handoverDir_);
            if (!s.is_ok()) {
                return s;
            }
        }
        return Status::ok();
    }

    int execute(bool isTo, const std::string& requestedHash) {
        r = Result{};
        if (isTo && !builds::isHash(requestedHash)) {
            return finish(Status::refused("`" + requestedHash +
                                          "` is not a build's hash (64 hex digits; `nexus-server builds list`)"));
        }
        Status s = h.fs_.private_dir(h.handoverDir_);
        if (!s.is_ok()) {
            return finish(s);
        }

        at("lock");
        if (h.options_.lock_already_held) {
            h.locked_ = true;
        } else if (!fault(Fault::NoLock)) {
            const std::uint64_t waitBegan = now();
            for (;;) {
                s = h.fs_.lock(h.handoverDir_, store::LockMode::Exclusive, hold);
                if (s.is_ok()) {
                    h.locked_ = true;
                    break;
                }
                if (s.code() != Status::Code::Busy) {
                    return finish(s);
                }
                if (now() - waitBegan >= h.options_.lock_wait_ms) {
                    return finish(Status::busy("another handover holds the lock on " + h.handoverDir_ +
                                               " and did not finish within " +
                                               std::to_string(h.options_.lock_wait_ms / 1000) +
                                               " s; run `nexus-server handover status` or try again once it ends"));
                }
                pause(kLockRetryMs);
            }
        }
        pastLock = true;

        at("settle");
        s = h.fs_.sync_dir(h.handoverDir_);
        if (!s.is_ok()) {
            return finish(s);
        }
        std::vector<std::string> entries;
        s = h.fs_.list(h.handoverDir_, entries);
        if (!s.is_ok()) {
            return finish(s);
        }
        std::vector<std::string> leftovers;
        for (const std::string& n : entries) {
            if (n.rfind(kStateTmpPrefix, 0) == 0) {
                leftovers.push_back(n);
            }
        }
        std::sort(leftovers.begin(), leftovers.end());
        for (const std::string& n : leftovers) {
            at("clean");
            s = h.fs_.remove(h.handoverDir_ + "/" + n);
            if (!s.is_ok() && s.code() != Status::Code::NotFound) {
                return finish(s);
            }
        }

        enum class Step {
            Inspect,
            WSwitch,
            Switch,
            WDrain,
            Drain,
            WStart,
            Start,
            WRollback,
            Rollback,
            StartRb,
            Clear,
            Unlock,
        };

        Step next = Step::Inspect;
        std::string rtgt;
        std::string rprev;
        bool didOwn = false;
        bool resumingExplicit = false;
        std::string startFailWhy;
        Status outcome = Status::ok();
        bool explicitExit = false;
        int explicitCode = kExitOk;

        auto adoptBuildsFailure = [&](const builds::Result& br) {
            for (const std::string& e : br.err) {
                r.err.push_back(e);
            }
            explicitExit = true;
            explicitCode = br.exit;
            next = Step::Unlock;
        };

        while (next != Step::Unlock) {
            switch (next) {
            case Step::Inspect: {
                at("inspect");
                startFailWhy.clear();
                store::Bytes raw;
                const Status rs = h.fs_.read_all(statePath, raw);
                if (!rs.is_ok() && rs.code() != Status::Code::NotFound) {
                    outcome = rs;
                    next = Step::Unlock;
                    break;
                }
                const bool hasState = rs.is_ok();
                if (hasState && !(fault(Fault::OverwritePrev) && isTo)) {
                    StateRecord rec;
                    std::string why;
                    if (!readState(fromBytes(raw), &rec, &why)) {
                        outcome = Status::failed(statePath + " is damaged: " + why);
                        next = Step::Unlock;
                        break;
                    }
                    rtgt = rec.target;
                    rprev = rec.previous;
                    resumingExplicit = true;
                    didOwn = (!isTo || requestedHash == rec.target);
                    switch (rec.stage) {
                    case Stage::Switching:
                        next = Step::Switch;
                        break;
                    case Stage::Draining:
                        next = Step::Drain;
                        break;
                    case Stage::Starting:
                        next = Step::Start;
                        break;
                    case Stage::RollingBack:
                        next = Step::Rollback;
                        break;
                    }
                    break;
                }

                std::string cur;
                std::string prev;
                builds::Result br;
                if (h.placement_.active(&cur, &prev, br) != builds::kExitOk) {
                    didOwn = true;
                    adoptBuildsFailure(br);
                    break;
                }

                if (!isTo) {
                    didOwn = true;
                    if (cur.empty()) {
                        outcome = Status::refused(
                            "no build is current (`nexus-server builds current <hash>` chooses one)");
                        next = Step::Unlock;
                        break;
                    }
                    ServerSeen seen;
                    const Status ps = h.server_.probe(&seen);
                    if (!ps.is_ok()) {
                        outcome = ps;
                        next = Step::Unlock;
                        break;
                    }
                    if (seen.serving && !seen.legacy && seen.build == cur && !seen.draining) {
                        r.out.push_back("already serving " + cur);
                        next = Step::Unlock;
                        break;
                    }
                    builds::Result cr;
                    if (h.placement_.checkSwitch(cur, cr, fault(Fault::AllowUntested)) != builds::kExitOk) {
                        adoptBuildsFailure(cr);
                        break;
                    }
                    rtgt = cur;
                    rprev.clear();
                    next = seen.serving ? Step::WDrain : Step::WStart;
                    break;
                }

                didOwn = true;
                builds::Result cr;
                if (h.placement_.checkSwitch(requestedHash, cr, fault(Fault::AllowUntested)) != builds::kExitOk) {
                    adoptBuildsFailure(cr);
                    break;
                }
                if (!fault(Fault::InMemoryOnly)) {
                    (void)supervision::noteHandoverBegin(h.fs_, siblingSuperviseDir(h.handoverDir_));
                }
                ServerSeen seen;
                const Status ps = h.server_.probe(&seen);
                if (!ps.is_ok()) {
                    outcome = ps;
                    next = Step::Unlock;
                    break;
                }
                if (cur == requestedHash && seen.serving && !seen.legacy && seen.build == requestedHash &&
                    !seen.draining) {
                    if (!fault(Fault::InMemoryOnly)) {
                        (void)supervision::noteHandoverTarget(h.fs_, siblingSuperviseDir(h.handoverDir_),
                                                              requestedHash);
                    }
                    r.out.push_back("already serving " + requestedHash);
                    next = Step::Unlock;
                    break;
                }
                rtgt = requestedHash;
                rprev = (cur != requestedHash) ? cur : std::string();
                next = Step::WSwitch;
                break;
            }

            case Step::WSwitch: {
                s = writeStage("wswitch", Stage::Switching, "sswitch", rtgt, rprev);
                if (!s.is_ok()) {
                    outcome = s;
                    next = Step::Unlock;
                } else {
                    next = Step::Switch;
                }
                break;
            }

            case Step::Switch: {
                at("switch");
                builds::Result br;
                if (h.placement_.makeCurrent(rtgt, br, fault(Fault::AllowUntested)) != builds::kExitOk) {
                    adoptBuildsFailure(br);
                } else {
                    next = Step::WDrain;
                }
                break;
            }

            case Step::WDrain: {
                s = writeStage("wdrain", Stage::Draining, "sdrain", rtgt, rprev);
                if (!s.is_ok()) {
                    outcome = s;
                    next = Step::Unlock;
                } else {
                    next = Step::Drain;
                }
                break;
            }

            case Step::Drain: {
                const std::uint64_t drainBegan = now();
                for (;;) {
                    at("drain");
                    ServerSeen seen;
                    const Status ps = h.server_.probe(&seen);
                    if (!ps.is_ok()) {
                        outcome = ps;
                        next = Step::Unlock;
                        break;
                    }
                    if (!seen.serving) {
                        next = Step::WStart;
                        break;
                    }
                    if (!seen.legacy && seen.build == rtgt && !seen.draining) {
                        if (didOwn) {
                            if ((isTo || resumingExplicit) && !fault(Fault::InMemoryOnly)) {
                                (void)supervision::noteHandoverTarget(h.fs_, siblingSuperviseDir(h.handoverDir_), rtgt);
                            }
                            r.out.push_back("serving " + rtgt +
                                            (rprev.empty() ? std::string() : " (previous " + rprev + ")"));
                        }
                        next = Step::Clear;
                        break;
                    }
                    if (seen.legacy || fault(Fault::StopCuts)) {
                        const Status ss = h.server_.stop();
                        if (!ss.is_ok()) {
                            outcome = ss;
                            next = Step::Unlock;
                        } else {
                            next = Step::WStart;
                        }
                        break;
                    }
                    if (!seen.draining) {
                        std::string why;
                        const DrainReply dr = h.server_.requestDrain(&why);
                        if (dr == DrainReply::UnknownRequest) {
                            const Status ss = h.server_.stop();
                            if (!ss.is_ok()) {
                                outcome = ss;
                                next = Step::Unlock;
                            } else {
                                next = Step::WStart;
                            }
                            break;
                        }
                        if (dr == DrainReply::Failed) {
                            outcome = Status::failed(why);
                            next = Step::Unlock;
                            break;
                        }
                    }
                    if (now() - drainBegan >= h.options_.drain_wait_ms) {
                        const Status ss = h.server_.stop();
                        if (!ss.is_ok()) {
                            outcome = Status::failed("the draining server did not exit within " +
                                                     std::to_string(h.options_.drain_wait_ms / 1000) + " s: " +
                                                     ss.message());
                            next = Step::Unlock;
                        } else {
                            next = Step::WStart;
                        }
                        break;
                    }
                    pause(h.options_.poll_ms);
                }
                break;
            }

            case Step::WStart: {
                s = writeStage("wstart", Stage::Starting, "sstart", rtgt, rprev);
                if (!s.is_ok()) {
                    outcome = s;
                    next = Step::Unlock;
                } else {
                    next = Step::Start;
                }
                break;
            }

            case Step::Start: {
                h.server_.abortStarted();
                const std::uint64_t startBegan = now();
                for (;;) {
                    at("start");
                    ServerSeen seen;
                    const Status ps = h.server_.probe(&seen);
                    if (!ps.is_ok()) {
                        h.server_.abortStarted();
                        outcome = ps;
                        next = Step::Unlock;
                        break;
                    }
                    if (seen.serving && !seen.legacy && seen.build == rtgt && !seen.draining) {
                        h.server_.keepStarted();
                        if (didOwn) {
                            if ((isTo || resumingExplicit) && !fault(Fault::InMemoryOnly)) {
                                (void)supervision::noteHandoverTarget(h.fs_, siblingSuperviseDir(h.handoverDir_), rtgt);
                            }
                            r.out.push_back("serving " + rtgt +
                                            (rprev.empty() ? std::string() : " (previous " + rprev + ")"));
                        }
                        next = Step::Clear;
                        break;
                    }
                    if (seen.serving) {
                        h.server_.abortStarted();
                        next = Step::WDrain;
                        break;
                    }
                    std::string why;
                    const StartReply sr =
                        h.server_.stepStart(rtgt, now() - startBegan, h.options_.start_wait_ms, &why);
                    if (sr == StartReply::Failed) {
                        h.server_.abortStarted();
                        startFailWhy = why;
                        if (fault(Fault::NoRollback) || rprev.empty()) {
                            if (didOwn) {
                                outcome = Status::failed(
                                    why + (rprev.empty() ? " (there is no previous build to roll back to)" : ""));
                            }
                            next = Step::Clear;
                        } else {
                            next = Step::WRollback;
                        }
                        break;
                    }
                    pause(h.options_.poll_ms);
                }
                break;
            }

            case Step::WRollback: {
                s = writeStage("wrollback", Stage::RollingBack, "srollback", rtgt, rprev);
                if (!s.is_ok()) {
                    outcome = s;
                    next = Step::Unlock;
                } else {
                    next = Step::Rollback;
                }
                break;
            }

            case Step::Rollback: {
                at("rollback");
                h.server_.abortStarted();
                ServerSeen seen;
                if (h.server_.probe(&seen).is_ok() && seen.serving &&
                    (seen.legacy || seen.build != rprev || seen.draining)) {
                    (void)h.server_.stop();
                }
                builds::Result br;
                const int rbRc = h.placement_.rollback(rprev, br);
                if (rbRc == builds::kExitOk) {
                    next = Step::StartRb;
                } else if (rbRc == builds::kExitRefused) {
                    for (const std::string& e : br.err) {
                        r.err.push_back(e);
                    }
                    explicitExit = true;
                    explicitCode = kExitRefused;
                    next = Step::Clear;
                } else {
                    adoptBuildsFailure(br);
                }
                break;
            }

            case Step::StartRb: {
                h.server_.abortStarted();
                const std::uint64_t rbBegan = now();
                for (;;) {
                    at("start_rb");
                    ServerSeen seen;
                    const Status ps = h.server_.probe(&seen);
                    if (!ps.is_ok()) {
                        h.server_.abortStarted();
                        outcome = ps;
                        next = Step::Unlock;
                        break;
                    }
                    if (seen.serving && !seen.legacy && seen.build == rprev && !seen.draining) {
                        h.server_.keepStarted();
                        if (didOwn) {
                            r.out.push_back("rolled back to " + rprev);
                            if (isTo) {
                                r.err.push_back(
                                    (startFailWhy.empty() ? ("build " + rtgt + " did not reach SERVING")
                                                          : startFailWhy) +
                                    "; rolled back to " + rprev + ", which is serving");
                                explicitExit = true;
                                explicitCode = kExitRolledBack;
                            }
                        }
                        next = Step::Clear;
                        break;
                    }
                    if (seen.serving) {
                        h.server_.abortStarted();
                        outcome = Status::failed("while rolling back to " + rprev + ", another build answered on " +
                                                 h.handoverDir_);
                        next = Step::Unlock;
                        break;
                    }
                    std::string why;
                    const StartReply sr =
                        h.server_.stepStart(rprev, now() - rbBegan, h.options_.start_wait_ms, &why);
                    if (sr == StartReply::Failed) {
                        h.server_.abortStarted();
                        outcome = Status::failed((startFailWhy.empty() ? std::string() : startFailWhy + "; and ") +
                                                 "rollback to " + rprev + " failed: " + why);
                        next = Step::Unlock;
                        break;
                    }
                    pause(h.options_.poll_ms);
                }
                break;
            }

            case Step::Clear: {
                at("clear");
                s = h.fs_.remove(statePath);
                if (!s.is_ok() && s.code() != Status::Code::NotFound) {
                    outcome = s;
                    next = Step::Unlock;
                    break;
                }
                if (!fault(Fault::NoSync)) {
                    at("sclear");
                    s = h.fs_.sync_dir(h.handoverDir_);
                    if (!s.is_ok()) {
                        outcome = s;
                        next = Step::Unlock;
                        break;
                    }
                }
                next = didOwn ? Step::Unlock : Step::Inspect;
                break;
            }

            case Step::Unlock:
                break;
            }
        }

        return finish(outcome, explicitExit, explicitCode);
    }
};

Handover::Handover(store::Fs& fs, PlacementPort& placement, ServerPort& server, std::string handoverDir,
                   Options options)
    : fs_(fs),
      placement_(placement),
      server_(server),
      handoverDir_(std::move(handoverDir)),
      options_(std::move(options)) {}

Handover::~Handover() = default;

int Handover::to(const std::string& hash, Result& r) {
    Run run(*this, r);
    return run.execute(true, hash);
}

int Handover::resume(Result& r) {
    Run run(*this, r);
    return run.execute(false, std::string());
}

int Handover::status(Result& r) {
    r = Result{};
    std::string cur;
    std::string prev;
    builds::Result br;
    if (placement_.active(&cur, &prev, br) != builds::kExitOk) {
        r.err = br.err;
        r.exit = br.exit;
        return r.exit;
    }

    ServerSeen seen;
    const Status ps = server_.probe(&seen);
    if (!ps.is_ok()) {
        r.err.push_back(ps.message());
        r.exit = exitOf(ps);
        return r.exit;
    }

    std::unique_ptr<store::FileLock> sharedHold;
    const Status ls = fs_.lock(handoverDir_, store::LockMode::Shared, sharedHold);
    const bool lockBusy = ls.code() == Status::Code::Busy || ls.code() == Status::Code::Refused;
    if (!ls.is_ok() && ls.code() != Status::Code::NotFound && !lockBusy) {
        r.err.push_back(ls.message());
        r.exit = exitOf(ls);
        return r.exit;
    }

    store::Bytes raw;
    const Status rs = fs_.read_all(handoverDir_ + "/" + kStateFile, raw);
    if (!rs.is_ok() && rs.code() != Status::Code::NotFound) {
        r.err.push_back(rs.message());
        r.exit = exitOf(rs);
        return r.exit;
    }
    if (rs.is_ok()) {
        StateRecord rec;
        std::string why;
        if (!readState(fromBytes(raw), &rec, &why)) {
            if (lockBusy) {
                r.out.push_back("handover running");
            } else {
                r.err.push_back(handoverDir_ + "/" + kStateFile + " is damaged: " + why);
                r.exit = kExitFailed;
                return r.exit;
            }
        } else {
            r.out.push_back(std::string("handover ") + (lockBusy ? "running " : "interrupted ") +
                            stageName(rec.stage) + " target " + rec.target + " previous " +
                            (rec.previous.empty() ? std::string("none") : rec.previous));
        }
    } else if (lockBusy) {
        r.out.push_back("handover running");
    } else {
        r.out.push_back("handover idle");
    }

    r.out.push_back("current " + (cur.empty() ? std::string("none") : cur) + " previous " +
                    (prev.empty() ? std::string("none") : prev));
    if (!seen.serving) {
        r.out.push_back("server stopped");
    } else if (seen.legacy) {
        r.out.push_back("server serving legacy");
    } else {
        r.out.push_back("server " + std::string(seen.draining ? "draining " : "serving ") +
                        (seen.build.empty() ? std::string("none") : seen.build));
    }
    r.exit = kExitOk;
    return r.exit;
}

std::unique_ptr<PlacementPort> makeBuildsPlacement(const std::string& serverDir, const builds::Options& options) {
    return std::make_unique<BuildsPlacement>(serverDir, options);
}

std::unique_ptr<ServerPort> makePosixServerPort(const std::string& serverDir,
                                                const std::function<std::uint64_t()>& now_ms,
                                                const std::function<void(std::uint64_t)>& sleep_ms,
                                                pid_t* adopted_pid_out) {
    return std::make_unique<PosixServerPort>(serverDir, now_ms, sleep_ms, adopted_pid_out);
}

std::string help(const std::string& verb) {
    if (verb.empty() || verb == "help") {
        return "nexus-server handover: replace the running server with a placed build under an OS lock\n"
               "  handover <hash>    switch current to <hash>, drain the running server, start <hash>,\n"
               "                     and roll back to previous if <hash> does not reach SERVING\n"
               "  handover [resume]  finish any interrupted handover in DIR/handover/state, or start\n"
               "                     DIR/builds/active's current build if it is not serving\n"
               "  handover status    print DIR/handover/state, current/previous, and the running server\n"
               "  handover help [<verb>]\n"
               "exit statuses: 0 ok, 1 failed, 2 usage, 3 refused, 4 busy, 5 rolled back to previous\n";
    }
    if (verb == "to" || verb == "switch") {
        return "nexus-server handover <hash> [--dir DIR]\n"
               "  1. takes the exclusive flock on DIR/handover, syncs it, removes .state-* leftovers,\n"
               "     and finishes resuming any interrupted DIR/handover/state first\n"
               "  2. checks <hash> (placed, whole, tested, compatible formats), writes stage switching,\n"
               "     and switches DIR/builds/active to (<hash>, previous)\n"
               "  3. writes stage draining and drains the running server (or stops a pre-drain server)\n"
               "  4. writes stage starting and starts DIR/current/bin/nexus-server serve --dir DIR\n"
               "  5. if <hash> fails to answer SERVING within the start limit, writes stage rolling-back,\n"
               "     switches DIR/builds/active back to previous, starts previous, and exits 5\n";
    }
    if (verb == "resume") {
        return "nexus-server handover [resume] [--dir DIR]\n"
               "  takes the exclusive flock on DIR/handover and resumes whatever stage DIR/handover/state\n"
               "  records (switching, draining, starting or rolling-back); with no state file, drains any\n"
               "  other running build and starts DIR/builds/active's current build. Exits 0 once serving.\n";
    }
    if (verb == "status") {
        return "nexus-server handover status [--dir DIR]\n"
               "  reads DIR/handover/state, DIR/builds/active and the running server's control socket\n"
               "  without changing anything on disk.\n";
    }
    return std::string();
}

int command(const std::string& serverDir, const std::vector<std::string>& args, const Options& options, Result& r) {
    r = Result{};
    ::signal(SIGPIPE, SIG_IGN);
    if (args.size() > 2) {
        r.err.push_back("usage: nexus-server handover [<hash>|resume|status|help] [--dir DIR]");
        r.exit = kExitUsage;
        return r.exit;
    }
    if (!args.empty() && args[0] == "help") {
        const std::string text = help(args.size() == 2 ? args[1] : std::string());
        if (text.empty()) {
            r.err.push_back("unknown handover verb `" + args[1] + "` (`nexus-server handover help`)");
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
    if (args.size() == 2) {
        r.err.push_back("usage: nexus-server handover [<hash>|resume|status|help] [--dir DIR]");
        r.exit = kExitUsage;
        return r.exit;
    }

    store::PosixOptions fsOpts;
    fsOpts.now_ms = options.now_ms;
    fsOpts.sleep_ms = options.sleep_ms;
    fsOpts.test_fs_type = options.test_fs_type;
    fsOpts.test_locks_ignored = options.test_locks_ignored;
    std::unique_ptr<store::Fs> fs = store::make_posix_fs(fsOpts);

    const bool statusVerb = (!args.empty() && args[0] == "status");
    if (!statusVerb && !options.lock_already_held) {
        if (const char* envHome = std::getenv("HOME"); envHome != nullptr && *envHome != '\0') {
            std::string h = envHome;
            while (h.size() > 1 && h.back() == '/') {
                h.pop_back();
            }
            const std::string top = h + "/.nexus";
            std::string sdir = serverDir;
            while (sdir.size() > 1 && sdir.back() == '/') {
                sdir.pop_back();
            }
            if (sdir == top + "/server") {
                migration::Options mopts;
                mopts.old_server_live = [&] { return migration::probeOldServerSocket(top + "/server.sock"); };
                migration::Report rep;
                const Status ms = migration::migrateOldLayout(*fs, top, sdir, mopts, &rep);
                if (!ms.is_ok()) {
                    r.err.push_back(ms.message());
                    r.exit = rep.conflict ? kExitRefused : (ms.code() == Status::Code::Busy ? kExitBusy : kExitFailed);
                    return r.exit;
                }
            }
        }
    }

    builds::Options bOpts;
    bOpts.now_ms = options.now_ms;
    bOpts.sleep_ms = options.sleep_ms;
    bOpts.lock_wait_ms = options.lock_wait_ms;
    bOpts.test_fs_type = options.test_fs_type;
    bOpts.test_locks_ignored = options.test_locks_ignored;
    if (options.adopted_pid_out != nullptr) {
        *options.adopted_pid_out = -1;
    }
    std::unique_ptr<PlacementPort> placement = makeBuildsPlacement(serverDir, bOpts);
    std::unique_ptr<ServerPort> server =
        makePosixServerPort(serverDir, options.now_ms, options.sleep_ms, options.adopted_pid_out);

    Handover engine(*fs, *placement, *server, serverDir + "/" + kHandoverDir, options);
    if (args.empty() || args[0] == "resume") {
        return engine.resume(r);
    }
    if (args[0] == "status") {
        return engine.status(r);
    }
    return engine.to(args[0], r);
}

} // namespace nexus::server::handover
