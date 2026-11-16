// Functional tests for Layer 4 supervision (`src/server/supervision.{h,cpp}`, label `functional`): real OS processes,
// Unix domain sockets, and advisory flocks in isolated temp directories (never touching ~/.nexus, launchctl, or
// systemctl).

#include "server/builds.h"
#include "server/handover.h"
#include "server/supervision.h"

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
#include <spawn.h>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

extern char** environ;

namespace fs = std::filesystem;
using namespace nexus::server;
using namespace nexus::server::supervision;

namespace {

#define CHECK(cond)                                                                                      \
    do {                                                                                                 \
        if (!(cond)) {                                                                                   \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                         \
            throw std::runtime_error("check failed: " #cond);                                            \
        }                                                                                                \
    } while (0)

std::uint64_t nowMs() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}

void sleepMs(std::uint64_t ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

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

void ensurePrivateServerDir(const fs::path& home, const fs::path& sdir) {
    fs::create_directories(home);
    fs::permissions(home, fs::perms(0700));
    fs::create_directories(sdir.parent_path());
    fs::permissions(sdir.parent_path(), fs::perms(0700));
    fs::create_directories(sdir);
    fs::permissions(sdir, fs::perms(0700));
}

std::string placeFakeBuild(const fs::path& work, const fs::path& serverDir, const std::string& fakeBin,
                           const std::string& mode, const std::string& tag) {
    const fs::path stage = work / ("stage-" + tag);
    removeAllSafe(stage);
    fs::create_directories(stage / "bin");
    fs::create_directories(stage / "drivers/claude/dist");
    fs::copy_file(fakeBin, stage / "bin/nexus-server", fs::copy_options::overwrite_existing);
    fs::permissions(stage / "bin/nexus-server", fs::perms(0755));

    {
        std::ofstream m(stage / "drivers/mode.txt", std::ios::binary);
        m << mode << "\n";
    }
    {
        std::ofstream js(stage / "drivers/claude/dist/main.js", std::ios::binary);
        js << "// tag " << tag << " mode " << mode << "\n";
    }
    {
        std::ofstream fmt(stage / "formats", std::ios::binary);
        fmt << builds::thisBuildFormats();
    }

    std::string hash;
    CHECK(builds::hashTree(stage.string(), &hash).is_ok());
    const fs::path ev = work / ("ev-" + tag);
    {
        std::ofstream evOut(ev, std::ios::binary);
        evOut << builds::evidenceText({hash, "pass", "supervision_functional"});
    }

    builds::Builds b(serverDir.string(), {});
    builds::Result r;
    CHECK(b.place(stage.string(), ev.string(), r) == builds::kExitOk);
    removeAllSafe(stage);
    std::error_code ec;
    fs::remove(ev, ec);
    return hash;
}

bool askSocket(const std::string& sockPath, const std::string& req, std::string* first, std::string* second = nullptr) {
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
    if (::connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof addr) != 0) {
        ::close(fd);
        return false;
    }
    const std::string wire = req + "\n";
    if (::send(fd, wire.data(), wire.size(), 0) != static_cast<ssize_t>(wire.size())) {
        ::close(fd);
        return false;
    }
    auto readOne = [&](std::string* out) {
        out->clear();
        char ch = 0;
        while (out->size() < 2048) {
            pollfd pfd{fd, POLLIN, 0};
            if (::poll(&pfd, 1, 2000) <= 0) {
                return false;
            }
            const ssize_t n = ::read(fd, &ch, 1);
            if (n <= 0) {
                return false;
            }
            if (ch == '\n') {
                if (!out->empty() && out->back() == '\r') {
                    out->pop_back();
                }
                return true;
            }
            out->push_back(ch);
        }
        return false;
    };
    const bool ok1 = readOne(first);
    if (ok1 && second != nullptr && *first == "SERVING") {
        (void)readOne(second);
    }
    ::close(fd);
    return ok1;
}

bool waitForServingBuild(const std::string& sockPath, const std::string& expectedHash, std::uint64_t timeoutMs) {
    const std::uint64_t start = nowMs();
    while (nowMs() - start < timeoutMs) {
        std::string l1;
        std::string l2;
        if (askSocket(sockPath, "status", &l1, &l2)) {
            ServerSeen seen;
            if (parseStatusLines(l1, l2, !l2.empty(), &seen) && seen.serving && !seen.draining &&
                seen.build == expectedHash) {
                return true;
            }
        }
        sleepMs(20);
    }
    return false;
}

struct ChildProc {
    pid_t pid = -1;

