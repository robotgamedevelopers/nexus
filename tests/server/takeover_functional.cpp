// End-to-end multi-process functional tests across all 5 takeover layers (label `functional`):
//   1. e2e_migration_handover_drain: live old server on ~/.nexus/server.sock refuses supervise without halting ->
//      old server stops -> supervise migrates ~/.nexus -> ~/.nexus/server once and serves Build 1 (3/3) with migrated
//      session intact -> open turn drained without loss into Build 2 (5/3) -> compatible format-5 write -> downgrade
//      back to Build 1 (3/3) -> intentional stop honored across supervisor restarts.
//   2. e2e_post_start_crash_format5_compat_rollback: Build 1 (3/3) -> handover to Build 3a (5/5, crashes after
//      SERVING before writing format 5) -> supervise inspects disk store via Builds::rollbackTo, rolls back to
//      Build 1 cleanly, shuns Build 3a, and serves Build 1.
//   3. e2e_breaking_format5_refuses_rollback_and_halts: Build 1 (3/3) -> handover to Build 3b (5/5) -> writes
//      breaking reader = 5 to <dir>/store -> repeated post-start crashes -> Builds::rollbackTo refuses rolling back
//      to Build 1, supervise shuns Build 1 without spinning, and halts cleanly with exit 78 (EX_CONFIG).

#include "server/builds.h"
#include "server/handover.h"
#include "server/supervise_state.h"
#include "server/supervision.h"
#include "store/fs.h"
#include "store/store.h"

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
#include <sstream>
#include <stdexcept>
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
namespace store = nexus::store;
using namespace nexus::server;

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

std::string formatsBlock(std::uint32_t writesRec, std::uint32_t readerRec, std::uint32_t writesSnap,
                         std::uint32_t readerSnap) {
    return builds::formatsText({
        {"store-record", writesRec, readerRec},
        {"store-snapshot", writesSnap, readerSnap},
        {"config", 1, 1},
        {"link", 1, 1},
        {"driver-protocol", 1, 1},
    });
}

std::string placeFakeBuild(const fs::path& work, const fs::path& serverDir, const std::string& fakeBin,
                           const std::string& mode, int writesRec, int readerRec, const std::string& tag) {
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
        std::ofstream sr(stage / "drivers/store_readers.txt", std::ios::binary);
        sr << writesRec << " " << (writesRec >= 5 ? 5 : 4) << "\n";
    }
    {
        std::ofstream js(stage / "drivers/claude/dist/main.js", std::ios::binary);
        js << "// tag " << tag << " mode " << mode << "\n";
    }
    {
        std::ofstream fmt(stage / "formats", std::ios::binary);
        fmt << formatsBlock(static_cast<std::uint32_t>(writesRec), static_cast<std::uint32_t>(readerRec),
                            writesRec >= 5 ? 5u : 4u, readerRec >= 5 ? 5u : 4u);
    }

    std::string hash;
    CHECK(builds::hashTree(stage.string(), &hash).is_ok());
    const fs::path ev = work / ("ev-" + tag);
    {
        std::ofstream evOut(ev, std::ios::binary);
        evOut << builds::evidenceText({hash, "pass", "takeover_functional " + tag});
    }

    builds::Builds b(serverDir.string(), {});
    builds::Result r;
    CHECK(b.place(stage.string(), ev.string(), r) == builds::kExitOk);
    removeAllSafe(stage);
    std::error_code ec;
    fs::remove(ev, ec);
    return hash;
}

