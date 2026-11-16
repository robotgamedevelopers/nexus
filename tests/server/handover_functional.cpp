// Functional tests of handover (src/server/handover.h, src/server/drain.h) over real placed builds (`Builds`), real
// Unix control sockets, real OS locks (`PosixFs` + `Store` + `Builds::holdBuild`), and real child processes.
//
// Usage: handover_functional <fake_build_server> <case> <dir>

#include "server/builds.h"
#include "server/drain.h"
#include "server/handover.h"
#include "store/fs.h"

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <poll.h>
#include <sstream>
#include <string>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;
using namespace nexus::server;
using namespace nexus::server::handover;

namespace {

int gFailures = 0;

#define CHECK(cond)                                                                              \
    do {                                                                                         \
        if (!(cond)) {                                                                           \
            std::fprintf(stderr, "handover_functional FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++gFailures;                                                                         \
        }                                                                                        \
    } while (0)

void makeWritableTree(const fs::path& p) {
    std::error_code ec;
    const fs::file_status st = fs::symlink_status(p, ec);
    if (ec || st.type() == fs::file_type::not_found || st.type() == fs::file_type::symlink) {
        return;
    }
    if (st.type() == fs::file_type::directory) {
        fs::permissions(p, fs::perms::owner_all, fs::perm_options::add, ec);
        for (fs::directory_iterator it(p, ec), end; !ec && it != end; it.increment(ec)) {
            makeWritableTree(it->path());
        }
    } else if (st.type() == fs::file_type::regular) {
        fs::permissions(p, fs::perms::owner_write, fs::perm_options::add, ec);
    }
}

void removeAllSafe(const fs::path& root) {
    std::error_code ec;
    if (fs::symlink_status(root, ec).type() == fs::file_type::not_found) {
        return;
    }
    makeWritableTree(root);
    fs::remove_all(root, ec);
}

bool askSocket(const std::string& sockPath, const std::string& req, std::vector<std::string>* lines) {
    lines->clear();
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (sockPath.size() >= sizeof addr.sun_path) {
        return false;
    }
    std::memcpy(addr.sun_path, sockPath.data(), sockPath.size());
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        return false;
    }
#ifdef SO_NOSIGPIPE
    const int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
    if (::connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof addr) != 0) {
        ::close(fd);
        return false;
    }
    const std::string wire = req + "\n";
    int flags = 0;
#ifdef MSG_NOSIGNAL
    flags |= MSG_NOSIGNAL;
#endif
    if (::send(fd, wire.data(), wire.size(), flags) != static_cast<ssize_t>(wire.size())) {
        ::close(fd);
        return false;
    }
    std::string raw;
    char buf[512];
    for (;;) {
        pollfd pfd{fd, POLLIN, 0};
        if (::poll(&pfd, 1, 2000) <= 0) {
            break;
        }
        const ssize_t n = ::read(fd, buf, sizeof buf);
        if (n <= 0) {
            break;
        }
        raw.append(buf, static_cast<std::size_t>(n));
    }
    ::close(fd);
    std::istringstream in(raw);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        lines->push_back(line);
    }
    return !lines->empty();
}

void stopAnyServer(const std::string& serverDir) {
    std::vector<std::string> lines;
    (void)askSocket(serverDir + "/server.sock", "stop", &lines);
    for (int i = 0; i < 100 && ::access((serverDir + "/server.sock").c_str(), F_OK) == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
    }
}

std::string placeFakeBuild(const fs::path& work, const std::string& serverDir, const std::string& fakeBin,
                           const std::string& tag, const std::string& mode, bool tested = true) {
    const fs::path stage = work / ("stage-" + tag);
    removeAllSafe(stage);
    fs::create_directories(stage / "bin");
    fs::create_directories(stage / "drivers");
    fs::copy_file(fakeBin, stage / "bin/nexus-server", fs::copy_options::overwrite_existing);
    fs::permissions(stage / "bin/nexus-server", fs::perms(0755));
    {
        std::ofstream m(stage / "drivers/mode.txt", std::ios::binary);
        m << mode << "\n" << tag << "\n";
    }
    fs::permissions(stage / "drivers/mode.txt", fs::perms(0644));
    {
        std::ofstream f(stage / "formats", std::ios::binary);
        f << builds::thisBuildFormats();
    }
    fs::permissions(stage / "formats", fs::perms(0644));

    std::string h;
    CHECK(builds::hashTree(stage.string(), &h).is_ok());
    std::string evPath;
    if (tested) {
        const fs::path ep = work / ("ev-" + tag);
        std::ofstream e(ep, std::ios::binary);
        e << builds::evidenceText({h, "pass", "handover_functional"});
        e.close();
        evPath = ep.string();
    }
    builds::Builds b(serverDir, {});
    builds::Result r;
    CHECK(b.place(stage.string(), evPath, r) == builds::kExitOk);
    return h;
}

void caseDrainIdleAndTurn(const fs::path& work, const std::string& serverDir, const std::string& fakeBin) {
    const std::string b1 = placeFakeBuild(work, serverDir, fakeBin, "b1", "good");
    const std::string b2 = placeFakeBuild(work, serverDir, fakeBin, "b2", "good");
    {
        builds::Builds b(serverDir, {});
        builds::Result br;
        CHECK(b.makeCurrent(b1, br) == builds::kExitOk);
    }

    // Cold start B1 via `handover resume`.
    {
        Result r;
        CHECK(command(serverDir, {"resume"}, {}, r) == kExitOk);
    }
    std::vector<std::string> st;
    CHECK(askSocket(serverDir + "/server.sock", "status", &st) && st.size() == 2 && st[0] == "SERVING" &&
          st[1] == "build " + b1);

    // 1. Idle handover B1 -> B2.
    {
        Result r;
        CHECK(command(serverDir, {b2}, {}, r) == kExitOk);
    }
    CHECK(askSocket(serverDir + "/server.sock", "status", &st) && st.size() == 2 && st[0] == "SERVING" &&
          st[1] == "build " + b2);
    CHECK(!fs::exists(fs::path(serverDir) / "handover/state"));

    // 2. Open turn on B2: handover B2 -> B1 waits for the turn to commit and finish, never cutting it!
    CHECK(askSocket(serverDir + "/server.sock", "turn open", &st) && st[0] == "OK");

    const pid_t child = ::fork();
    CHECK(child >= 0);
    if (child == 0) {
        Result r;
        const int rc = command(serverDir, {b1}, {}, r);
        ::_exit(rc);
    }

    // Wait until B2 reports `draining` on `status`.
    bool sawDraining = false;
    for (int i = 0; i < 200; ++i) {
        if (askSocket(serverDir + "/server.sock", "status", &st) && st.size() == 2 &&
            st[1] == "build " + b2 + " draining") {
            sawDraining = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
    }
    CHECK(sawDraining);

    // Child handover must STILL be waiting while the turn is open.
    int wstatus = 0;
    CHECK(::waitpid(child, &wstatus, WNOHANG) == 0);

    // Finish the turn (commits 1 durable transcript entry to store/), allowing B2 to exit 75 and B1 to serve.
    CHECK(askSocket(serverDir + "/server.sock", "turn end", &st) && st[0] == "OK");
    CHECK(::waitpid(child, &wstatus, 0) == child);
    CHECK(WIFEXITED(wstatus) && WEXITSTATUS(wstatus) == kExitOk);

    CHECK(askSocket(serverDir + "/server.sock", "status", &st) && st.size() == 2 && st[0] == "SERVING" &&
          st[1] == "build " + b1);
    // Verify the entry committed before drain finished survived in `<serverDir>/store` on B1!
    CHECK(askSocket(serverDir + "/server.sock", "entries", &st) && st[0] == "1");
}

void caseDrainDeadlineAndLegacy(const fs::path& work, const std::string& serverDir, const std::string& fakeBin) {
    ::setenv(kDrainLimitVariable, "150", 1);
    const std::string b1 = placeFakeBuild(work, serverDir, fakeBin, "b1", "good");
    const std::string b2 = placeFakeBuild(work, serverDir, fakeBin, "b2", "good");
    const std::string bLeg = placeFakeBuild(work, serverDir, fakeBin, "bleg", "legacy");

    {
        builds::Builds b(serverDir, {});
        builds::Result br;
        CHECK(b.makeCurrent(b1, br) == builds::kExitOk);
    }
    {
        Result r;
        CHECK(command(serverDir, {"resume"}, {}, r) == kExitOk);
    }

    // Open a turn that never finishes; NEXUS_DRAIN_MS=150 ensures B1 exits after 150 ms and B2 serves.
    std::vector<std::string> st;
    CHECK(askSocket(serverDir + "/server.sock", "turn open", &st) && st[0] == "OK");
    {
        Result r;
        CHECK(command(serverDir, {b2}, {}, r) == kExitOk);
    }
    ::unsetenv(kDrainLimitVariable);
    CHECK(askSocket(serverDir + "/server.sock", "status", &st) && st.size() == 2 && st[1] == "build " + b2);

    // Hand over to legacy server, then from legacy server back to B1 (tests 1-line status + stop fallback).
    stopAnyServer(serverDir);
    {
        builds::Builds b(serverDir, {});
        builds::Result br;
        CHECK(b.makeCurrent(bLeg, br) == builds::kExitOk);
    }
    // Start legacy server manually, verify 1-line status, then run `handover b1`.
    const std::string prog = serverDir + "/current/bin/nexus-server";
    const pid_t legPid = ::fork();
    if (legPid == 0) {
        ::execl(prog.c_str(), prog.c_str(), "serve", "--dir", serverDir.c_str(), static_cast<char*>(nullptr));
        ::_exit(1);
    }
    for (int i = 0; i < 200; ++i) {
        if (askSocket(serverDir + "/server.sock", "status", &st) && st.size() == 1 && st[0] == "SERVING") {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
    }
    CHECK(st.size() == 1 && st[0] == "SERVING");

    {
        Result r;
        CHECK(command(serverDir, {b1}, {}, r) == kExitOk);
    }
    int legStatus = 0;
    (void)::waitpid(legPid, &legStatus, 0);
    CHECK(askSocket(serverDir + "/server.sock", "status", &st) && st.size() == 2 && st[1] == "build " + b1);
}

void caseRollbackCrashAndHang(const fs::path& work, const std::string& serverDir, const std::string& fakeBin) {
    const std::string b1 = placeFakeBuild(work, serverDir, fakeBin, "b1", "good");
    const std::string bCrash = placeFakeBuild(work, serverDir, fakeBin, "bcrash", "crash");
    const std::string bHang = placeFakeBuild(work, serverDir, fakeBin, "bhang", "hang");

    {
        builds::Builds b(serverDir, {});
        builds::Result br;
        CHECK(b.makeCurrent(b1, br) == builds::kExitOk);
    }
    {
        Result r;
        CHECK(command(serverDir, {"resume"}, {}, r) == kExitOk);
    }

    // 1. Target crashes on startup -> rolls back to B1, B1 answers SERVING, returns kExitRolledBack (5).
    {
        Options opts;
        opts.start_wait_ms = 2000;
        Result r;
        CHECK(command(serverDir, {bCrash}, opts, r) == kExitRolledBack);
    }
    std::vector<std::string> st;
    CHECK(askSocket(serverDir + "/server.sock", "status", &st) && st.size() == 2 && st[1] == "build " + b1);
    CHECK(!fs::exists(fs::path(serverDir) / "handover/state"));

    // 2. Target hangs holding the store lock without binding server.sock -> killed after start_wait_ms, rolled back to
    //    B1, B1 acquires the store lock and answers SERVING!
    {
        Options opts;
        opts.start_wait_ms = 300;
        Result r;
        CHECK(command(serverDir, {bHang}, opts, r) == kExitRolledBack);
    }
    CHECK(askSocket(serverDir + "/server.sock", "status", &st) && st.size() == 2 && st[1] == "build " + b1);
    CHECK(!fs::exists(fs::path(serverDir) / "handover/state"));
}

void caseCrashEveryStep(const fs::path& work, const std::string& serverDir, const std::string& fakeBin) {
    const std::string b1 = placeFakeBuild(work, serverDir, fakeBin, "b1", "good");
    const std::string b2 = placeFakeBuild(work, serverDir, fakeBin, "b2", "good");
    const std::string bCrash = placeFakeBuild(work, serverDir, fakeBin, "bcrash", "crash");

    for (const std::string& target : {b2, bCrash}) {
        // Reset cleanly to B1 serving and collect the step count first.
        stopAnyServer(serverDir);
        fs::remove_all(fs::path(serverDir) / "handover");
        {
            builds::Builds b(serverDir, {});
            builds::Result br;
            CHECK(b.makeCurrent(b1, br) == builds::kExitOk);
        }
        {
            Result r;
            CHECK(command(serverDir, {"resume"}, {}, r) == kExitOk);
        }

        std::vector<std::string> steps;
        {
            Options opts;
            opts.start_wait_ms = 1500;
            opts.hook = [&](const std::string& s) {
                if (s != "idle") {
                    steps.push_back(s);
                }
            };
            Result r;
            (void)command(serverDir, {target}, opts, r);
        }
        CHECK(!steps.empty());

        for (std::size_t cut = 0; cut < steps.size(); ++cut) {
            // Reset to B1 serving before each cut point.
            stopAnyServer(serverDir);
            fs::remove_all(fs::path(serverDir) / "handover");
            {
                builds::Builds b(serverDir, {});
                builds::Result br;
                CHECK(b.makeCurrent(b1, br) == builds::kExitOk);
            }
            {
                Result r;
                CHECK(command(serverDir, {"resume"}, {}, r) == kExitOk);
            }

            // Fork a child that crashes abruptly (_exit(99)) at step `cut`.
            const pid_t pid = ::fork();
            CHECK(pid >= 0);
            if (pid == 0) {
                std::size_t seen = 0;
                Options opts;
                opts.start_wait_ms = 1500;
                opts.hook = [&](const std::string& s) {
                    if (s != "idle" && seen++ == cut) {
                        // Also plant a leftover `.state-torn` when past lock to test leftover cleanup on real disk.
                        if (seen > 1) {
                            std::ofstream torn(serverDir + "/handover/.state-torn", std::ios::binary);
                            torn << "partial";
                        }
                        ::_exit(99);
                    }
                };
                Result r;
                const int rc = command(serverDir, {target}, opts, r);
                ::_exit(rc);
            }
            int wstatus = 0;
            CHECK(::waitpid(pid, &wstatus, 0) == pid);
            CHECK(WIFEXITED(wstatus) && WEXITSTATUS(wstatus) == 99);

            // Resume in this process: must recover to a working build (B1 or B2), remove state & leftovers, exit 0!
            Options resOpts;
            resOpts.start_wait_ms = 2000;
            Result rr;
            CHECK(command(serverDir, {"resume"}, resOpts, rr) == kExitOk);
            CHECK(!fs::exists(fs::path(serverDir) / "handover/state"));
            CHECK(!fs::exists(fs::path(serverDir) / "handover/.state-torn"));

            std::string cur;
            std::string prev;
            {
                builds::Builds b(serverDir, {});
                builds::Result br;
                CHECK(b.active(&cur, &prev, br) == builds::kExitOk);
            }
            CHECK(cur == b1 || cur == b2);
            std::vector<std::string> st;
            CHECK(askSocket(serverDir + "/server.sock", "status", &st) && st.size() == 2 && st[0] == "SERVING" &&
                  st[1] == "build " + cur);
        }
    }
}

void caseConcurrentAndLocks(const fs::path& work, const std::string& serverDir, const std::string& fakeBin) {
    const std::string b1 = placeFakeBuild(work, serverDir, fakeBin, "b1", "good");
    const std::string b2 = placeFakeBuild(work, serverDir, fakeBin, "b2", "good");
    const std::string bUntested = placeFakeBuild(work, serverDir, fakeBin, "buntested", "good", false);

    {
        builds::Builds b(serverDir, {});
        builds::Result br;
        CHECK(b.makeCurrent(b1, br) == builds::kExitOk);
    }
    {
        Result r;
        CHECK(command(serverDir, {"resume"}, {}, r) == kExitOk);
    }

    // 1. Untested target refused with kExitRefused (3).
    {
        Result r;
        CHECK(command(serverDir, {bUntested}, {}, r) == kExitRefused);
    }

    // 2. Unlockable filesystem seams refused with kExitFailed (1).
    {
        Options nfsOpts;
        nfsOpts.test_fs_type = "nfs";
        Result r;
        CHECK(command(serverDir, {b2}, nfsOpts, r) == kExitFailed);
    }
    {
        Options ignOpts;
        ignOpts.test_locks_ignored = true;
        Result r;
        CHECK(command(serverDir, {b2}, ignOpts, r) == kExitFailed);
    }

    // 3. Busy lock returns kExitBusy (4) when held past lock_wait_ms.
    {
        fs::create_directories(serverDir + "/handover");
        const int fd = ::open((serverDir + "/handover").c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        CHECK(fd >= 0);
        CHECK(::flock(fd, LOCK_EX | LOCK_NB) == 0);
        const pid_t pid = ::fork();
        if (pid == 0) {
            Options busyOpts;
            busyOpts.lock_wait_ms = 80;
            Result r;
            ::_exit(command(serverDir, {b2}, busyOpts, r));
        }
        int ws = 0;
        CHECK(::waitpid(pid, &ws, 0) == pid);
        CHECK(WIFEXITED(ws) && WEXITSTATUS(ws) == kExitBusy);
        ::flock(fd, LOCK_UN);
        ::close(fd);
    }

    // 4. Two concurrent child processes switching B1 and B2 over 4 rounds.
    for (int round = 0; round < 4; ++round) {
        const pid_t p1 = ::fork();
        if (p1 == 0) {
            Result r;
            ::_exit(command(serverDir, {b2}, {}, r));
        }
        const pid_t p2 = ::fork();
        if (p2 == 0) {
            Result r;
            ::_exit(command(serverDir, {b1}, {}, r));
        }
        int s1 = 0;
        int s2 = 0;
        CHECK(::waitpid(p1, &s1, 0) == p1 && WIFEXITED(s1) && WEXITSTATUS(s1) == kExitOk);
        CHECK(::waitpid(p2, &s2, 0) == p2 && WIFEXITED(s2) && WEXITSTATUS(s2) == kExitOk);
    }

    std::string cur;
    std::string prev;
    {
        builds::Builds b(serverDir, {});
        builds::Result br;
        CHECK(b.active(&cur, &prev, br) == builds::kExitOk);
    }
    std::vector<std::string> st;
    CHECK(askSocket(serverDir + "/server.sock", "status", &st) && st.size() == 2 && st[1] == "build " + cur);

    // 5. `status` and `help`.
    {
        Result r;
        CHECK(command(serverDir, {"status"}, {}, r) == kExitOk);
        CHECK(r.out.size() == 3 && r.out[0] == "handover idle");
    }
    for (const std::string& hv : {std::string(), std::string("to"), std::string("resume"), std::string("status")}) {
        Result r;
        const std::vector<std::string> args = hv.empty() ? std::vector<std::string>{"help"}
                                                         : std::vector<std::string>{"help", hv};
        CHECK(command(serverDir, args, {}, r) == kExitOk);
        CHECK(!r.out.empty());
    }
}

} // namespace

int main(int argc, char** argv) {
    ::signal(SIGPIPE, SIG_IGN);
    if (argc != 4) {
        std::fprintf(stderr, "usage: handover_functional <fake_build_server> <case> <dir>\n");
        return 2;
    }
    const std::string fakeBin = fs::absolute(argv[1]).string();
    const std::string which = argv[2];
    const fs::path root = fs::absolute(argv[3]);
    const std::string serverDir = (root / "s").string();

    stopAnyServer(serverDir);
    removeAllSafe(root);
    fs::create_directories(serverDir);
    fs::permissions(root, fs::perms(0700));
    fs::permissions(serverDir, fs::perms(0700));

    struct CleanGuard {
        fs::path r;
        std::string s;
        ~CleanGuard() {
            stopAnyServer(s);
            removeAllSafe(r);
        }
    } guard{root, serverDir};

    if (which == "drain_idle_and_turn") {
        caseDrainIdleAndTurn(root, serverDir, fakeBin);
    } else if (which == "drain_deadline_and_legacy") {
        caseDrainDeadlineAndLegacy(root, serverDir, fakeBin);
    } else if (which == "rollback_crash_and_hang") {
        caseRollbackCrashAndHang(root, serverDir, fakeBin);
    } else if (which == "crash_every_step") {
        caseCrashEveryStep(root, serverDir, fakeBin);
    } else if (which == "concurrent_and_locks") {
        caseConcurrentAndLocks(root, serverDir, fakeBin);
    } else {
        std::fprintf(stderr, "handover_functional: unknown case `%s`\n", which.c_str());
        return 2;
    }

    if (gFailures != 0) {
        std::fprintf(stderr, "handover_functional (%s): %d failures\n", which.c_str(), gFailures);
        return 1;
    }
    std::printf("handover_functional (%s): passed\n", which.c_str());
    return 0;
}