    ChildProc() = default;
    ChildProc(const ChildProc&) = delete;
    ChildProc& operator=(const ChildProc&) = delete;

    ~ChildProc() { killAndReap(); }

    void spawn(const std::string& exe, const std::vector<std::string>& args, const std::string& home,
               const std::string& logFile) {
        killAndReap();
        posix_spawn_file_actions_t files;
        posix_spawn_file_actions_init(&files);
        posix_spawn_file_actions_addopen(&files, 0, "/dev/null", O_RDONLY, 0);
        posix_spawn_file_actions_addopen(&files, 1, logFile.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0600);
        posix_spawn_file_actions_adddup2(&files, 1, 2);

        std::vector<std::string> envBuf;
        for (char** ep = environ; ep && *ep; ++ep) {
            const std::string e(*ep);
            if (e.rfind("HOME=", 0) == 0 || e.rfind("NEXUS_SUPERVISE_", 0) == 0) {
                continue;
            }
            envBuf.push_back(e);
        }
        envBuf.push_back("HOME=" + home);
        envBuf.push_back("NEXUS_SUPERVISE_BACKOFF_MS=40");
        envBuf.push_back("NEXUS_SUPERVISE_STABLE_MS=150");

        std::vector<char*> envp;
        for (std::string& s : envBuf) {
            envp.push_back(s.data());
        }
        envp.push_back(nullptr);

        std::vector<std::string> argvBuf;
        argvBuf.push_back(exe);
        for (const std::string& a : args) {
            argvBuf.push_back(a);
        }
        std::vector<char*> argv;
        for (std::string& s : argvBuf) {
            argv.push_back(s.data());
        }
        argv.push_back(nullptr);

        posix_spawnattr_t attr;
        posix_spawnattr_init(&attr);
        posix_spawnattr_setpgroup(&attr, 0);
        posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP);

        pid_t p = -1;
        const int rc = ::posix_spawn(&p, exe.c_str(), &files, &attr, argv.data(), envp.data());
        posix_spawnattr_destroy(&attr);
        posix_spawn_file_actions_destroy(&files);
        CHECK(rc == 0);
        pid = p;
    }

    bool waitExit(int* exitCode, std::uint64_t timeoutMs) {
        if (pid <= 0) {
            return false;
        }
        const std::uint64_t start = nowMs();
        while (nowMs() - start < timeoutMs) {
            int status = 0;
            const pid_t w = ::waitpid(pid, &status, WNOHANG);
            if (w == pid) {
                pid = -1;
                *exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : (128 + WTERMSIG(status));
                return true;
            }
            sleepMs(20);
        }
        return false;
    }

    void terminateCleanly(int* exitCode) {
        if (pid <= 0) {
            return;
        }
        ::kill(pid, SIGTERM);
        if (!waitExit(exitCode, 5000)) {
            killAndReap();
            *exitCode = -1;
        }
    }

    void killAndReap() {
        if (pid > 0) {
            int status = 0;
            if (::waitpid(pid, &status, WNOHANG) == 0) {
                ::kill(-pid, SIGKILL);
                ::kill(pid, SIGKILL);
                while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
                }
            }
            pid = -1;
        }
    }
};

