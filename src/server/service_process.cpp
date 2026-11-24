#include "server/service_process.h"

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
#include <thread>
#include <utility>

#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace nexus::server {
namespace {

using Clock = std::chrono::steady_clock;
constexpr auto grace = std::chrono::seconds(2);
constexpr int deferred = 75;
// Carry the service's literal environment through exec without applying its loader settings to the guard.
constexpr std::string_view childEnvPrefix = "NEXUS_SERVICE_ENV_";

void report(const std::string& name, const char* what, int error) {
    std::fprintf(stderr, "nexus-server: service %s: %s: %s\n", name.c_str(), what, std::strerror(error));
}

std::uint64_t wallMs() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}

bool safeName(const std::string& name) {
    return !name.empty() && name.size() <= 128 && name != "." && name != ".." &&
        name.find_first_not_of("0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ._-") == std::string::npos;
}

class Fd {
public:
    explicit Fd(int fd = -1) : value(fd) {}
    ~Fd() { reset(); }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    void reset() { if (value >= 0) { close(value); value = -1; } }
    int value;
};

int privateDir(const std::string& path) {
    if (mkdir(path.c_str(), 0700) < 0 && errno != EEXIST) return -1;
    const int fd = open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return -1;
    struct stat st{};
    if (fstat(fd, &st) == 0 && st.st_uid == geteuid() && (st.st_mode & 0077) == 0) return fd;
    close(fd);
    errno = EACCES;
    return -1;
}

int privateFile(int directory, const std::string& name, int flags, bool sharedInitialDue = false) {
    const int fd = openat(directory, name.c_str(), flags | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0600);
    if (fd < 0) return -1;
    struct stat st{};
    if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_uid == geteuid() && (st.st_nlink == 1 || sharedInitialDue) &&
        (st.st_mode & 0077) == 0) return fd;
    close(fd);
    errno = EACCES;
    return -1;
}

bool number(std::string_view text, std::uint64_t& n) {
    const auto result = std::from_chars(text.data(), text.data() + text.size(), n);
    return !text.empty() && result.ec == std::errc{} && result.ptr == text.data() + text.size();
}

// Rename makes a claim either entirely old or entirely new after a crash. Only the lease owner advances it.
bool storeDue(int dir, const std::string& name, std::uint64_t period, std::uint64_t due, bool initial) {
    const std::string dest = name + ".due";
    const std::string temp = dest + "." + std::to_string(getpid());
    // A prior process using this PID may have died before install. No other live writer owns this temp name.
    if (unlinkat(dir, temp.c_str(), 0) < 0 && errno != ENOENT) return false;
    Fd fd(privateFile(dir, temp, O_WRONLY | O_CREAT | O_EXCL));
    if (fd.value < 0) return false;
    const auto data = std::to_string(period) + ":" + std::to_string(due);
    bool ok = write(fd.value, data.data(), data.size()) == static_cast<ssize_t>(data.size()) && fsync(fd.value) == 0;
    if (ok && initial) {
        ok = linkat(dir, temp.c_str(), dir, dest.c_str(), 0) == 0 || errno == EEXIST;
    } else if (ok) {
        ok = renameat(dir, temp.c_str(), dir, dest.c_str()) == 0;
    }
    const int error = errno;
    unlinkat(dir, temp.c_str(), 0);
    if (!ok) { errno = error; return false; }
    return fsync(dir) == 0;
}

bool readDue(int dir, const std::string& name, std::uint64_t& period, std::uint64_t& due) {
    // A crash between installing the first due file by linkat and removing its temporary name
    // leaves two names for the same complete record. We only read this inode. Advances replace it.
    Fd fd(privateFile(dir, name + ".due", O_RDONLY, true));
    if (fd.value < 0) return false;
    char bytes[64];
    const auto len = read(fd.value, bytes, sizeof bytes);
    if (len < 0) return false;
    const std::string_view data(bytes, static_cast<std::size_t>(len));
    const auto colon = data.find(':');
    if (colon != data.npos && number(data.substr(0, colon), period) && number(data.substr(colon + 1), due)) return true;
    errno = EINVAL;
    return false;
}