bool sendAll(int fd, const std::string& s) {
    std::size_t off = 0;
    int flags = 0;
#ifdef MSG_NOSIGNAL
    flags |= MSG_NOSIGNAL;
#endif
    while (off < s.size()) {
        const ssize_t n = ::send(fd, s.data() + off, s.size() - off, flags);
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

bool readLine(int fd, std::string* out) {
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
    const bool ok = (::connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof addr) == 0) &&
                    sendAll(fd, req + "\n") && readLine(fd, first);
    if (ok && second != nullptr) {
        (void)readLine(fd, second);
    }
    ::close(fd);
    return ok;
}

bool waitServingHash(const std::string& sockPath, const std::string& wantHash, std::uint64_t timeoutMs = 15000) {
    const std::uint64_t t0 = nowMs();
    while (nowMs() - t0 < timeoutMs) {
        std::string l1, l2;
        if (askSocket(sockPath, "status", &l1, &l2) && l1 == "SERVING") {
            ServerSeen seen;
            if (parseStatusLines(l1, l2, !l2.empty(), &seen) && seen.serving && !seen.draining &&
                seen.build == wantHash) {
                return true;
            }
        }
        sleepMs(25);
    }
    return false;
}

pid_t spawnProc(const std::string& exe, const std::vector<std::string>& args, const std::string& home) {
    std::vector<std::string> envBuf;
    for (char** ep = environ; ep != nullptr && *ep != nullptr; ++ep) {
        const std::string entry(*ep);
        if (entry.rfind("HOME=", 0) == 0 || entry.rfind("NEXUS_SUPERVISE_", 0) == 0) {
            continue;
        }
        envBuf.push_back(entry);
    }
    envBuf.push_back("HOME=" + home);
    envBuf.push_back("NEXUS_SUPERVISE_BACKOFF_MS=20");
    envBuf.push_back("NEXUS_SUPERVISE_STABLE_MS=60000");

    std::vector<char*> envp;
    for (std::string& e : envBuf) {
        envp.push_back(e.data());
    }
    envp.push_back(nullptr);

    std::vector<std::string> argvBuf;
    argvBuf.push_back(exe);
    for (const std::string& a : args) {
        argvBuf.push_back(a);
    }
    std::vector<char*> argv;
    for (std::string& a : argvBuf) {
        argv.push_back(a.data());
    }
    argv.push_back(nullptr);

    pid_t pid = -1;
    CHECK(::posix_spawn(&pid, exe.c_str(), nullptr, nullptr, argv.data(), envp.data()) == 0);
    return pid;
}

int waitProc(pid_t pid, std::uint64_t timeoutMs = 15000) {
    const std::uint64_t t0 = nowMs();
    while (nowMs() - t0 < timeoutMs) {
        int status = 0;
        const pid_t w = ::waitpid(pid, &status, WNOHANG);
        if (w == pid) {
            if (WIFEXITED(status)) {
                return WEXITSTATUS(status);
            }
            return 128 + WTERMSIG(status);
        }
        sleepMs(20);
    }
    ::kill(pid, SIGKILL);
    int status = 0;
    (void)::waitpid(pid, &status, 0);
    throw std::runtime_error("process timed out");
}

void killIfRunning(pid_t pid) {
    if (pid <= 0) {
        return;
    }
    int status = 0;
    if (::waitpid(pid, &status, WNOHANG) == 0) {
        ::kill(pid, SIGKILL);
        (void)::waitpid(pid, &status, 0);
    }
}

void testE2eMigrationHandoverDrain(const std::string& fakeBin, const fs::path& base) {
    const fs::path home = base / "home";
    const fs::path top = home / ".nexus";
    const fs::path sdir = top / "server";
    fs::create_directories(top);
    fs::permissions(home, fs::perms(0700));
    fs::permissions(top, fs::perms(0700));

    // 1. Seed old ~/.nexus/store and ~/.nexus/drivers.
    std::unique_ptr<store::Fs> pfs = store::make_posix_fs();
    {
        std::unique_ptr<store::Store> oldSt;
        CHECK(store::Store::open(*pfs, (top / "store").string(), {}, oldSt).is_ok());
        store::Change mk;
        mk.kind = store::Change::Kind::SetInfo;
        mk.info.friend_name = "alice";
        mk.info.harness = "claude";
        store::Change app;
        app.kind = store::Change::Kind::Append;
        app.entry = store::Entry{1, "old-layout-preserved", true};
        const store::Change batch[] = {mk, app};
        CHECK(oldSt->commit(oldSt->next_session(), batch).is_ok());
    }
    const std::string drv = "{\"drivers\":[]}\n";
    CHECK(pfs->write_new((top / "drivers").string(), store::Bytes(drv.begin(), drv.end())).is_ok());

    fs::create_directories(sdir);
    fs::permissions(sdir, fs::perms(0700));
    const std::string b1 = placeFakeBuild(base, sdir, fakeBin, "good", 3, 3, "b1");
    const std::string b2 = placeFakeBuild(base, sdir, fakeBin, "good", 5, 3, "b2");
    {
        builds::Builds b(sdir.string(), {});
        builds::Result r;
        CHECK(b.makeCurrent(b1, r) == builds::kExitOk);
    }

    // 2. Start a live simulated pre-StoreLock server on ~/.nexus/server.sock!
    const std::string oldSock = (top / "server.sock").string();
    ::unlink(oldSock.c_str());
    const int oldListen = ::socket(AF_UNIX, SOCK_STREAM, 0);
    CHECK(oldListen >= 0);
    sockaddr_un oldAddr{};
    oldAddr.sun_family = AF_UNIX;
    std::memcpy(oldAddr.sun_path, oldSock.data(), oldSock.size());
    CHECK(::bind(oldListen, reinterpret_cast<const sockaddr*>(&oldAddr), sizeof oldAddr) == 0);
    CHECK(::listen(oldListen, 4) == 0);
    bool stopOldThread = false;
    std::thread oldSrvThread([&]() {
        while (!stopOldThread) {
            pollfd pfd{oldListen, POLLIN, 0};
            if (::poll(&pfd, 1, 25) > 0 && (pfd.revents & POLLIN)) {
                const int cfd = ::accept(oldListen, nullptr, nullptr);
                if (cfd >= 0) {
                    std::string line;
                    if (readLine(cfd, &line)) {
                        sendAll(cfd, "SERVING\n");
                    }
                    ::close(cfd);
                }
            }
        }
    });

    // `supervise` MUST refuse cleanly (exit 1) without migrating ~/.nexus or halting supervise/state!
    const pid_t refusedSup = spawnProc((sdir / "current/bin/nexus-server").string(),
                                       {"supervise", "--dir", sdir.string()}, home.string());
    const int refusedCode = waitProc(refusedSup);
    CHECK(refusedCode == supervision::kExitFailed);
    CHECK(fs::exists(top / "store"));
    CHECK(!fs::exists(sdir / "supervise/state"));

    // 3. Stop the old server -> start `supervise` -> migrates once and serves b1 with migrated entry intact!
    stopOldThread = true;
    oldSrvThread.join();
    ::close(oldListen);
    ::unlink(oldSock.c_str());

    pid_t supPid = spawnProc((sdir / "current/bin/nexus-server").string(),
                             {"supervise", "--dir", sdir.string()}, home.string());
    struct Guard {
        pid_t& p;
        ~Guard() { killIfRunning(p); }
    } supGuard{supPid};

    const std::string sock = (sdir / "server.sock").string();
    CHECK(waitServingHash(sock, b1));
    CHECK(!fs::exists(top / "store"));
    CHECK(!fs::exists(top / "drivers"));
    CHECK(fs::exists(sdir / "drivers"));

    std::string reply;
    CHECK(askSocket(sock, "entries", &reply) && reply == "1");

    // 4. Open turn on b1, plant a stray `~/.nexus/server.log` to prove `handover` skips `migrateOldLayout` when a
    //    new-layout server is already serving on `sdir/server.sock` (no false `sdir/store` Busy failure!), start
    //    `handover <b2>` concurrently, and complete turn -> drained with 0 lost entries!
    {
        const std::string strayLog = "stray-top-log\n";
        CHECK(pfs->write_new((top / "server.log").string(), store::Bytes(strayLog.begin(), strayLog.end())).is_ok());
    }
    CHECK(askSocket(sock, "turn open", &reply) && reply == "OK");
    const pid_t hoPid = spawnProc((sdir / "current/bin/nexus-server").string(),
                                  {"handover", b2, "--dir", sdir.string()}, home.string());
    sleepMs(80);
    CHECK(askSocket(sock, "turn end", &reply) && reply == "OK");
    CHECK(waitProc(hoPid) == handover::kExitOk);
    CHECK(waitServingHash(sock, b2));
    CHECK(askSocket(sock, "entries", &reply) && reply == "2");

    // 5. Write rollback-compatible format-5 record on b2 (5/3). Write-downgrading `handover <b1>` while b2 holds
    //    `<dir>/store` is refused cleanly; after `stop` releases `<dir>/store`, `handover <b1>` inspects disk formats,
    //    switches to b1 (3/3), clears `stopped`, and b1 reads all 3 entries!
    CHECK(askSocket(sock, "write-format5-compat", &reply) && reply == "OK");
    const pid_t hoBusyPid = spawnProc((sdir / "current/bin/nexus-server").string(),
                                      {"handover", b1, "--dir", sdir.string()}, home.string());
    CHECK(waitProc(hoBusyPid) == handover::kExitRefused);

    CHECK(askSocket(sock, "stop", &reply) && reply == "OK");
    CHECK(waitProc(supPid) == supervision::kExitOk);
    supPid = -1;

    // 6. Without `--start` or `handover`, restarting `supervise` honors the intentional stop (`exit 0`).
    const pid_t stayStopped = spawnProc((sdir / "current/bin/nexus-server").string(),
                                        {"supervise", "--dir", sdir.string()}, home.string());
    CHECK(waitProc(stayStopped) == supervision::kExitOk);

    // 7. Explicit `handover <b1>` inspects disk formats (`maxReader == 3 <= 3`), clears `stopped`, and starts `b1`!
    const pid_t hoBackPid = spawnProc((sdir / "current/bin/nexus-server").string(),
                                      {"handover", b1, "--dir", sdir.string()}, home.string());
    CHECK(waitProc(hoBackPid) == handover::kExitOk);
    CHECK(waitServingHash(sock, b1));
    CHECK(askSocket(sock, "entries", &reply) && reply == "3");
    CHECK(askSocket(sock, "stop", &reply) && reply == "OK");
}

void testE2ePostStartCrashFormat5CompatRollback(const std::string& fakeBin, const fs::path& base) {
    const fs::path home = base / "home";
    const fs::path sdir = home / ".nexus/server";
    fs::create_directories(sdir);
    fs::permissions(home, fs::perms(0700));
    fs::permissions(home / ".nexus", fs::perms(0700));
    fs::permissions(sdir, fs::perms(0700));

    const std::string b1 = placeFakeBuild(base, sdir, fakeBin, "good", 3, 3, "b1");
    const std::string b3Crash = placeFakeBuild(base, sdir, fakeBin, "crash-after-serve", 5, 5, "b3-crash");
    {
        builds::Builds b(sdir.string(), {});
        builds::Result r;
        CHECK(b.makeCurrent(b1, r) == builds::kExitOk);
    }

    pid_t supPid = spawnProc((sdir / "current/bin/nexus-server").string(),
                             {"supervise", "--dir", sdir.string()}, home.string());
    struct Guard {
        pid_t& p;
        ~Guard() { killIfRunning(p); }
    } supGuard{supPid};

    const std::string sock = (sdir / "server.sock").string();
    CHECK(waitServingHash(sock, b1));
    std::string reply;
    CHECK(askSocket(sock, "turn open", &reply) && reply == "OK");
    CHECK(askSocket(sock, "turn end", &reply) && reply == "OK");

    // Handover to 5/5 `crash-after-serve` build: reaches SERVING, then crashes twice before writing format 5;
    // SupervisorLoop rolls back to 3/3 `b1` via `Builds::rollbackTo`!
    const pid_t hoPid = spawnProc((sdir / "current/bin/nexus-server").string(),
                                  {"handover", b3Crash, "--dir", sdir.string()}, home.string());
    const int hoRc = waitProc(hoPid);
    CHECK(hoRc == handover::kExitOk || hoRc == handover::kExitRolledBack);

    CHECK(waitServingHash(sock, b1, 15000));
    CHECK(askSocket(sock, "entries", &reply) && reply == "1");

    CHECK(askSocket(sock, "stop", &reply) && reply == "OK");
    CHECK(waitProc(supPid) == supervision::kExitOk);
    supPid = -1;
}

void testE2eBreakingFormat5RefusesRollbackAndHalts(const std::string& fakeBin, const fs::path& base) {
    const fs::path home = base / "home";
    const fs::path sdir = home / ".nexus/server";
    fs::create_directories(sdir);
    fs::permissions(home, fs::perms(0700));
    fs::permissions(home / ".nexus", fs::perms(0700));
    fs::permissions(sdir, fs::perms(0700));

    const std::string b1 = placeFakeBuild(base, sdir, fakeBin, "good", 3, 3, "b1");
    const std::string b3 = placeFakeBuild(base, sdir, fakeBin, "good", 5, 5, "b3");
    {
        builds::Builds b(sdir.string(), {});
        builds::Result r;
        CHECK(b.makeCurrent(b1, r) == builds::kExitOk);
    }

    pid_t supPid = spawnProc((sdir / "current/bin/nexus-server").string(),
                             {"supervise", "--dir", sdir.string()}, home.string());
    struct Guard {
        pid_t& p;
        ~Guard() { killIfRunning(p); }
    } supGuard{supPid};

    const std::string sock = (sdir / "server.sock").string();
    CHECK(waitServingHash(sock, b1));

    const pid_t hoPid = spawnProc((sdir / "current/bin/nexus-server").string(),
                                  {"handover", b3, "--dir", sdir.string()}, home.string());
    CHECK(waitProc(hoPid) == handover::kExitOk);
    CHECK(waitServingHash(sock, b3));

    // Write breaking format-5 record (`reader = 5`) on b3!
    std::string reply;
    CHECK(askSocket(sock, "write-format5-break", &reply) && reply == "OK");

    // Crash b3 repeatedly until `supervise` exhausts restarts and halts cleanly (`kExitHalted == 78`)!
    const std::uint64_t t0 = nowMs();
    int supRc = -1;
    while (nowMs() - t0 < 15000) {
        int st = 0;
        if (::waitpid(supPid, &st, WNOHANG) == supPid) {
            supRc = WIFEXITED(st) ? WEXITSTATUS(st) : (128 + WTERMSIG(st));
            supPid = -1;
            break;
        }
        (void)askSocket(sock, "crash", &reply);
        sleepMs(35);
    }
    CHECK(supRc == supervision::kExitHalted);

    // Verify `builds/active` remained on `b3` (never switched to unreadable `b1`) and `supervise/state` shunned `b1`!
    std::string cur, prev;
    builds::Builds b(sdir.string(), {});
    builds::Result br;
    CHECK(b.active(&cur, &prev, br) == builds::kExitOk);
    CHECK(cur == b3);

    std::unique_ptr<store::Fs> pfs = store::make_posix_fs();
    supervision::SuperviseState sst;
    CHECK(supervision::loadEffectiveState(*pfs, (sdir / "supervise").string(), &sst).is_ok());
    CHECK(sst.halted);
    CHECK(sst.shunned.count(b1) == 1);
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 4) {
        std::fprintf(stderr, "usage: takeover_functional <fake_build_server> <case> <scratch_dir>\n");
        return 2;
    }
    const std::string fakeBin = argv[1];
    const std::string tc = argv[2];
    const fs::path base = fs::path(argv[3]);
    removeAllSafe(base);

    struct Cleanup {
        fs::path root;
        ~Cleanup() { removeAllSafe(root); }
    } cleanup{base};

    try {
        if (tc == "e2e_migration_handover_drain") {
            testE2eMigrationHandoverDrain(fakeBin, base);
        } else if (tc == "e2e_post_start_crash_format5_compat_rollback") {
            testE2ePostStartCrashFormat5CompatRollback(fakeBin, base);
        } else if (tc == "e2e_breaking_format5_refuses_rollback_and_halts") {
            testE2eBreakingFormat5RefusesRollbackAndHalts(fakeBin, base);
        } else {
            std::fprintf(stderr, "unknown case: %s\n", tc.c_str());
            return 2;
        }
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "takeover_functional (%s) failed: %s\n", tc.c_str(), ex.what());
        return 1;
    }
    std::printf("takeover_functional (%s): PASS\n", tc.c_str());
    return 0;
}