void caseStartAndStop(const std::string& fakeBin, const fs::path& work) {
    const fs::path home = work / "home";
    const fs::path sdir = work / "srv";
    ensurePrivateServerDir(home, sdir);

    const std::string b1 = placeFakeBuild(work, sdir, fakeBin, "good", "1");
    {
        builds::Builds b(sdir.string(), {});
        builds::Result br;
        CHECK(b.makeCurrent(b1, br, false) == builds::kExitOk);
    }

    const std::string supExe = (sdir / "current/bin/nexus-server").string();
    const std::string sock = (sdir / "server.sock").string();
    const std::string supLog = (work / "sup.log").string();

    ChildProc sup;
    sup.spawn(supExe, {"supervise", "--dir", sdir.string()}, home.string(), supLog);
    CHECK(waitForServingBuild(sock, b1, 5000));

    // `supervise status` reports running + serving b1.
    {
        Result sr;
        CHECK(command(sdir.string(), home.string(), {"status"}, {}, sr) == kExitOk);
        CHECK(sr.out.size() == 3);
        CHECK(sr.out[0].find("supervise running build " + b1) == 0);
        CHECK(sr.out[2] == "server serving " + b1);
    }

    // Intentional `stop` on server.sock: server exits 0, supervisor records `stopped` and exits 0.
    std::string reply;
    CHECK(askSocket(sock, "stop", &reply) && reply == "OK");
    int code = -1;
    CHECK(sup.waitExit(&code, 5000));
    CHECK(code == kExitOk);

    // A restarted supervisor honors `stopped = 1` on disk and stays stopped (exits 0 immediately).
    ChildProc sup2;
    sup2.spawn(supExe, {"supervise", "--dir", sdir.string()}, home.string(), supLog);
    CHECK(sup2.waitExit(&code, 3000));
    CHECK(code == kExitOk);
    CHECK(!askSocket(sock, "status", &reply));

    // `supervise --start` clears `stopped` and brings b1 back to SERVING; SIGTERM on supervisor shuts down cleanly
    // without setting `stopped = 1` on disk.
    ChildProc sup3;
    sup3.spawn(supExe, {"supervise", "--start", "--dir", sdir.string()}, home.string(), supLog);
    CHECK(waitForServingBuild(sock, b1, 5000));
    sup3.terminateCleanly(&code);
    CHECK(code == kExitOk);

    // Next boot (`supervise` without `--start`) starts b1 again because SIGTERM did not mark `stopped = 1`!
    ChildProc sup4;
    sup4.spawn(supExe, {"supervise", "--dir", sdir.string()}, home.string(), supLog);
    CHECK(waitForServingBuild(sock, b1, 5000));
    sup4.terminateCleanly(&code);
    CHECK(code == kExitOk);
}

void caseDrainHandover(const std::string& fakeBin, const fs::path& work) {
    const fs::path home = work / "home";
    const fs::path sdir = work / "srv";
    ensurePrivateServerDir(home, sdir);

    const std::string b1 = placeFakeBuild(work, sdir, fakeBin, "good", "1");
    const std::string b2 = placeFakeBuild(work, sdir, fakeBin, "good", "2");
    {
        builds::Builds b(sdir.string(), {});
        builds::Result br;
        CHECK(b.makeCurrent(b1, br, false) == builds::kExitOk);
    }

    const std::string supExe = (sdir / "current/bin/nexus-server").string();
    const std::string sock = (sdir / "server.sock").string();
    const std::string supLog = (work / "sup.log").string();

    ChildProc sup;
    sup.spawn(supExe, {"supervise", "--dir", sdir.string()}, home.string(), supLog);
    CHECK(waitForServingBuild(sock, b1, 5000));

    // Open a turn on b1, run concurrent handover to b2 while finishing the turn cleanly.
    std::string reply;
    CHECK(askSocket(sock, "turn open", &reply) && reply == "OK");

    std::thread finisher([&] {
        sleepMs(80);
        std::string r;
        (void)askSocket(sock, "turn end", &r);
    });

    handover::Result hr;
    CHECK(handover::command(sdir.string(), {b2}, {}, hr) == handover::kExitOk);
    finisher.join();

    // Supervisor stayed alive across the drain (exit 75), adopted b2, and durable store entry is intact!
    CHECK(sup.pid > 0);
    int dummy = 0;
    CHECK(!sup.waitExit(&dummy, 80));
    CHECK(waitForServingBuild(sock, b2, 5000));
    CHECK(askSocket(sock, "entries", &reply) && reply == "1");

    // Now crash the adopted b2 (`crash` on server.sock): supervisor detects adopted crash and respawns b2!
    CHECK(askSocket(sock, "crash", &reply) && reply == "OK");
    CHECK(waitForServingBuild(sock, b2, 5000));

    int code = -1;
    sup.terminateCleanly(&code);
    CHECK(code == kExitOk);
}