bool firstDue(const std::string& path, const std::string& name, std::uint64_t period, std::uint64_t now,
              std::uint64_t& due) {
    Fd dir(privateDir(path));
    if (dir.value < 0) return false;
    std::uint64_t previousPeriod = 0;
    if (!readDue(dir.value, name, previousPeriod, due)) {
        if (errno != ENOENT || !storeDue(dir.value, name, period, now + period, true) ||
            !readDue(dir.value, name, previousPeriod, due)) return false;
    }
    if (previousPeriod == period) return true;
    // The former interval's helper may still be completing its run. Change the period only under its lease,
    // so an old completion cannot replace the new, durably scheduled first deadline.
    Fd lock(privateFile(dir.value, name + ".lock", O_RDWR | O_CREAT));
    if (lock.value < 0 || flock(lock.value, LOCK_EX | LOCK_NB) != 0 ||
        !readDue(dir.value, name, previousPeriod, due)) return false;
    if (previousPeriod == period) return true;
    due = now + period;
    return storeDue(dir.value, name, period, due, false);
}

std::vector<char*> pointers(std::vector<std::string>& strings) {
    std::vector<char*> out;
    out.reserve(strings.size() + 1);
    for (auto& s : strings) out.push_back(s.data());
    out.push_back(nullptr);
    return out;
}

bool groupAlive(pid_t pgid) {
    return kill(-pgid, 0) == 0 || errno != ESRCH;
}

volatile sig_atomic_t guardStopping = 0;
void stopGuard(int) { guardStopping = 1; }

bool ownerGone(int timeout) {
    pollfd p{STDIN_FILENO, POLLIN, 0};
    const int ready = poll(&p, 1, timeout);
    return guardStopping || ready > 0 || (ready < 0 && errno != EINTR);
}

int logFile(const std::string& log) {
    if (log.empty() || log.front() != '/') { errno = EINVAL; return -1; }
    std::error_code error;
    std::filesystem::create_directories(std::filesystem::path(log).parent_path(), error);
    if (error) { errno = error.value(); return -1; }
    const int fd = open(log.c_str(), O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0600);
    if (fd < 0) return -1;
    struct stat st{};
    if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode)) return fd;
    close(fd);
    errno = EINVAL;
    return -1;
}

std::string executable(const std::string& cmd) {
    if (!cmd.empty() && cmd.front() == '/') return cmd;
    if (cmd.empty() || cmd.find('/') != cmd.npos) return {};
    const char* path = getenv("NEXUS_SERVICE_ENV_PATH");
    if (path == nullptr) return {};
    std::string_view rest(path);
    while (!rest.empty()) {
        const auto colon = rest.find(':');
        const auto part = rest.substr(0, colon);
        if (!part.empty() && part.front() == '/') {
            const auto candidate = std::string(part) + '/' + cmd;
            if (access(candidate.c_str(), X_OK) == 0) return candidate;
        }
        if (colon == rest.npos) break;
        rest.remove_prefix(colon + 1);
    }
    return {};
}

bool knownGroup(int lock, pid_t& group) {
    char data[32];
    const auto n = pread(lock, data, sizeof data, 0);
    if (n < 0) return false;
    if (n == 0) { group = 0; return true; }
    std::uint64_t value = 0;
    if (number({data, static_cast<std::size_t>(n)}, value) && value > 1 &&
        value <= static_cast<std::uint64_t>(std::numeric_limits<pid_t>::max())) {
        group = static_cast<pid_t>(value); return true;
    }
    errno = EINVAL;
    return false;
}

bool markGroup(int lock, pid_t group) {
    const auto marker = std::to_string(group);
    return ftruncate(lock, 0) == 0 && write(lock, marker.data(), marker.size()) == static_cast<ssize_t>(marker.size()) &&
        fsync(lock) == 0;
}

