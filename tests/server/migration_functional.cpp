// Functional tests of Layer 5 old-layout migration and store format against rollback (src/server/migration.h,
// src/server/builds.h, src/store/store.h, src/server/durable.h): real POSIX files, AF_UNIX sockets, OS locks, git
// config repositories and child processes killed mid-step in isolated scratch homes (never touching real ~/.nexus).
//
// Usage: migration_functional <case> <dir>

#include "server/builds.h"
#include "server/config_repo.h"
#include "server/driver_table.h"
#include "server/durable.h"
#include "server/home.h"
#include "server/migration.h"
#include "store/codec.h"
#include "store/fs.h"
#include "store/journal.h"
#include "store/store.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <poll.h>
#include <signal.h>
#include <sstream>
#include <string>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;
using namespace nexus;
using namespace nexus::server;

namespace {

int failures = 0;

#define CHECK(cond)                                                                       \
    do {                                                                                  \
        if (!(cond)) {                                                                    \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                                   \
        }                                                                                 \
    } while (0)

bool has(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

fs::path root;

void removeAll(const fs::path& p) {
    std::error_code ec;
    const fs::file_status top = fs::symlink_status(p, ec);
    if (ec || !fs::exists(top)) {
        return;
    }
    if (fs::is_directory(top)) {
        fs::permissions(p, fs::perms::owner_all, fs::perm_options::add, ec);
    }
    ec.clear();
    for (auto it = fs::recursive_directory_iterator(p, ec); !ec && it != fs::recursive_directory_iterator();
         it.increment(ec)) {
        std::error_code iec;
        const fs::file_status st = it->symlink_status(iec);
        if (!iec && fs::is_directory(st)) {
            fs::permissions(it->path(), fs::perms::owner_all, fs::perm_options::add, iec);
        } else if (!iec && fs::is_regular_file(st)) {
            fs::permissions(it->path(), fs::perms::owner_write, fs::perm_options::add, iec);
        }
    }
    ec.clear();
    fs::remove_all(p, ec);
    if (ec) {
        std::fprintf(stderr, "cannot remove %s: %s\n", p.c_str(), ec.message().c_str());
        ++failures;
    }
}

void writeFile(const fs::path& p, const std::string& text, bool exec = false, int mode = -1) {
    fs::create_directories(p.parent_path());
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out << text;
    out.close();
    if (!out) {
        std::fprintf(stderr, "cannot write %s\n", p.c_str());
        removeAll(root);
        std::exit(1);
    }
    if (mode >= 0) {
        ::chmod(p.c_str(), static_cast<mode_t>(mode));
    } else {
        fs::permissions(p, exec ? fs::perms(0755) : fs::perms(0644));
    }
}

std::string readAll(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::vector<std::string> sortedEntries(const fs::path& dir) {
    std::vector<std::string> out;
    std::error_code ec;
    for (const fs::directory_entry& e : fs::directory_iterator(dir, ec)) {
        out.push_back(e.path().filename().string());
    }
    std::sort(out.begin(), out.end());
    return out;
}

int modeOf(const fs::path& p) {
    struct stat st{};
    return ::lstat(p.c_str(), &st) == 0 ? int(st.st_mode & 0777) : -1;
}

// Populates an old-layout ~/.nexus directory with a compacted + logged store, drivers, config, server.log and client
// config.
void seedOldLayout(const fs::path& homeDir) {
    const fs::path top = homeDir / ".nexus";
    fs::create_directories(top);
    ::chmod(top.c_str(), 0700);

    auto pfs = store::make_posix_fs();
    store::StoreOptions small;
    small.compact_bytes = 64;
    {
        DurableStore ds;
        std::string why;
        CHECK(ds.open(*pfs, (top / "store").string(), &why, small));
        const sync::SessionChange mk = sync::infoChange(sync::SessionInfo{"ada", "claude", "main", "opus", "high", ""});
        const sync::SessionChange u1{
            std::make_shared<const sync::SessionEntry>(sync::SessionEntry{EntryKind::Person, "hello from old", true}), ""};
        CHECK(ds.commit(1, {mk, u1}, "win-1", 1, &why));

        const sync::SessionChange a1{
            std::make_shared<const sync::SessionEntry>(sync::SessionEntry{EntryKind::AI, "old reply", true}), ""};
        const sync::SessionChange q1{nullptr, "queued follow-up"};
        CHECK(ds.commit(1, {a1, q1}, "win-1", 2, &why));
    }
    // Pre-StoreLock layouts had no `gate` file in ~/.nexus/store; test migrating cleanly without one.
    ::unlink((top / "store" / store::kGateName).c_str());

    writeFile(top / "drivers",
              "driver claude /usr/bin/node /opt/drivers/claude/main.js --account main\n"
              "ai ada claude ~/work\n",
              false, 0600);
    writeFile(top / "config", "# migrated server config\n", false, 0600);
    writeFile(top / "server.log", "old server log line\n", false, 0600);
    writeFile(top / "client" / "config", "server studio\n", false, 0600);
}

// Case 1: old ~/.nexus moves once to ~/.nexus/server, client stays untouched, DurableStore and ConfigRepo open and
// adopt the migrated state, and a second run is a fast-path no-op.
void caseOldToNew() {
    const fs::path home = root / "home";
    seedOldLayout(home);

    std::string resolvedHome;
    std::string top;
    std::string serverDir;
    std::string why;
    CHECK(serverHome(home.string().c_str(), &resolvedHome, &top, &serverDir, &why));
    CHECK(top == (home / ".nexus").string());
    CHECK(serverDir == (home / ".nexus" / "server").string());

    // `new` opens server.log in ~/.nexus/server via posix_spawn before `serve` starts; verify pre-existing server.log
    // in ~/.nexus/server does not block migration.
    writeFile(fs::path(serverDir) / "server.log", "spawned server log\n", false, 0600);
    ::chmod(serverDir.c_str(), 0700);

    auto pfs = store::make_posix_fs();
    migration::Options opts;
    opts.old_server_live = [&] { return migration::probeOldServerSocket(top + "/server.sock"); };
    migration::Report rep;
    CHECK(migration::migrateOldLayout(*pfs, top, serverDir, opts, &rep).is_ok());
    CHECK(rep.migrated && !rep.resumed && !rep.conflict);
    CHECK(modeOf(top) == 0700 && modeOf(serverDir) == 0700);

    // Only `client` and `server` remain at the top of ~/.nexus.
    CHECK((sortedEntries(top) == std::vector<std::string>{"client", "server"}));
    CHECK(readAll(fs::path(top) / "client" / "config") == "server studio\n");
    CHECK(readAll(fs::path(serverDir) / "config") == "# migrated server config\n");

    // DurableStore reopens every session, transcript entry, queued prompt and client counter.
    {
        DurableStore ds;
        CHECK(ds.open(*pfs, serverDir + "/store", &why));
        CHECK(ds.size() == 1);
        CHECK(ds.applied("win-1") == 2);
        const sync::StoredSession* ses = ds.find(1);
        CHECK(ses != nullptr);
        if (ses != nullptr) {
            CHECK(ses->info.aiName == "ada" && ses->info.account == "main");
            CHECK(ses->transcript.size() == 2);
            CHECK(ses->transcript[0]->text == "hello from old");
            CHECK(ses->transcript[1]->text == "old reply");
            CHECK((ses->queue == std::vector<std::string>{"queued follow-up"}));
        }
    }

    // ConfigRepo seeds accounts.json and ais.json from the migrated `drivers` file, just as Server::restore() does.
    {
        ConfigRepo repo(serverDir + "/nexus-config");
        CHECK(repo.open(&why));
        CHECK(!repo.hasAccounts());
        const DriverTable dt = parseDriverTable(readAll(fs::path(serverDir) / "drivers"), home.string());
        CHECK(dt.errors.empty());
        const accounts::Registry seed = accountsFromDrivers(dt, "space");
        CHECK(repo.keepSeed(seed, "seed from migrated drivers", &why));
        CHECK(repo.hasAccounts());
        accounts::Registry committed;
        std::vector<std::string> report;
        CHECK(repo.committed(&committed, &report, false));
        CHECK(committed.accounts.size() == 1 && committed.accounts[0].name == "main");
        CHECK(committed.ais.size() == 1 && committed.ais[0].name == "ada");
    }

    // Second start is an immediate fast-path no-op.
    migration::Report rep2;
    CHECK(migration::migrateOldLayout(*pfs, top, serverDir, opts, &rep2).is_ok());
    CHECK(!rep2.migrated && !rep2.resumed);
}

// Case 2: refuses while a real pre-StoreLock server answers on ~/.nexus/server.sock or a process holds flock(LOCK_EX)
// on ~/.nexus/store, leaving ~/.nexus untouched; succeeds once the old server stops (cleaning stale server.sock); and
// serializes two concurrent migrator processes cleanly.
void caseLiveOldRefused() {
    fs::path sockRoot = root;
    bool tempShortRoot = false;
    sockaddr_un probeAddr{};
    if ((root / "h1" / ".nexus" / "server.sock").string().size() >= sizeof(probeAddr.sun_path)) {
        const char* base = std::getenv("TMPDIR");
        std::string pat = std::string(base != nullptr && *base != '\0' ? base : "/tmp") + "/nxmig-XXXXXX";
        char* made = ::mkdtemp(pat.data());
        CHECK(made != nullptr);
        sockRoot = made;
        tempShortRoot = true;
    }
    struct ShortGuard {
        fs::path p;
        bool active;
        ~ShortGuard() {
            if (active) {
                removeAll(p);
            }
        }
    } shortGuard{sockRoot, tempShortRoot};

    // 2a. Pre-StoreLock server listening on ~/.nexus/server.sock (answers `ERROR unknown request\n`).
    {
        const fs::path home = sockRoot / "h1";
        seedOldLayout(home);
        const fs::path top = home / ".nexus";
        const fs::path serverDir = top / "server";
        const std::string sockPath = (top / "server.sock").string();

        const int listenFd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        CHECK(listenFd >= 0);
        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        std::memcpy(addr.sun_path, sockPath.c_str(), sockPath.size() + 1);
        ::unlink(sockPath.c_str());
        CHECK(::bind(listenFd, reinterpret_cast<const sockaddr*>(&addr), sizeof addr) == 0);
        CHECK(::listen(listenFd, 4) == 0);

        const pid_t oldPid = ::fork();
        CHECK(oldPid >= 0);
        if (oldPid == 0) {
            for (;;) {
                const int cfd = ::accept(listenFd, nullptr, nullptr);
                if (cfd < 0) {
                    if (errno == EINTR) {
                        continue;
                    }
                    _exit(0);
                }
                char buf[64];
                (void)::read(cfd, buf, sizeof buf);
                const char reply[] = "ERROR unknown request\n";
                (void)::write(cfd, reply, sizeof(reply) - 1);
                ::close(cfd);
            }
        }
        ::close(listenFd);

        auto pfs = store::make_posix_fs();
        migration::Options opts;
        opts.old_server_live = [&] { return migration::probeOldServerSocket(sockPath); };
        const store::Status st = migration::migrateOldLayout(*pfs, top.string(), serverDir.string(), opts);
        CHECK(st.code() == store::Status::Code::Failed);
        CHECK(has(st.message(), "server.sock"));
        CHECK(fs::exists(top / "store" / "log") && !fs::exists(serverDir / "store"));

        // Kill the old server (leaving stale ~/.nexus/server.sock on disk!): next migration cleans stale socket and
        // succeeds.
        ::kill(oldPid, SIGKILL);
        int wstatus = 0;
        (void)::waitpid(oldPid, &wstatus, 0);
        CHECK(fs::exists(top / "server.sock"));

        migration::Report rep;
        CHECK(migration::migrateOldLayout(*pfs, top.string(), serverDir.string(), opts, &rep).is_ok());
        CHECK(rep.migrated);
        CHECK(!fs::exists(top / "server.sock"));
        CHECK((sortedEntries(top) == std::vector<std::string>{"client", "server"}));
    }

    // 2b. Old process holding flock(LOCK_EX) on ~/.nexus/store without a socket.
    {
        const fs::path home = sockRoot / "h2";
        seedOldLayout(home);
        const fs::path top = home / ".nexus";
        const fs::path serverDir = top / "server";

        int readyPipe[2]{-1, -1};
        int stopPipe[2]{-1, -1};
        CHECK(::pipe(readyPipe) == 0 && ::pipe(stopPipe) == 0);
        const pid_t holder = ::fork();
        CHECK(holder >= 0);
        if (holder == 0) {
            ::close(readyPipe[0]);
            ::close(stopPipe[1]);
            auto cfs = store::make_posix_fs();
            std::unique_ptr<store::FileLock> lk;
            if (!cfs->lock((top / "store").string(), store::LockMode::Exclusive, lk).is_ok()) {
                _exit(2);
            }
            const char c = '1';
            (void)::write(readyPipe[1], &c, 1);
            char waitByte = 0;
            (void)::read(stopPipe[0], &waitByte, 1);
            _exit(0);
        }
        ::close(readyPipe[1]);
        ::close(stopPipe[0]);
        char readyByte = 0;
        CHECK(::read(readyPipe[0], &readyByte, 1) == 1);
        ::close(readyPipe[0]);

        auto pfs = store::make_posix_fs(store::PosixOptions{
            [] { return std::uint64_t(1000); }, [](std::uint64_t) {}, 0, {}, false, false});
        const store::Status st = migration::migrateOldLayout(*pfs, top.string(), serverDir.string());
        CHECK(st.code() == store::Status::Code::Busy);
        CHECK(has(st.message(), "/store"));

        ::close(stopPipe[1]);
        int wstatus = 0;
        (void)::waitpid(holder, &wstatus, 0);

        auto realFs = store::make_posix_fs();
        CHECK(migration::migrateOldLayout(*realFs, top.string(), serverDir.string()).is_ok());
        CHECK((sortedEntries(top) == std::vector<std::string>{"client", "server"}));
    }
}

// Case 3: a child process killed (_exit) at every migration phase resumes cleanly on the next start; and pre-existing
// conflicting ~/.nexus/server/store or differing drivers/config refuses without overwriting either side.
void caseCrashResumeAndConflict() {
    // First count how many hook events a full migration emits.
    std::size_t totalHooks = 0;
    {
        const fs::path home = root / "count";
        seedOldLayout(home);
        const fs::path top = home / ".nexus";
        const fs::path serverDir = top / "server";
        writeFile(top / "server.sock", "", false, 0600);
        auto pfs = store::make_posix_fs();
        migration::Options opts;
        opts.hook = [&](const std::string& ph, const std::string&) {
            if (ph != "idle") {
                ++totalHooks;
            }
        };
        CHECK(migration::migrateOldLayout(*pfs, top.string(), serverDir.string(), opts).is_ok());
    }
    CHECK(totalHooks >= 12);

    // Cut a child process with `_exit(99)` at every hook index `0 .. totalHooks - 1` and verify resume recovers 100%.
    for (std::size_t cut = 0; cut < totalHooks; ++cut) {
        const fs::path home = root / ("cut-" + std::to_string(cut));
        seedOldLayout(home);
        const fs::path top = home / ".nexus";
        const fs::path serverDir = top / "server";
        writeFile(top / "server.sock", "", false, 0600);

        const pid_t child = ::fork();
        CHECK(child >= 0);
        if (child == 0) {
            auto cfs = store::make_posix_fs();
            migration::Options opts;
            std::size_t seen = 0;
            opts.hook = [&](const std::string& ph, const std::string&) {
                if (ph == "idle") {
                    return;
                }
                if (seen == cut) {
                    _exit(99);
                }
                ++seen;
            };
            (void)migration::migrateOldLayout(*cfs, top.string(), serverDir.string(), opts);
            _exit(0);
        }
        int wstatus = 0;
        CHECK(::waitpid(child, &wstatus, 0) == child);
        CHECK(WIFEXITED(wstatus) && WEXITSTATUS(wstatus) == 99);

        auto pfs = store::make_posix_fs();
        CHECK(migration::migrateOldLayout(*pfs, top.string(), serverDir.string()).is_ok());
        CHECK((sortedEntries(top) == std::vector<std::string>{"client", "server"}));
        CHECK(!fs::exists(serverDir / migration::kPlanFile));

        DurableStore ds;
        std::string why;
        CHECK(ds.open(*pfs, (serverDir / "store").string(), &why));
        CHECK(ds.size() == 1 && ds.applied("win-1") == 2);
        const sync::StoredSession* ses = ds.find(1);
        CHECK(ses != nullptr && ses->transcript.size() == 2);
        removeAll(home);
    }

    // Conflict refusal: both ~/.nexus/store and ~/.nexus/server/store hold stores -> neither is touched.
    {
        const fs::path home = root / "conflict";
        seedOldLayout(home);
        const fs::path top = home / ".nexus";
        const fs::path serverDir = top / "server";
        auto pfs = store::make_posix_fs();
        {
            DurableStore newDs;
            std::string why;
            CHECK(newDs.open(*pfs, (serverDir / "store").string(), &why));
            const sync::SessionChange mk =
                sync::infoChange(sync::SessionInfo{"bob", "claude", "main", "opus", "high", ""});
            CHECK(newDs.commit(1, {mk}, "win-new", 1, &why));
        }
        const std::string oldLogBefore = readAll(top / "store" / "log");
        const std::string newLogBefore = readAll(serverDir / "store" / "log");

        migration::Report rep;
        const store::Status st = migration::migrateOldLayout(*pfs, top.string(), serverDir.string(), {}, &rep);
        CHECK(st.code() == store::Status::Code::Failed);
        CHECK(rep.conflict && !rep.migrated);
        CHECK(has(st.message(), "neither was overwritten"));
        CHECK(readAll(top / "store" / "log") == oldLogBefore);
        CHECK(readAll(serverDir / "store" / "log") == newLogBefore);
    }

    // Symlink, FIFO, directory and open-mode hardening:
    //   - symlinked ~/.nexus/store -> external directory is refused before touching external snapshot/log or writing .migrating;
    //   - symlinked ~/.nexus/config -> outside secret file is refused without copying target bytes;
    //   - FIFO ~/.nexus/server.log and directory ~/.nexus/drivers are refused immediately without blocking or mutating server/;
    //   - pre-existing 0755 ~/.nexus/server/store on resume is refused cleanly.
    {
        auto pfs = store::make_posix_fs();

        // 3b. Symlinked ~/.nexus/store -> external store directory.
        const fs::path ext = root / "ext-store";
        const fs::path symStoreHome = root / "sym-store";
        const fs::path symStoreTop = symStoreHome / ".nexus";
        const fs::path symStoreDir = symStoreTop / "server";
        fs::create_directories(ext);
        ::chmod(ext.c_str(), 0700);
        writeFile(ext / "snapshot", "ext-snap-bytes", false, 0600);
        writeFile(ext / "log", "ext-log-bytes", false, 0600);
        fs::create_directories(symStoreTop);
        ::chmod(symStoreTop.c_str(), 0700);
        CHECK(::symlink(ext.c_str(), (symStoreTop / "store").c_str()) == 0);
        const store::Status stStoreLink = migration::migrateOldLayout(*pfs, symStoreTop.string(), symStoreDir.string());
        CHECK(stStoreLink.code() == store::Status::Code::Failed);
        CHECK(has(stStoreLink.message(), "symbolic link"));
        CHECK(fs::exists(ext / "snapshot") && readAll(ext / "snapshot") == "ext-snap-bytes");
        CHECK(fs::exists(ext / "log") && readAll(ext / "log") == "ext-log-bytes");
        CHECK(!fs::exists(symStoreDir / migration::kPlanFile));

        // 3c. Symlinked ~/.nexus/config -> outside file.
        const fs::path secret = root / "outside-secret";
        const fs::path symCfgTop = root / "sym-cfg" / ".nexus";
        const fs::path symCfgDir = symCfgTop / "server";
        writeFile(secret, "do-not-copy-secret\n", false, 0600);
        fs::create_directories(symCfgTop);
        ::chmod(symCfgTop.c_str(), 0700);
        CHECK(::symlink(secret.c_str(), (symCfgTop / "config").c_str()) == 0);
        const store::Status stCfgLink = migration::migrateOldLayout(*pfs, symCfgTop.string(), symCfgDir.string());
        CHECK(stCfgLink.code() == store::Status::Code::Failed);
        CHECK(has(stCfgLink.message(), "symbolic link"));
        CHECK(!fs::exists(symCfgDir / "config"));
        CHECK(readAll(secret) == "do-not-copy-secret\n");

        // 3d. FIFO at ~/.nexus/server.log and directory at ~/.nexus/drivers: neither blocks nor writes server/drivers.
        const fs::path nonRegTop = root / "nonreg" / ".nexus";
        const fs::path nonRegDir = nonRegTop / "server";
        fs::create_directories(nonRegTop / "drivers");
        ::chmod(nonRegTop.c_str(), 0700);
        ::chmod((nonRegTop / "drivers").c_str(), 0700);
        const store::Status stDir = migration::migrateOldLayout(*pfs, nonRegTop.string(), nonRegDir.string());
        CHECK(stDir.code() == store::Status::Code::Failed);
        CHECK(has(stDir.message(), "not a regular file"));
        CHECK(!fs::exists(nonRegDir / "drivers"));

        fs::remove_all(nonRegTop / "drivers");
        CHECK(::mkfifo((nonRegTop / "server.log").c_str(), 0600) == 0);
        const store::Status stFifo = migration::migrateOldLayout(*pfs, nonRegTop.string(), nonRegDir.string());
        CHECK(stFifo.code() == store::Status::Code::Failed);
        CHECK(has(stFifo.message(), "not a regular file"));
        CHECK(!fs::exists(nonRegDir / "server.log"));

        // 3e. Resume when ~/.nexus/server/store is mode 0755: refused cleanly before unlocking as migrated.
        const fs::path modeTop = root / "mode-resume" / ".nexus";
        const fs::path modeDir = modeTop / "server";
        fs::create_directories(modeTop);
        fs::create_directories(modeDir / "store");
        ::chmod(modeTop.c_str(), 0700);
        ::chmod(modeDir.c_str(), 0700);
        writeFile(modeDir / migration::kPlanFile, "log\n", false, 0600);
        writeFile(modeDir / "store" / "log", "", false, 0600);
        ::chmod((modeDir / "store").c_str(), 0755);
        const store::Status stMode = migration::migrateOldLayout(*pfs, modeTop.string(), modeDir.string());
        CHECK(stMode.code() == store::Status::Code::Failed);
        CHECK(has(stMode.message(), "open to other users"));
    }
}

fs::path makeBuildTree(const std::string& name, const std::string& formatsText) {
    const fs::path t = root / "trees" / name;
    removeAll(t);
    writeFile(t / "bin" / "nexus-server", "#!/bin/sh\necho " + name + "\n", true);
    writeFile(t / "drivers" / "claude" / "main.js", "// " + name + "\n");
    writeFile(t / "formats", formatsText);
    return t;
}

std::string placeTestedBuild(builds::Builds& b, const std::string& name, const std::string& formatsText) {
    const fs::path tree = makeBuildTree(name, formatsText);
    std::string h;
    CHECK(builds::hashTree(tree.string(), &h).is_ok());
    const fs::path ev = root / "trees" / (name + ".evidence");
    writeFile(ev, builds::evidenceText(builds::Evidence{h, "pass", "functional"}));
    builds::Result r;
    CHECK(b.place(tree.string(), ev.string(), r) == builds::kExitOk);
    return h;
}

// Case 4: store format against rollback across real placed builds:
//   - N+1 (5/3) writes appended fields, unknown-change-only record on held session 1, and unknown entry kind ->
//     `builds back` to N (3/3) succeeds and N's DurableStore replays everything (unknown entry shown as Notice).
//   - N+1 (5/5) crashes BEFORE writing format 5 -> `builds back` to N (3/3) succeeds.
//   - N+1 (5/5) DOES write reader=5 -> `builds back` to N refuses (exit 3), and multi-hop 5/5 -> 5/3 -> 3/3 refuses
//     until 5/3 compacts the store to reader <= 4, after which `builds current N` succeeds.
//   - Config version 3 on disk refuses rollback to a build whose `config` writer is 2.
void caseRollbackFormats() {
    const fs::path serverDir = root / "srv";
    fs::create_directories(serverDir);
    ::chmod(serverDir.c_str(), 0700);

    builds::Builds b(serverDir.string(), builds::Options{});
    const std::string hBase = placeTestedBuild(
        b, "b-3-3",
        "format store-record 3 3\nformat store-snapshot 4 4\nformat config 2 2\nformat link 4 1\nformat driver-protocol 1 1\n");
    const std::string hCompat = placeTestedBuild(
        b, "b-5-3",
        "format store-record 5 3\nformat store-snapshot 5 4\nformat config 2 2\nformat link 4 1\nformat driver-protocol 1 1\n");
    const std::string hBreak = placeTestedBuild(
        b, "b-5-5",
        "format store-record 5 5\nformat store-snapshot 5 5\nformat config 2 2\nformat link 4 1\nformat driver-protocol 1 1\n");
    const std::string hCfg3 = placeTestedBuild(
        b, "b-cfg-3",
        "format store-record 3 3\nformat store-snapshot 4 4\nformat config 3 3\nformat link 4 1\nformat driver-protocol 1 1\n");

    builds::Result r;
    CHECK(b.makeCurrent(hBase, r) == builds::kExitOk);

    auto pfs = store::make_posix_fs();
    std::string why;
    {
        DurableStore ds;
        CHECK(ds.open(*pfs, (serverDir / "store").string(), &why));
        const sync::SessionChange mk = sync::infoChange(sync::SessionInfo{"ada", "claude", "main", "opus", "high", ""});
        const sync::SessionChange u1{
            std::make_shared<const sync::SessionEntry>(sync::SessionEntry{EntryKind::Person, "written on N", true}), ""};
        CHECK(ds.commit(1, {mk, u1}, "win-1", 1, &why));
    }

    // 4a. Upgrade N (3/3) -> N+1 (5/3), write format-5 records (reader=3) with appended fields, unknown entry kind 7,
    // and an unknown-change-only record (`Change::Kind = 9`) on session 1; roll back to N and replay via DurableStore.
    CHECK(b.makeCurrent(hCompat, r) == builds::kExitOk);
    {
        std::unique_ptr<store::FileLock> lk;
        CHECK(pfs->lock((serverDir / "store").string(), store::LockMode::Exclusive, lk).is_ok());
        store::Journal j(*pfs, (serverDir / "store").string());
        store::Journal::Recovery rec;
        CHECK(j.open(rec).is_ok());

        // Record 1 from N+1: unknown entry kind 7 + appended entry field + appended record tail.
        {
            store::Writer w;
            w.u8(5);
            w.u8(3);
            w.u64(1);
            w.u32(1);
            w.u8(std::uint8_t(store::Change::Kind::Append));
            store::Writer entry;
            entry.u8(7);
            entry.u8(1);
            entry.str("future artifact card");
            entry.u64(42);
            store::Writer frame;
            frame.bytes(entry.out());
            w.bytes(frame.out());
            w.str("win-1");
            w.u64(2);
            w.str("appended-record-metadata");
            CHECK(j.append(w.out()).is_ok());
        }
        // Record 2 from N+1: ONLY an unknown Change::Kind (9) on session 1 with client counter 3.
        {
            store::Writer w;
            w.u8(5);
            w.u8(3);
            w.u64(1);
            w.u32(1);
            w.u8(9);
            store::Writer frame;
            frame.str("session-pin-state");
            w.bytes(frame.out());
            w.str("win-1");
            w.u64(3);
            CHECK(j.append(w.out()).is_ok());
        }
        CHECK(j.sync().is_ok());
    }

    CHECK(b.back(r) == builds::kExitOk);
    {
        DurableStore ds;
        CHECK(ds.open(*pfs, (serverDir / "store").string(), &why));
        CHECK(ds.report().skipped >= 3);
        CHECK(ds.applied("win-1") == 3);
        const sync::StoredSession* ses = ds.find(1);
        CHECK(ses != nullptr && ses->transcript.size() == 2);
        if (ses != nullptr && ses->transcript.size() == 2) {
            CHECK(ses->transcript[0]->kind == EntryKind::Person);
            CHECK(ses->transcript[0]->text == "written on N");
            CHECK(ses->transcript[1]->kind == EntryKind::Notice);
            CHECK(ses->transcript[1]->text == "future artifact card");
        }
        // Build N continues committing to the same store cleanly.
        const sync::SessionChange u2{
            std::make_shared<const sync::SessionEntry>(sync::SessionEntry{EntryKind::Person, "written after rollback", true}),
            ""};
        CHECK(ds.commit(1, {u2}, "win-1", 4, &why));
    }

    // 4b. Upgrade N (3/3) -> B3 (5/5), B3 crashes BEFORE writing format 5 -> `builds back` to N SUCCEEDS!
    CHECK(b.makeCurrent(hBreak, r) == builds::kExitOk);
    CHECK(b.back(r) == builds::kExitOk);

    // 4c. Upgrade N -> B3 (5/5), B3 DOES write a reader=5 record -> `builds back` refuses (exit 3), and multi-hop
    // B3 -> B2 (5/3) -> B1 (3/3) also refuses until B2 compacts the store to reader <= 4!
    CHECK(b.makeCurrent(hBreak, r) == builds::kExitOk);
    {
        std::unique_ptr<store::FileLock> lk;
        CHECK(pfs->lock((serverDir / "store").string(), store::LockMode::Exclusive, lk).is_ok());
        store::Journal j(*pfs, (serverDir / "store").string());
        store::Journal::Recovery rec;
        CHECK(j.open(rec).is_ok());
        store::Writer w;
        w.u8(5);
        w.u8(5);
        w.u64(1);
        w.u32(1);
        w.u8(std::uint8_t(store::Change::Kind::Enqueue));
        store::Writer frame;
        frame.str("breaking-prompt");
        w.bytes(frame.out());
        w.str("");
        w.u64(0);
        CHECK(j.append(w.out()).is_ok());
        CHECK(j.sync().is_ok());
    }
    CHECK(b.back(r) == builds::kExitRefused);
    CHECK(!r.err.empty() && has(r.err[0], "store-record") && has(r.err[0], "requires a reader of at least 5"));

    // Multi-hop: B3 (5/5) -> B2 (5/3) is allowed, then B2 (5/3) -> B1 (3/3) is REFUSED while reader 5 is in the log!
    CHECK(b.makeCurrent(hCompat, r) == builds::kExitOk);
    CHECK(b.makeCurrent(hBase, r) == builds::kExitRefused);
    CHECK(!r.err.empty() && has(r.err[0], "store-record") && has(r.err[0], "requires a reader of at least 5"));

    // Once B2 compacts the store into a snapshot with reader = 4 (clearing the reader=5 log record), B2 -> B1 succeeds!
    {
        std::unique_ptr<store::FileLock> lk;
        CHECK(pfs->lock((serverDir / "store").string(), store::LockMode::Exclusive, lk).is_ok());
        store::Journal j(*pfs, (serverDir / "store").string());
        store::Journal::Recovery rec;
        CHECK(j.open(rec).is_ok());
        store::Writer snap;
        snap.u8(5);
        snap.u8(4);
        snap.u64(2);
        snap.u32(1);
        {
            store::Writer s;
            s.u64(1);
            for (const char* f : {"ada", "claude", "main", "opus", "high"}) {
                s.str(f);
            }
            s.u32(1);
            {
                store::Writer e;
                e.u8(0);
                e.u8(1);
                e.str("compacted by B2");
                s.bytes(e.out());
            }
            s.u32(0);
            s.u64(1);
            s.u64(0);
            s.str("");
            s.u32(0);
            s.u8(0);
            s.str("");
            s.str("");
            s.str("b2-extra-session-tail");
            snap.bytes(s.out());
        }
        snap.u32(0);
        CHECK(j.compact(snap.out()).is_ok());
    }
    CHECK(b.makeCurrent(hBase, r) == builds::kExitOk);
    {
        DurableStore ds;
        CHECK(ds.open(*pfs, (serverDir / "store").string(), &why));
        const sync::StoredSession* ses = ds.find(1);
        CHECK(ses != nullptr && ses->transcript.size() == 1 && ses->transcript[0]->text == "compacted by B2");
    }

    // 4d. Config version 3 in nexus-config/accounts.json refuses rollback from hCfg3 (3/3) to hBase (2/2).
    builds::Result rCfg;
    CHECK(b.makeCurrent(hCfg3, rCfg) == builds::kExitOk);
    writeFile(serverDir / "nexus-config" / "accounts.json", "{\"version\": 3, \"accounts\": []}\n", false, 0600);
    rCfg = builds::Result{};
    CHECK(b.back(rCfg) == builds::kExitRefused);
    CHECK(!rCfg.err.empty() && has(rCfg.err[0], "accounts.json") && has(rCfg.err[0], "requires a reader of at least 3"));
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: migration_functional <case> <dir>\n");
        return 2;
    }
    const std::string which = argv[1];
    root = fs::path(argv[2]);
    removeAll(root);
    fs::create_directories(root);
    struct CleanGuard {
        ~CleanGuard() { removeAll(root); }
    } clean;

    if (which == "old_to_new") {
        caseOldToNew();
    } else if (which == "live_old_refused") {
        caseLiveOldRefused();
    } else if (which == "crash_resume_and_conflict") {
        caseCrashResumeAndConflict();
    } else if (which == "rollback_formats") {
        caseRollbackFormats();
    } else {
        std::fprintf(stderr, "unknown case: %s\n", which.c_str());
        return 2;
    }

    if (failures != 0) {
        std::fprintf(stderr, "migration_functional (%s): %d check(s) failed\n", which.c_str(), failures);
        return 1;
    }
    std::printf("migration_functional (%s): all checks passed\n", which.c_str());
    return 0;
}