void casePostStartRollback(const std::string& fakeBin, const fs::path& work) {
    const fs::path home = work / "home";
    const fs::path sdir = work / "srv";
    ensurePrivateServerDir(home, sdir);

    // b1 is healthy; bLate reaches SERVING (so Layer 3 handover succeeds!), then crashes 150 ms later!
    const std::string b1 = placeFakeBuild(work, sdir, fakeBin, "good", "1");
    const std::string bLate = placeFakeBuild(work, sdir, fakeBin, "crash-after-serve", "2");
    {
        builds::Builds b(sdir.string(), {});
        builds::Result br;
        CHECK(b.makeCurrent(b1, br, false) == builds::kExitOk);
    }

    const std::string supExe = (sdir / "current/bin/nexus-server").string();
    const std::string sock = (sdir / "server.sock").string();
    const std::string supLog = (work / "sup.log").string();

    ChildProc sup;
    sup.spawn(supExe, {"supervise", "--dir", sdir.string()}, home.string(), supLog);
    CHECK(waitForServingBuild(sock, b1, 5000));

    // Handover to bLate succeeds because bLate binds server.sock and answers SERVING!
    handover::Result hr;
    CHECK(handover::command(sdir.string(), {bLate}, {}, hr) == handover::kExitOk);

    // bLate then crashes post-start; supervisor retries once, sees second post-start crash, acquires handover lock,
    // shuns bLate, switches builds/active back to b1, and brings b1 to SERVING!
    const std::uint64_t deadline = nowMs() + 8000;
    bool rolledBackAndServing = false;
    while (nowMs() < deadline) {
        builds::Builds b(sdir.string(), {});
        std::string cur;
        std::string prev;
        builds::Result br;
        if (b.active(&cur, &prev, br) == builds::kExitOk && cur == b1 && prev == bLate) {
            if (waitForServingBuild(sock, b1, 1000)) {
                rolledBackAndServing = true;
                break;
            }
        }
        sleepMs(40);
    }
    CHECK(rolledBackAndServing);

    // Confirm supervise status shows shunned == 1 and b1 stably serving.
    Result sr;
    CHECK(command(sdir.string(), home.string(), {"status"}, {}, sr) == kExitOk);
    CHECK(sr.out.size() == 3);
    CHECK(sr.out[0].find("shunned 1") != std::string::npos);
    CHECK(sr.out[1] == "current " + b1 + " previous " + bLate);
    CHECK(sr.out[2] == "server serving " + b1);

    int code = -1;
    sup.terminateCleanly(&code);
    CHECK(code == kExitOk);
}