bool reaped(pid_t pid, int& status) {
    const auto result = waitpid(pid, &status, WNOHANG);
    return result == pid || (result < 0 && errno == ECHILD);
}

// Keep an exited leader waitable until the last group signal. Its reserved PID then cannot be
// reused as the ID of an unrelated process group while this helper can still signal it.
bool childEnded(pid_t pid) {
    siginfo_t info{};
    int rc;
    do { rc = waitid(P_PID, static_cast<id_t>(pid), &info, WEXITED | WNOHANG | WNOWAIT); }
    while (rc < 0 && errno == EINTR);
    return rc == 0 && info.si_pid == pid;
}

bool finishGroup(pid_t pid, bool ordinaryExit, int& status) {
    const auto start = Clock::now();
    if (!ordinaryExit) {
        // Signal the whole group before any member has its PID reaped. If the leader
        // exits, its descendants have received TERM too and are cleaned up below.
        kill(-pid, SIGTERM);
        while (!childEnded(pid) && Clock::now() - start < grace)
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    kill(-pid, SIGKILL);
    // No signals after this reap. A surviving process group keeps its ID allocated until its last member exits.
    bool leaderDone = false;
    const auto until = Clock::now() + grace;
    do {
        if (!leaderDone) leaderDone = reaped(pid, status);
        if (leaderDone && !groupAlive(pid)) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    } while (Clock::now() < until);
    return false;
}
} // namespace

