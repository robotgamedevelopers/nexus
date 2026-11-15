// Functional test of the store's lock in the real nexus-server (src/server/start_order.h, src/store/store.h): real
// processes, files, flock and the control socket, so functional. HOME, for every server, is a directory of this test's
// own; nothing touches the machine's ~/.nexus.
//
//   - Races: in each of 10 rounds, 4 server directories at once, each with 2 `serve` started together on it. Exactly
//     one of each pair serves; the other exits 0 at once and says in one line that another server has the store or the
//     socket. 40 races in all.
//   - `new` raced: 6 `new` started together on a fresh directory, each of which may start a server: every one prints
//     its CONNECT line, and one server answers.
//   - kill -9: the server holding the store is killed with SIGKILL, once serving and again as soon as its socket is
//     bound (mid-start), and a new one, started at once, serves (no lock is left behind, and no child of the dead one
//     holds its socket).
//   - `store check` while a server runs: refused in one line, exit 1, and the server keeps serving; once it has
//     stopped, the check reads the store, exit 0, and changes nothing; no lock file is ever made.
//   - A writer holds the lock and no server answers (this test holding it, as a server stopping or replaying would):
//     a server started waits and serves once it is let go; held past the wait (NEXUS_TEST_HELD_WAIT_MS), exit 3.
//   - A reader holds the lock past a writer's wait (this test holding it shared, as a hung `store check`): exit 1,
//     naming readers, never exit 0; let go, the next one serves.
//   - The store reached by another name (a second server directory whose store is a symbolic link): the second
//     server writes nothing and exits 3; the first serves on.
//   - The server's directory moved aside and a new one made at its old name: the old server, stopped, removes no
//     socket but its own and writes nothing into the new store.
//
// Usage: server_lock <nexus-server> <dir>, with <dir> a directory of this test's own (a Unix socket's path must be
// short, so it is under /tmp, named for the build tree: tests/link/CMakeLists.txt).

#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <stdlib.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

extern char** environ;