void caseDualFailHalt(const std::string& fakeBin, const fs::path& work) {
    const fs::path home = work / "home";
    const fs::path sdir = work / "srv";
    ensurePrivateServerDir(home, sdir);

    // Both placed builds crash after reaching SERVING!
    const std::string bLate1 = placeFakeBuild(work, sdir, fakeBin, "crash-after-serve", "1");
    const std::string bLate2 = placeFakeBuild(work, sdir, fakeBin, "crash-after-serve", "2");
    {
        builds::Builds b(sdir.string(), {});
        builds::Result br;
        CHECK(b.makeCurrent(bLate1, br, false) == builds::kExitOk);
        CHECK(b.makeCurrent(bLate2, br, false) == builds::kExitOk);
    }

    const std::string supExe = (sdir / "current/bin/nexus-server").string();
    const std::string supLog = (work / "sup.log").string();

    ChildProc sup;
    sup.spawn(supExe, {"supervise", "--dir", sdir.string()}, home.string(), supLog);
    int code = -1;
    CHECK(sup.waitExit(&code, 12000));
    CHECK(code == kExitHalted);

    // Rolled back once to bLate1 (shunning bLate2), never ping-ponged back to bLate2, and halted!
    builds::Builds b(sdir.string(), {});
    std::string cur;
    std::string prev;
    builds::Result br;
    CHECK(b.active(&cur, &prev, br) == builds::kExitOk);
    CHECK(cur == bLate1);
    CHECK(prev == bLate2);

    // A second supervisor invocation reads `halted = 1` and exits 78 immediately.
    ChildProc sup2;
    sup2.spawn(supExe, {"supervise", "--dir", sdir.string()}, home.string(), supLog);
    CHECK(sup2.waitExit(&code, 2000));
    CHECK(code == kExitHalted);
}

int runOfflineTool(const std::string& prog, const std::vector<std::string>& args) {
    std::vector<std::string> argvBuf;
    argvBuf.push_back(prog);
    for (const std::string& a : args) {
        argvBuf.push_back(a);
    }
    std::vector<char*> argv;
    for (std::string& s : argvBuf) {
        argv.push_back(s.data());
    }
    argv.push_back(nullptr);
    pid_t pid = -1;
    if (::posix_spawnp(&pid, prog.c_str(), nullptr, nullptr, argv.data(), environ) != 0) {
        return -1;
    }
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}