int runServiceChild(int argc, char** argv) {
    if (argc < 9) return 2;
    const std::string name = argv[3];
    std::uint64_t period = 0, claim = 0;
    sockaddr_storage peer{};
    socklen_t size = sizeof peer;
    if (!safeName(name) || !number(argv[4], period) || !number(argv[5], claim) ||
        getpeername(STDIN_FILENO, reinterpret_cast<sockaddr*>(&peer), &size) != 0 || peer.ss_family != AF_UNIX) return 2;
    signal(SIGPIPE, SIG_IGN);
    signal(SIGCHLD, SIG_DFL);
    struct sigaction handler{};
    handler.sa_handler = stopGuard;
    sigaction(SIGTERM, &handler, nullptr);
    sigaction(SIGINT, &handler, nullptr);
    Fd dir(privateDir(argv[2]));
    if (dir.value < 0) { report(name, "open service lease directory", errno); return 1; }
    Fd lock(privateFile(dir.value, name + ".lock", O_RDWR | O_CREAT));
    if (lock.value < 0) { report(name, "open service lease", errno); return 1; }
    while (flock(lock.value, LOCK_EX | LOCK_NB) < 0) {
        if (errno != EWOULDBLOCK && errno != EINTR) { report(name, "acquire service lease", errno); return 1; }
        if (ownerGone(100)) return 0;
    }
    pid_t previous = 0;
    if (!knownGroup(lock.value, previous)) { report(name, "read service lease", errno); return 1; }
    if (previous > 0 && groupAlive(previous)) {
        // An externally killed helper may have left descendants. Never signal a group we cannot still identify.
        std::fprintf(stderr, "nexus-server: service %s: previous process group %ld is still present. "
                     "inspect its owner before starting this service again\n", name.c_str(), static_cast<long>(previous));
        return deferred;
    }
    if (ownerGone(0)) return 0;
    const std::string command = executable(argv[8]);
    if (command.empty()) { report(name, "locate service executable in its configured PATH", ENOENT); return 1; }
    std::vector<char*> childEnv;
    for (char** var = environ; *var; ++var) {
        if (std::string_view(*var).starts_with(childEnvPrefix)) childEnv.push_back(*var + childEnvPrefix.size());
    }
    childEnv.push_back(nullptr);
    Fd log(logFile(argv[7]));
    if (log.value < 0) { report(name, "open configured service log", errno); return 1; }
    Fd input(open("/dev/null", O_RDONLY | O_CLOEXEC));
    if (input.value < 0) { report(name, "open service input", errno); return 1; }
    if (chdir(argv[6]) != 0) { report(name, "enter configured working directory", errno); return 1; }
    if (period) {
        std::uint64_t previousPeriod = 0, due = 0;
        if (!readDue(dir.value, name, previousPeriod, due)) { report(name, "read scheduled claim", errno); return 1; }
        if (previousPeriod == period && due > claim) return deferred;
    }
    int gate[2], execError[2];
    if (pipe(gate) != 0 || pipe(execError) != 0 || fcntl(execError[1], F_SETFD, FD_CLOEXEC) != 0) {
        report(name, "make service launch pipe", errno); return 1;
    }
    const pid_t child = fork();
    if (child < 0) { report(name, "fork service", errno); return 1; }
    if (child == 0) {
        close(gate[1]); close(execError[0]);
        char go = 0;
        if (setpgid(0, 0) != 0 || read(gate[0], &go, 1) != 1 || go != 'G') _exit(1);
        close(gate[0]);
        signal(SIGTERM, SIG_DFL); signal(SIGINT, SIG_DFL); signal(SIGPIPE, SIG_DFL);
        if (dup2(log.value, STDOUT_FILENO) >= 0 && dup2(log.value, STDERR_FILENO) >= 0 &&
                dup2(input.value, STDIN_FILENO) >= 0) {
            execve(command.c_str(), argv + 8, childEnv.data());
        }
        const int error = errno;
        (void)!write(execError[1], &error, sizeof error);
        _exit(127);
    }
    close(gate[0]); close(execError[1]);
    // The child waits for this byte. If the guard dies before the marker is durable it sees EOF instead of starting.
    int status = 0;
    bool prepared = setpgid(child, child) == 0 && markGroup(lock.value, child) && fsync(dir.value) == 0;
    if (prepared && period) prepared = storeDue(dir.value, name, period, claim + period, false);
    bool launched = prepared && !ownerGone(0) && write(gate[1], "G", 1) == 1;
    if (!prepared) report(name, "persist service launch claim", errno);
    close(gate[1]);
    if (launched) {
        int error = 0;
        ssize_t n;
        do { n = read(execError[0], &error, sizeof error); } while (n < 0 && errno == EINTR);
        launched = n == 0;
        if (!launched) report(name, "execute service", n == static_cast<ssize_t>(sizeof error) ? error : EIO);
    }
    close(execError[0]);
    bool leaderDone = false;
    if (launched && write(STDIN_FILENO, "R", 1) == 1) {
        while (!(leaderDone = childEnded(child)) && !ownerGone(50)) {}
    }
    if (!finishGroup(child, leaderDone, status)) {
        std::fprintf(stderr, "nexus-server: service %s: group %ld still present after shutdown. Keeping its lease marker\n",
            name.c_str(), static_cast<long>(child));
        return 1;
    }
    if (period && prepared && !storeDue(dir.value, name, period, std::max(claim, wallMs()) + period, false)) {
        report(name, "record service interval after exit", errno); return 1;
    }
    if (ftruncate(lock.value, 0) != 0 || fsync(lock.value) != 0) { report(name, "clear service lease", errno); return 1; }
    return launched && WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : 1;
}

ServiceProcesses::ServiceProcesses(std::string self, std::function<std::string()> leasePath)
    : self_(std::move(self)), leasePath_(std::move(leasePath)) {}

ServiceProcesses::~ServiceProcesses() { stop(); }

void ServiceProcesses::end(Slot& slot) {
    if (slot.control >= 0) { close(slot.control); slot.control = -1; }
    slot.stopping = slot.helper >= 0;
}

void ServiceProcesses::configure(const std::vector<ManagedService>& services, std::uint64_t now) {
    for (auto& [name, slot] : slots_) {
        if (std::none_of(services.begin(), services.end(), [&](const auto& service) { return service.name == name; })) {
            slot.desired.reset();
            end(slot);
        }
    }
    for (const auto& service : services) {
        if (!safeName(service.name) || service.argv.empty()) continue;
        auto& slot = slots_[service.name];
        if (slot.desired == service) continue;
        end(slot);
        slot.desired = service;
        slot.next = now;
        slot.failures = 0;
        slot.dueWall = 0;
        if (service.everySeconds) schedule(slot, now);
    }
}