namespace {

int failures = 0;

#define CHECK(cond)                                                                       \
    do {                                                                                  \
        if (!(cond)) {                                                                    \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                                   \
        }                                                                                 \
    } while (0)

using Clock = std::chrono::steady_clock;

bool has(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

bool exists(const std::string& path) {
    struct stat st {};
    return lstat(path.c_str(), &st) == 0;
}

std::string readFile(const std::string& path) {
    std::ifstream in(path);
    std::ostringstream all;
    all << in.rdbuf();
    return all.str();
}

// Runs `command` through /bin/sh; its standard output and error in `out`, and its exit status.
int run(const std::string& command, std::string* out) {
    out->clear();
    FILE* p = popen((command + " 2>&1").c_str(), "r");
    if (p == nullptr) {
        return -1;
    }
    char buf[512];
    std::size_t n = 0;
    while ((n = std::fread(buf, 1, sizeof buf, p)) > 0) {
        out->append(buf, n);
    }
    const int status = pclose(p);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

// `args` as a child, its standard output and error to `log`.
pid_t spawn(const std::vector<std::string>& args, const std::string& log) {
    posix_spawn_file_actions_t files;
    posix_spawn_file_actions_init(&files);
    posix_spawn_file_actions_addopen(&files, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&files, 1, log.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    posix_spawn_file_actions_adddup2(&files, 1, 2);
    std::vector<std::string> copy = args;
    std::vector<char*> argv;
    for (std::string& a : copy) {
        argv.push_back(a.data());
    }
    argv.push_back(nullptr);
    pid_t pid = -1;
    if (posix_spawn(&pid, copy[0].c_str(), &files, nullptr, argv.data(), environ) != 0) {
        pid = -1;
    }
    posix_spawn_file_actions_destroy(&files);
    return pid;
}

// The child's exit status once it has ended, within `seconds`; -1 if it is still running then, -2 if it was killed.
int waitExit(pid_t pid, int seconds) {
    const auto end = Clock::now() + std::chrono::seconds(seconds);
    for (;;) {
        int status = 0;
        const pid_t got = waitpid(pid, &status, WNOHANG);
        if (got == pid) {
            return WIFEXITED(status) ? WEXITSTATUS(status) : -2;
        }
        if (Clock::now() >= end) {
            return -1;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

bool answering(const std::string& dir) {
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    const std::string path = dir + "/server.sock";
    std::memcpy(addr.sun_path, path.c_str(), std::min(path.size() + 1, sizeof addr.sun_path));
    const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    const bool ok = connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof addr) == 0;
    close(fd);
    return ok;
}

bool waitAnswering(const std::string& dir, int seconds) {
    const auto end = Clock::now() + std::chrono::seconds(seconds);
    while (Clock::now() < end) {
        if (answering(dir)) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

// `text` in the file at `path`, within `seconds`.
bool waitLog(const std::string& path, const std::string& text, int seconds) {
    const auto end = Clock::now() + std::chrono::seconds(seconds);
    while (Clock::now() < end) {
        if (has(readFile(path), text)) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

// The line a refused server writes: another server has the store's lock, or answers on the socket.
bool refusedLine(const std::string& log) {
    return has(log, "so this one exits") &&
        (has(log, "is open in another process, which holds its lock") || has(log, "already answers on"));
}

void stopServer(const std::string& server, const std::string& dir) {
    std::string out;
    run("'" + server + "' stop --dir '" + dir + "'", &out);
}

// Each round: four directories at once, two servers started together on each.
void races(const std::string& server, const std::string& top) {
    constexpr int kRounds = 10;
    constexpr int kDirs = 4;
    int served = 0;
    int refused = 0;
    for (int round = 0; round < kRounds; ++round) {
        std::vector<std::string> dirs;
        std::vector<pid_t> pids;
        std::vector<std::string> logs;
        for (int d = 0; d < kDirs; ++d) {
            const std::string dir = top + "/race-" + std::to_string(d);
            CHECK(mkdir(dir.c_str(), 0700) == 0 || errno == EEXIST);
            dirs.push_back(dir);
        }
        for (int d = 0; d < kDirs; ++d) {
            for (int k = 0; k < 2; ++k) {
                const std::string log = dirs[std::size_t(d)] + "/serve-" + std::to_string(k) + ".log";
                logs.push_back(log);
                pids.push_back(spawn({server, "serve", "--dir", dirs[std::size_t(d)]}, log));
            }
        }
        for (int d = 0; d < kDirs; ++d) {
            const std::size_t a = std::size_t(d) * 2;
            const std::size_t b = a + 1;
            CHECK(pids[a] > 0 && pids[b] > 0);
            // The one refused exits at once; the other is still running.
            int exitA = waitExit(pids[a], 0);
            int exitB = waitExit(pids[b], 0);
            const auto end = Clock::now() + std::chrono::seconds(20);
            while (exitA == -1 && exitB == -1 && Clock::now() < end) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                exitA = waitExit(pids[a], 0);
                exitB = waitExit(pids[b], 0);
            }
            const bool aRefused = exitA != -1;
            const std::size_t loser = aRefused ? a : b;
            const std::size_t winner = aRefused ? b : a;
            const int loserExit = aRefused ? exitA : exitB;
            CHECK(loserExit == 0);
            CHECK(refusedLine(readFile(logs[loser])));
            CHECK(waitAnswering(dirs[std::size_t(d)], 20));
            CHECK(waitExit(pids[winner], 0) == -1);
            if (loserExit == 0 && waitExit(pids[winner], 0) == -1) {
                ++served;
                ++refused;
            } else {
                std::fprintf(stderr, "round %d dir %d: exits %d and %d\n%s\n---\n%s\n", round, d, exitA, exitB,
                             readFile(logs[a]).c_str(), readFile(logs[b]).c_str());
            }
            stopServer(server, dirs[std::size_t(d)]);
            CHECK(waitExit(pids[winner], 20) == 0);
        }
    }
    std::printf("races: %d of %d served by exactly one server, the other refused with exit 0\n", served,
                kRounds * kDirs);
    CHECK(served == kRounds * kDirs && refused == served);
}

// Six `new` at once on a fresh directory: each may start a server; every one gets its CONNECT line.
void newRaced(const std::string& server, const std::string& top) {
    const std::string dir = top + "/new";
    CHECK(mkdir(dir.c_str(), 0700) == 0);
    std::vector<pid_t> pids;
    std::vector<std::string> logs;
    for (int k = 0; k < 6; ++k) {
        const std::string log = top + "/new-" + std::to_string(k) + ".log";
        logs.push_back(log);
        pids.push_back(spawn({server, "new", "--dir", dir}, log));
    }
    int connected = 0;
    for (std::size_t k = 0; k < pids.size(); ++k) {
        const int exit = waitExit(pids[k], 30);
        const std::string out = readFile(logs[k]);
        CHECK(exit == 0 && has(out, "NEXUS CONNECT"));
        if (exit == 0 && has(out, "NEXUS CONNECT")) {
            ++connected;
        } else {
            std::fprintf(stderr, "new %zu: exit %d: %s\n", k, exit, out.c_str());
        }
    }
    std::printf("new raced: %d of 6 connected\n", connected);
    CHECK(answering(dir));
    stopServer(server, dir);
    std::string out;
    for (int i = 0; i < 250 && answering(dir); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    // Every server any `new` started either served or said it was refused; none failed.
    const std::string log = readFile(dir + "/server.log");
    CHECK(!has(log, "nexus-server: store failed"));
    CHECK(has(log, "serving on"));
}

void killedOnce(const std::string& server, const std::string& dir, bool midStart);

// The holder killed with SIGKILL: a new server starts at once (the store's contents across a kill are
// store_functional's and link_e2e_restart's).
void killed(const std::string& server, const std::string& top) {
    for (int midStart = 0; midStart < 2; ++midStart) {
        killedOnce(server, top + (midStart ? "/killed-starting" : "/killed"), midStart != 0);
    }
}

// Killed once it serves, or (midStart) as soon as its socket is bound, while it may still be restoring the drivers.
void killedOnce(const std::string& server, const std::string& dir, bool midStart) {
    CHECK(mkdir(dir.c_str(), 0700) == 0);
    pid_t first = spawn({server, "serve", "--dir", dir}, dir + "/first.log");
    CHECK(first > 0);
    CHECK(midStart ? waitAnswering(dir, 20) : waitLog(dir + "/first.log", "serving on", 20));
    CHECK(kill(first, SIGKILL) == 0);
    CHECK(waitExit(first, 10) == -2);
    // Its socket is left behind, answering no one (no child of the dead server holds it), and its lock is gone with
    // it.
    struct stat st {};
    CHECK(lstat((dir + "/server.sock").c_str(), &st) == 0);
    CHECK(!answering(dir));
    const auto start = Clock::now();
    pid_t second = spawn({server, "serve", "--dir", dir}, dir + "/second.log");
    CHECK(second > 0 && waitLog(dir + "/second.log", "serving on", 20));
    const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
    std::printf("kill -9: the next server served %lld ms after it started\n", static_cast<long long>(took));
    CHECK(waitExit(second, 0) == -1);
    CHECK(has(readFile(dir + "/second.log"), "its lock (on the directory) held"));
    // It serves a window: `new` reaches it rather than starting another.
    std::string out;
    CHECK(run("'" + server + "' new --dir '" + dir + "'", &out) == 0 && has(out, "NEXUS CONNECT"));
    stopServer(server, dir);
    CHECK(waitExit(second, 20) == 0);
}

// `store check` while a server runs, and after.
void storeCheck(const std::string& server, const std::string& top) {
    const std::string dir = top + "/check";
    CHECK(mkdir(dir.c_str(), 0700) == 0);
    std::string out;
    CHECK(run("'" + server + "' store check --dir '" + dir + "'", &out) == 1 && has(out, "no store in"));
    pid_t pid = spawn({server, "serve", "--dir", dir}, dir + "/serve.log");
    CHECK(pid > 0 && waitAnswering(dir, 20));
    CHECK(run("'" + server + "' store check --dir '" + dir + "'", &out) == 1);
    CHECK(has(out, "is open in a running nexus-server, which holds its lock"));
    CHECK(std::count(out.begin(), out.end(), '\n') == 1);
    CHECK(!exists(dir + "/store/lock"));
    CHECK(answering(dir) && waitExit(pid, 0) == -1);
    stopServer(server, dir);
    CHECK(waitExit(pid, 20) == 0);
    struct stat before {};
    CHECK(stat((dir + "/store/log").c_str(), &before) == 0);
    CHECK(run("'" + server + "' store check --dir '" + dir + "'", &out) == 0);
    CHECK(has(out, "OK store " + dir + "/store") && has(out, "nothing was changed"));
    struct stat after {};
    CHECK(stat((dir + "/store/log").c_str(), &after) == 0 && after.st_size == before.st_size &&
          after.st_mtime == before.st_mtime);
    CHECK(run("'" + server + "' store --dir '" + dir + "'", &out) == 2 && has(out, "usage: nexus-server store check"));
    CHECK(!exists(dir + "/store/lock"));
}

// This test holding the store's directory locked itself, as another process would: a server stopping (its socket gone,
// its drivers ending) or starting (replaying), exclusive; a reader (`store check`, hung), shared. -1 on failure.
int holdStore(const std::string& dir, int how) {
    const int fd = open((dir + "/store").c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) {
        return -1;
    }
    if (flock(fd, how | LOCK_NB) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

// A writer holds the lock and no server answers (a server stopping after its socket closed, or one replaying): a server
// started then waits for the lock rather than exit 0, and serves once it is let go; held past the wait, it exits 3,
// its own code, saying so; never exit 0 with nothing serving.
void heldNoSocket(const std::string& server, const std::string& top) {
    const std::string dir = top + "/held";
    CHECK(mkdir(dir.c_str(), 0700) == 0 && mkdir((dir + "/store").c_str(), 0700) == 0);
    int fd = holdStore(dir, LOCK_EX);
    CHECK(fd >= 0);
    pid_t pid = spawn({"/usr/bin/env", "NEXUS_TEST_HELD_WAIT_MS=5000", server, "serve", "--dir", dir},
                      dir + "/waits.log");
    CHECK(pid > 0 && waitLog(dir + "/waits.log", "waiting up to 5000 ms", 20));
    CHECK(waitExit(pid, 0) == -1 && !answering(dir));
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    close(fd);      // the holder ends
    CHECK(waitLog(dir + "/waits.log", "serving on", 20) && waitAnswering(dir, 20));
    std::printf("held: a server waited for a held lock and served once it was let go\n");
    stopServer(server, dir);
    CHECK(waitExit(pid, 20) == 0);
    // Held for good: exit 3 after the wait, nothing left serving that was not, and a line that says what holds it.
    fd = holdStore(dir, LOCK_EX);
    CHECK(fd >= 0);
    const auto start = Clock::now();
    pid = spawn({"/usr/bin/env", "NEXUS_TEST_HELD_WAIT_MS=300", server, "serve", "--dir", dir}, dir + "/stuck.log");
    const int exit = waitExit(pid, 20);
    const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
    const std::string log = readFile(dir + "/stuck.log");
    CHECK(exit == 3 && took >= 300);
    CHECK(has(log, "no server answered on " + dir + "/server.sock in 300 ms") && has(log, "lsof"));
    CHECK(!answering(dir));
    std::printf("held: exit %d after %lld ms: %s", exit, static_cast<long long>(took), log.c_str());
    close(fd);
}

// A reader (a hung `store check`) holds the lock shared past the writer's wait: the starting server exits 1, a start
// that failed, naming readers, never exit 0 as if another server had the store; let go, the next one serves.
void readerHeld(const std::string& server, const std::string& top) {
    const std::string dir = top + "/reader";
    CHECK(mkdir(dir.c_str(), 0700) == 0 && mkdir((dir + "/store").c_str(), 0700) == 0);
    const int fd = holdStore(dir, LOCK_SH);
    CHECK(fd >= 0);
    pid_t pid = spawn({server, "serve", "--dir", dir}, dir + "/first.log");
    const int exit = waitExit(pid, 20);
    const std::string log = readFile(dir + "/first.log");
    CHECK(exit == 1 && has(log, "held by readers only") && !has(log, "another nexus-server"));
    CHECK(!answering(dir));
    std::printf("reader: exit %d: %s", exit, log.c_str());
    close(fd);
    pid = spawn({server, "serve", "--dir", dir}, dir + "/second.log");
    CHECK(pid > 0 && waitAnswering(dir, 20));
    stopServer(server, dir);
    CHECK(waitExit(pid, 20) == 0);
}

// The store reached by another name while a server holds it: a second server directory whose store is a symbolic link
// to the first's (so another socket, and only the lock between them). With the lock on a file found by name, removing
// that file let a second writer in; the lock is the directory's now, and no file can be removed: the second server
// waits, and exits 3, having written nothing; the first serves on.
void storeLinked(const std::string& server, const std::string& top) {
    const std::string dir = top + "/linked";
    const std::string other = top + "/linked-other";
    CHECK(mkdir(dir.c_str(), 0700) == 0 && mkdir(other.c_str(), 0700) == 0);
    pid_t first = spawn({server, "serve", "--dir", dir}, dir + "/first.log");
    CHECK(first > 0 && waitLog(dir + "/first.log", "serving on", 20));
    CHECK(!exists(dir + "/store/lock"));
    CHECK(symlink((dir + "/store").c_str(), (other + "/store").c_str()) == 0);
    struct stat before {};
    CHECK(stat((dir + "/store/log").c_str(), &before) == 0);
    pid_t second = spawn({"/usr/bin/env", "NEXUS_TEST_HELD_WAIT_MS=300", server, "serve", "--dir", other},
                         other + "/second.log");
    const int exit = waitExit(second, 20);
    const std::string log = readFile(other + "/second.log");
    CHECK(exit == 3 && has(log, "is open in another process, which holds its lock"));
    CHECK(!has(log, "serving on") && !answering(other));
    struct stat after {};
    CHECK(stat((dir + "/store/log").c_str(), &after) == 0 && after.st_size == before.st_size);
    CHECK(waitExit(first, 0) == -1 && answering(dir));
    std::printf("linked: the store by another name: exit %d, the first server serving on\n", exit);
    stopServer(server, dir);
    CHECK(waitExit(first, 20) == 0);
}

// The server's directory moved aside while it runs, and a new one made at the old name with a server of its own: the
// old server, stopped through its moved socket, removes no socket but its own (the new server answers on), and writes
// nothing into the new store; each store is its server's.
void dirMoved(const std::string& server, const std::string& top) {
    const std::string dir = top + "/mv";
    const std::string moved = top + "/mv-old";
    CHECK(mkdir(dir.c_str(), 0700) == 0);
    pid_t first = spawn({server, "serve", "--dir", dir}, top + "/mv-first.log");
    CHECK(first > 0 && waitLog(top + "/mv-first.log", "serving on", 20));
    CHECK(rename(dir.c_str(), moved.c_str()) == 0);
    CHECK(mkdir(dir.c_str(), 0700) == 0);
    pid_t second = spawn({server, "serve", "--dir", dir}, top + "/mv-second.log");
    CHECK(second > 0 && waitLog(top + "/mv-second.log", "serving on", 20) && waitAnswering(dir, 20));
    struct stat before {};
    CHECK(lstat((dir + "/server.sock").c_str(), &before) == 0);
    std::string newLog = readFile(dir + "/store/log");
    stopServer(server, moved);
    CHECK(waitExit(first, 20) == 0);
    struct stat after {};
    CHECK(lstat((dir + "/server.sock").c_str(), &after) == 0 && after.st_ino == before.st_ino);
    CHECK(answering(dir) && waitExit(second, 0) == -1);
    CHECK(readFile(dir + "/store/log") == newLog);
    std::printf("moved: the old server stopped; the new one still answers at the old name\n");
    stopServer(server, dir);
    CHECK(waitExit(second, 20) == 0);
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: server_lock <nexus-server> <dir>\n");
        return 2;
    }
    const std::string server = argv[1];
    const std::string top = argv[2];
    std::string out;
    // What an earlier run left: its servers stopped (each directory's), its directory removed.
    for (const char* sub : {"race-0", "race-1", "race-2", "race-3", "new", "killed", "killed-starting", "check", "held",
                            "reader", "linked", "linked-other", "mv", "mv-old"}) {
        run("'" + server + "' stop --dir '" + top + "/" + sub + "'", &out);
    }
    run("rm -rf '" + top + "'", &out);
    CHECK(mkdir(top.c_str(), 0700) == 0);
    const std::string home = top + "/home";
    CHECK(mkdir(home.c_str(), 0700) == 0);
    CHECK(setenv("HOME", home.c_str(), 1) == 0);
    signal(SIGPIPE, SIG_IGN);

    races(server, top);
    newRaced(server, top);
    killed(server, top);
    storeCheck(server, top);
    heldNoSocket(server, top);
    readerHeld(server, top);
    storeLinked(server, top);
    dirMoved(server, top);

    if (failures != 0) {
        std::fprintf(stderr, "server_lock: %d check(s) failed; the directory %s is kept\n", failures, top.c_str());
        return 1;
    }
    run("rm -rf '" + top + "'", &out);
    std::printf("server_lock: all checks passed\n");
    return 0;
}