void caseUnitsAndLocks(const std::string& fakeBin, const fs::path& work) {
    const fs::path home = work / "home";
    const fs::path sdir = home / ".nexus/server";
    const fs::path unitScratch = work / "offline-units";
    ensurePrivateServerDir(home, sdir);
    fs::create_directories(unitScratch);

    const std::string b1 = placeFakeBuild(work, sdir, fakeBin, "good", "1");
    {
        builds::Builds b(sdir.string(), {});
        builds::Result br;
        CHECK(b.makeCurrent(b1, br, false) == builds::kExitOk);
    }

    // 1. Deterministic systemd unit generation + offline syntax check in isolated temp dir.
    Result ur;
    CHECK(command(sdir.string(), home.string(),
                  {"unit", "--kind", "systemd", "--home", home.string(), "--path", "/usr/local/bin:/usr/bin:/bin"}, {},
                  ur) == kExitOk);
    std::string unitText;
    for (const std::string& l : ur.out) {
        unitText += l + "\n";
    }
    CHECK(unitText.find("Restart=on-failure\n") != std::string::npos);
    CHECK(unitText.find("RestartPreventExitStatus=3 78\n") != std::string::npos);
    CHECK(unitText.find(" supervise --dir ") != std::string::npos);

    const fs::path unitFile = unitScratch / "nexus-server.service";
    {
        std::ofstream out(unitFile, std::ios::binary);
        out << unitText;
    }
#if defined(__linux__)
    // Offline verification of the generated unit file when systemd-analyze is installed (never touches systemctl).
    if (::access("/usr/bin/systemd-analyze", X_OK) == 0 || ::access("/bin/systemd-analyze", X_OK) == 0) {
        const int vrc = runOfflineTool("systemd-analyze", {"--man=no", "verify", unitFile.string()});
        CHECK(vrc == 0);
    }
#endif

    // 2. Deterministic launchd plist generation + offline plutil lint on macOS.
    Result pr;
    CHECK(command(sdir.string(), home.string(),
                  {"unit", "--kind", "launchd", "--home", home.string(), "--path", "/usr/local/bin:/usr/bin:/bin"}, {},
                  pr) == kExitOk);
    std::string plistText;
    for (const std::string& l : pr.out) {
        plistText += l + "\n";
    }
    CHECK(plistText.find("<string>com.robotgamedevelopers.nexus-server</string>") != std::string::npos);
    CHECK(plistText.find("<string>supervise</string>") != std::string::npos);
    const fs::path plistFile = unitScratch / "com.robotgamedevelopers.nexus-server.plist";
    {
        std::ofstream out(plistFile, std::ios::binary);
        out << plistText;
    }
#if defined(__APPLE__)
    if (::access("/usr/bin/plutil", X_OK) == 0) {
        CHECK(runOfflineTool("/usr/bin/plutil", {"-lint", "-s", plistFile.string()}) == 0);
    }
#endif

    // 3. unit-path and help.
    Result pathRes;
    CHECK(command(sdir.string(), home.string(), {"unit-path", "--kind", "systemd", "--home", home.string()}, {},
                  pathRes) == kExitOk);
    CHECK(pathRes.out.size() == 4);
    CHECK(pathRes.out[1] == "nexus-server.service");

    Result helpRes;
    CHECK(command(sdir.string(), home.string(), {"help"}, {}, helpRes) == kExitOk);
    CHECK(!helpRes.out.empty());

    // 4. Concurrent second supervisor gets kExitBusy (4).
    const std::string supExe = (sdir / "current/bin/nexus-server").string();
    const std::string sock = (sdir / "server.sock").string();
    ChildProc sup;
    sup.spawn(supExe, {"supervise", "--dir", sdir.string()}, home.string(), (work / "sup.log").string());
    CHECK(waitForServingBuild(sock, b1, 5000));

    Options busyOpts;
    busyOpts.lock_wait_ms = 60;
    Result busyRes;
    CHECK(command(sdir.string(), home.string(), {}, busyOpts, busyRes) == kExitBusy);

    int code = -1;
    sup.terminateCleanly(&code);
    CHECK(code == kExitOk);

    // 5. Unlockable filesystem (NFS / unkept locks) refused with kExitFailed (1).
    Options nfsOpts;
    nfsOpts.test_fs_type = "nfs";
    Result nfsRes;
    CHECK(command((work / "nfs-srv").string(), home.string(), {}, nfsOpts, nfsRes) == kExitFailed);

    Options unkeptOpts;
    unkeptOpts.test_locks_ignored = true;
    Result unkeptRes;
    CHECK(command((work / "unkept-srv").string(), home.string(), {}, unkeptOpts, unkeptRes) == kExitFailed);
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 4) {
        std::fprintf(stderr, "usage: supervision_functional <fake_build_server> <case> <work_dir>\n");
        return 2;
    }
    const std::string fakeBin = argv[1];
    const std::string which = argv[2];
    const fs::path work = argv[3];

    removeAllSafe(work);
    int rc = 0;
    try {
        if (which == "start_and_stop") {
            caseStartAndStop(fakeBin, work);
        } else if (which == "drain_handover") {
            caseDrainHandover(fakeBin, work);
        } else if (which == "post_start_rollback") {
            casePostStartRollback(fakeBin, work);
        } else if (which == "dual_fail_halt") {
            caseDualFailHalt(fakeBin, work);
        } else if (which == "units_and_locks") {
            caseUnitsAndLocks(fakeBin, work);
        } else {
            std::fprintf(stderr, "unknown case `%s`\n", which.c_str());
            rc = 2;
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "supervision_functional (%s) failed: %s\n", which.c_str(), e.what());
        rc = 1;
    }
    removeAllSafe(work);
    if (rc == 0) {
        std::printf("supervision_functional (%s): passed\n", which.c_str());
    }
    return rc;
}