void ServiceProcesses::schedule(Slot& slot, std::uint64_t now) {
    const auto& service = *slot.desired;
    const std::uint64_t period = std::uint64_t(service.everySeconds) * 1000;
    const auto wall = wallMs();
    if (!firstDue(leasePath_(), service.name, period, wall, slot.dueWall)) {
        if (errno != EWOULDBLOCK) report(service.name, "initialize service schedule", errno);
        slot.dueWall = 0;
        retry(slot, now);
        return;
    }
    slot.next = now + (slot.dueWall > wall ? slot.dueWall - wall : 0);
}

bool ServiceProcesses::start(Slot& slot, std::uint64_t now) {
    const auto& service = *slot.desired;
    const auto period = std::uint64_t(service.everySeconds) * 1000;
    slot.claim = period ? std::max(wallMs(), slot.dueWall + (now - slot.next)) : 0;
    std::vector<std::string> args{self_, "service-child", leasePath_(), service.name, std::to_string(period),
                                  std::to_string(slot.claim), service.cwd, service.log};
    args.insert(args.end(), service.argv.begin(), service.argv.end());
    std::vector<std::string> env;
    for (const auto& [key, value] : service.env) env.push_back(std::string(childEnvPrefix) + key + '=' + value);
    auto argv = pointers(args), envp = pointers(env);
    int channel[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, channel) != 0) { report(service.name, "make lifetime socket", errno); return false; }
    Fd parent(channel[0]), helper(channel[1]);
    if (fcntl(parent.value, F_SETFD, FD_CLOEXEC) < 0 || fcntl(helper.value, F_SETFD, FD_CLOEXEC) < 0) {
        report(service.name, "isolate lifetime socket", errno); return false;
    }
    posix_spawn_file_actions_t files;
    posix_spawnattr_t attr;
    int err = posix_spawn_file_actions_init(&files);
    if (err) { report(service.name, "prepare helper files", err); return false; }
    err = posix_spawnattr_init(&attr);
    if (err) { posix_spawn_file_actions_destroy(&files); report(service.name, "prepare helper attributes", err); return false; }
    // Only stdin (the lifetime socket) and the diagnostic stream cross into the helper. In particular it receives
    // neither the server's control socket nor another service's lifetime socket.
    err = posix_spawn_file_actions_adddup2(&files, helper.value, STDIN_FILENO);
    if (!err) err = posix_spawn_file_actions_addopen(&files, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
#if defined(__APPLE__)
    if (!err) err = posix_spawn_file_actions_addinherit_np(&files, STDERR_FILENO);
    if (!err) err = posix_spawnattr_setflags(&attr, POSIX_SPAWN_CLOEXEC_DEFAULT | POSIX_SPAWN_SETPGROUP);
#else
    if (!err) err = posix_spawn_file_actions_addclosefrom_np(&files, 3);
    if (!err) err = posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP);
#endif
    if (!err) err = posix_spawnattr_setpgroup(&attr, 0);
    pid_t pid = -1;
    if (!err) err = posix_spawn(&pid, self_.c_str(), &files, &attr, argv.data(), envp.data());
    posix_spawn_file_actions_destroy(&files);
    posix_spawnattr_destroy(&attr);
    if (err) { report(service.name, "start service helper", err); return false; }
    slot.helper = pid;
    slot.control = std::exchange(parent.value, -1);
    slot.started = now;
    slot.running = false;
    slot.stopping = false;
    return true;
}

void ServiceProcesses::retry(Slot& slot, std::uint64_t now) {
    const auto seconds = std::min(30u, 1u << std::min(slot.failures, 5u));
    slot.next = now + seconds * 1000;
    slot.failures = std::min(slot.failures + 1, 5u);
}

void ServiceProcesses::tick(std::uint64_t now) {
    for (auto it = slots_.begin(); it != slots_.end();) {
        auto& slot = it->second;
        if (slot.helper > 0) {
            if (!slot.running && slot.control >= 0) {
                char byte = 0;
                if (recv(slot.control, &byte, 1, MSG_DONTWAIT) == 1 && byte == 'R') slot.running = true;
            }
            int status = 0;
            if (!reaped(slot.helper, status)) { ++it; continue; }
            if (!slot.stopping && slot.desired) {
                if (now - slot.started >= 30000 && slot.running) slot.failures = 0;
                if (slot.desired->everySeconds && slot.running) {
                    const auto period = std::uint64_t(slot.desired->everySeconds) * 1000;
                    slot.next = now + period;
                    slot.dueWall = std::max(wallMs(), slot.claim) + period;
                } else {
                    retry(slot, now);
                    if (slot.desired->everySeconds) slot.dueWall = 0;
                }
                if (WIFSIGNALED(status) || (WIFEXITED(status) && WEXITSTATUS(status) != 0 && WEXITSTATUS(status) != deferred)) {
                    std::fprintf(stderr, "nexus-server: service %s ended (%s %d). See its configured log\n",
                        it->first.c_str(), WIFSIGNALED(status) ? "signal" : "exit", WIFSIGNALED(status) ? WTERMSIG(status) : WEXITSTATUS(status));
                }
            }
            end(slot);
            slot.helper = -1;
            slot.running = slot.stopping = false;
        }
        if (!slot.desired) { it = slots_.erase(it); continue; }
        if (slot.next <= now) {
            if (slot.desired->everySeconds && !slot.dueWall) schedule(slot, now);
            if (slot.next <= now && (!slot.desired->everySeconds || slot.dueWall) && !start(slot, now)) retry(slot, now);
        }
        ++it;
    }
}

int ServiceProcesses::waitMs(std::uint64_t now) const {
    std::uint64_t wait = 1000;
    for (const auto& [name, slot] : slots_) {
        (void)name;
        if (slot.helper > 0) wait = std::min<std::uint64_t>(wait, slot.running && !slot.stopping ? 1000 : 100);
        else if (slot.desired) wait = std::min(wait, slot.next > now ? slot.next - now : 0);
    }
    return static_cast<int>(wait);
}

std::string ServiceProcesses::status() const {
    if (slots_.empty()) return "no managed services\n";
    std::string result;
    for (const auto& [name, slot] : slots_) {
        result += "service " + name + " " + (slot.stopping ? "stopping" : slot.running ? "running" :
                  slot.helper > 0 ? "starting" : "waiting") + "\n";
    }
    return result;
}

void ServiceProcesses::stop() {
    for (auto& [name, slot] : slots_) { (void)name; end(slot); }
    const auto endAt = Clock::now() + grace * 2 + std::chrono::seconds(1);
    bool forced = false;
    while (!slots_.empty()) {
        for (auto it = slots_.begin(); it != slots_.end();) {
            int status = 0;
            const auto pid = it->second.helper;
            if (pid <= 0 || reaped(pid, status)) it = slots_.erase(it);
            else ++it;
        }
        if (slots_.empty()) break;
        if (!forced && Clock::now() >= endAt) {
            for (auto& [name, slot] : slots_) {
                std::fprintf(stderr, "nexus-server: service %s helper failed to stop. Ending its helper and "
                             "preserving the group marker\n", name.c_str());
                kill(slot.helper, SIGKILL);
            }
            forced = true;
        }
        if (forced && Clock::now() >= endAt + std::chrono::milliseconds(500)) {
            for (const auto& [name, slot] : slots_)
                std::fprintf(stderr, "nexus-server: service %s helper %ld still awaits kernel exit\n",
                             name.c_str(), static_cast<long>(slot.helper));
            slots_.clear();
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

} // namespace nexus::server
