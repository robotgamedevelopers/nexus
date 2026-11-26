// migration_walk: a random walk of the real layer 5 migration, store-reader and build-rollback code
// (src/server/migration.cpp, src/store/store.cpp, src/server/builds.cpp) on real POSIX files and locks, written as a
// trace for models/migration/MigrationTrace.tla.
//
// Usage:
//   migration_walk <seed> <steps>
//     [--fault <no-lock|ignore-live-old|clobber-new|no-sync|no-plan|leave-old|blind-back|multihop|refuse-unknown-change>]
//     <out.tla>

#include "server/builds.h"
#include "server/migration.h"
#include "store/codec.h"
#include "store/fs.h"
#include "store/journal.h"
#include "store/store.h"

#include <algorithm>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <span>
#include <sstream>
#include <string>
#include <sys/file.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;
using namespace nexus;
using namespace nexus::server;

namespace {

struct Killed {};

enum class WalkFault {
    None,
    NoLock,
    IgnoreLiveOld,
    ClobberNew,
    NoSync,
    NoPlan,
    LeaveOld,
    BlindBack,
    MultiHop,
    RefuseUnknownChange
};

std::uint64_t rngState = 0;
std::uint64_t nextRng() {
    std::uint64_t z = (rngState += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}
std::uint64_t below(std::uint64_t n) {
    return n == 0 ? 0 : nextRng() % n;
}

[[noreturn]] void die(const std::string& why) {
    std::fprintf(stderr, "migration_walk: %s\n", why.c_str());
    std::fflush(stderr);
    std::_Exit(1);
}

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
        die("cannot remove " + p.string() + ": " + ec.message());
    }
}

void writeHostFile(const fs::path& p, const std::string& text, bool exec, int mode = -1) {
    fs::create_directories(p.parent_path());
    std::ofstream out(p, std::ios::binary);
    out << text;
    out.close();
    if (!out) {
        die("cannot write " + p.string());
    }
    if (mode >= 0) {
        ::chmod(p.c_str(), static_cast<mode_t>(mode));
    } else {
        fs::permissions(p, exec ? fs::perms(0755) : fs::perms(0644));
    }
}

std::string readHostFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) {
        return {};
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::string strSet(const std::set<std::string>& s) {
    std::string out = "{";
    for (const std::string& x : s) {
        if (out.size() > 1) {
            out += ", ";
        }
        out += "\"" + x + "\"";
    }
    return out + "}";
}

std::mutex gm;
std::condition_variable gcv;

struct Actor {
    std::string name;
    bool busy = false;
    bool parked = false;
    bool go = false;
    bool kill = false;
    bool quit = false;
    std::uint64_t events = 0;
    std::string at = "idle";
    std::string part = "none";
    migration::Report lastReport;
    store::Status lastStatus = store::Status::ok();
    std::function<void()> job;
    bool hasJob = false;
    std::thread thread;
};

void park(Actor& a, const std::string& phase, const std::string& part) {
    if (phase == "idle") {
        return;
    }
    std::unique_lock<std::mutex> lk(gm);
    a.at = phase;
    a.part = part;
    a.parked = true;
    ++a.events;
    gcv.notify_all();
    gcv.wait(lk, [&] { return a.go; });
    a.go = false;
    a.parked = false;
    if (a.kill) {
        a.kill = false;
        throw Killed{};
    }
}

void actorLoop(Actor& a) {
    for (;;) {
        std::function<void()> job;
        {
            std::unique_lock<std::mutex> lk(gm);
            gcv.wait(lk, [&] { return a.hasJob || a.quit; });
            if (!a.hasJob) {
                return;
            }
            job = a.job;
            a.hasJob = false;
            a.busy = true;
        }
        try {
            job();
        } catch (const Killed&) {
        }
        std::unique_lock<std::mutex> lk(gm);
        a.busy = false;
        a.parked = false;
        a.at = "idle";
        a.part = "none";
        ++a.events;
        gcv.notify_all();
    }
}

void advance(Actor& a, bool start, std::function<void()> job = {}) {
    std::unique_lock<std::mutex> lk(gm);
    const std::uint64_t before = a.events;
    if (start) {
        a.job = std::move(job);
        a.hasJob = true;
    } else {
        a.go = true;
    }
    gcv.notify_all();
    gcv.wait(lk, [&] { return a.events != before; });
}

void killActor(Actor& a) {
    {
        std::unique_lock<std::mutex> lk(gm);
        if (!a.busy) {
            return;
        }
        a.kill = true;
    }
    advance(a, false);
}

// Encodes a framed record (format 5, reader `reader`) appending one text entry (with an extra trailing field) on
// session 1, or an unknown-change-only record (`kind = 9`) on session 1.
store::Bytes makeFormat5Append(std::uint8_t reader, const std::string& text) {
    store::Writer w;
    w.u8(5);
    w.u8(reader);
    w.u64(1);
    w.u32(1);
    w.u8(std::uint8_t(store::Change::Kind::Append));
    {
        store::Writer entry;
        entry.u8(7);    // unknown entry kind (shown as Notice by older server)
        entry.u8(1);
        entry.str(text);
        entry.u64(99);  // appended entry field
        store::Writer frame;
        frame.bytes(entry.out());
        w.bytes(frame.out());
    }
    w.str("");
    w.u64(0);
    w.str("later-record-tail");
    return w.out();
}

store::Bytes makeFormat5UnknownChangeOnly(std::uint8_t reader) {
    store::Writer w;
    w.u8(5);
    w.u8(reader);
    w.u64(1);
    w.u32(1);
    w.u8(9);    // Change::Kind unknown to build 1
    {
        store::Writer frame;
        frame.str("session-metadata");
        w.bytes(frame.out());
    }
    w.str("");
    w.u64(0);
    return w.out();
}

store::Bytes makeSnapshotPayload(std::uint8_t snapFmt, std::uint8_t snapReader, std::size_t entryCount) {
    store::Writer w;
    w.u8(snapFmt);
    w.u8(snapReader);
    w.u64(2);   // next_session_ = 2
    w.u32(1);   // 1 session
    {
        store::Writer s;
        s.u64(1);
        for (const char* f : {"ada", "claude", "main", "opus", "high"}) {
            s.str(f);
        }
        s.u32(static_cast<std::uint32_t>(entryCount));
        for (std::size_t i = 0; i < entryCount; ++i) {
            store::Writer e;
            e.u8(0);
            e.u8(1);
            e.str("entry-" + std::to_string(i + 1));
            s.bytes(e.out());
        }
        s.u32(0);   // queue
        s.u64(1);   // next_turn
        s.u64(0);   // open_turn
        s.str("");  // driver session
        s.u32(0);   // waiting
        s.u8(0);    // summary
        s.str("");  // session_dir
        s.str("");  // instance
        if (snapFmt > 4) {
            s.str("later-session-tail");
        }
        w.bytes(s.out());
    }
    w.u32(0);   // clients
    return w.out();
}

struct Walk {
    fs::path work;
    fs::path topDir;
    fs::path serverDir;
    WalkFault fault = WalkFault::None;

    std::set<std::string> activeParts{"snap", "log", "drivers"};
    std::set<std::string> initTop{"snap", "log", "drivers"};
    bool initTopSock = true;
    std::string initOldSrv = "sock";
    std::set<std::string> initDir{};

    std::string oldSnapBytes;
    std::string oldLogBytes;
    std::string oldDriversBytes = "driver claude /usr/bin/node /d/main.js --account main\nai ada claude\n";
    std::string newLogBytes;

    std::string oldSrv = "sock";
    std::unique_ptr<store::Fs> oldHoldFs;
    std::unique_ptr<store::FileLock> oldHoldLock;

    std::map<int, std::string> hashOfBuild;
    std::map<std::string, int> buildOfHash;

    bool refusedConflict = false;
    bool srvUp = false;
    bool openFailed = false;
    std::uint32_t liveMaxReader = 0;
    std::size_t liveBaseEntries = 0;
    std::unique_ptr<store::Fs> srvFs;
    std::unique_ptr<store::Store> srvStore;
    std::unique_ptr<store::FileLock> srvRawLock;
    std::unique_ptr<store::Journal> srvRawJournal;

    Actor actors[2];
    std::string lockOwnerHint = "none";
    std::vector<std::string> records;

    migration::Fault migFault() const {
        switch (fault) {
        case WalkFault::NoLock:
            return migration::Fault::NoLock;
        case WalkFault::IgnoreLiveOld:
            return migration::Fault::IgnoreLiveOld;
        case WalkFault::ClobberNew:
            return migration::Fault::ClobberNew;
        case WalkFault::NoSync:
            return migration::Fault::NoSync;
        case WalkFault::NoPlan:
            return migration::Fault::NoPlan;
        case WalkFault::LeaveOld:
            return migration::Fault::LeaveOld;
        default:
            return migration::Fault::None;
        }
    }

    builds::Fault buildsFault() const {
        if (fault == WalkFault::BlindBack) {
            return builds::Fault::BlindBack;
        }
        if (fault == WalkFault::MultiHop) {
            return builds::Fault::MultiHopNoStoreCheck;
        }
        return builds::Fault::None;
    }

    // Checks whether ~/.nexus/server is flock'd exclusively in the OS right now.
    std::string probeLockHolder() const {
        const int fd = ::open(serverDir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (fd < 0) {
            return "none";
        }
        const bool freeLock = (::flock(fd, LOCK_EX | LOCK_NB) == 0);
        if (freeLock) {
            ::flock(fd, LOCK_UN);
        }
        ::close(fd);
        if (freeLock) {
            return "none";
        }
        return lockOwnerHint;
    }

    std::pair<int, int> activeBuilds() const {
        std::error_code ec;
        const fs::path act = serverDir / "builds" / "active";
        if (!fs::is_symlink(act, ec)) {
            return {1, 0};
        }
        const std::string target = fs::read_symlink(act, ec).string();
        std::string c;
        std::string p;
        if (!builds::readPairName(target, &c, &p)) {
            return {1, 0};
        }
        const int cb = buildOfHash.count(c) ? buildOfHash.at(c) : 1;
        const int pb = (!p.empty() && buildOfHash.count(p)) ? buildOfHash.at(p) : 0;
        return {cb, pb};
    }

    void placeThreeBuilds() {
        const std::pair<int, std::string> specs[3] = {
            {1, "format store-record 3 3\nformat store-snapshot 4 4\nformat config 2 2\nformat link 4 1\nformat driver-protocol 1 1\n"},
            {2, "format store-record 5 3\nformat store-snapshot 5 4\nformat config 2 2\nformat link 4 1\nformat driver-protocol 1 1\n"},
            {3, "format store-record 5 5\nformat store-snapshot 5 5\nformat config 2 2\nformat link 4 1\nformat driver-protocol 1 1\n"},
        };
        for (const auto& [num, fmts] : specs) {
            const fs::path stage = work / ("stage-" + std::to_string(num));
            writeHostFile(stage / "bin" / "nexus-server", "#!/bin/sh\necho build-" + std::to_string(num) + "\n", true);
            writeHostFile(stage / "drivers" / "claude" / "main.js", "driver-" + std::to_string(num) + "\n", false);
            writeHostFile(stage / "formats", fmts, false);
            std::string h;
            if (!builds::hashTree(stage.string(), &h).is_ok()) {
                die("hashTree failed");
            }
            const fs::path ev = work / ("ev-" + std::to_string(num));
            writeHostFile(ev, builds::evidenceText(builds::Evidence{h, "pass", "walk"}), false);
            builds::Builds b(serverDir.string(), builds::Options{});
            builds::Result r;
            if (b.place(stage.string(), ev.string(), r) != builds::kExitOk) {
                die("place failed");
            }
            hashOfBuild[num] = h;
            buildOfHash[h] = num;
            if (num == 1 && b.makeCurrent(h, r) != builds::kExitOk) {
                die("makeCurrent(1) failed");
            }
        }
    }

    void setup(std::uint64_t seed) {
        removeAll(work);
        fs::create_directories(topDir);
        fs::create_directories(serverDir);
        ::chmod(topDir.c_str(), 0700);
        ::chmod(serverDir.c_str(), 0700);

        if (fault == WalkFault::ClobberNew || seed == 4) {
            initDir = {"log"};
            initTopSock = false;
            initOldSrv = "off";
        } else if (fault == WalkFault::BlindBack || fault == WalkFault::MultiHop ||
                   fault == WalkFault::RefuseUnknownChange) {
            initTop = {};
            initDir = {"log"};
            initTopSock = false;
            initOldSrv = "off";
        } else if (seed == 2) {
            initOldSrv = "locked";
        } else if (seed == 3) {
            initOldSrv = "off";
            initTopSock = false;
        }
        oldSrv = initOldSrv;

        placeThreeBuilds();

        if (!initTop.empty()) {
            auto fs = store::make_posix_fs();
            store::StoreOptions small;
            small.compact_bytes = 64;
            {
                std::unique_ptr<store::Store> s;
                if (!store::Store::open(*fs, (topDir / "store").string(), small, s).is_ok()) {
                    die("open old store failed");
                }
                store::Change mk;
                mk.kind = store::Change::Kind::SetInfo;
                mk.info = store::Info{"ada", "claude", "main", "opus", "high", ""};
                store::Change msg;
                msg.kind = store::Change::Kind::Append;
                msg.entry = store::Entry{0, "entry-1", true};
                if (!s->commit(1, std::vector<store::Change>{mk, msg}, "w1", 1).is_ok()) {
                    die("commit 1 old store failed");
                }
                store::Change q;
                q.kind = store::Change::Kind::Enqueue;
                q.prompt = "queued";
                if (!s->commit(1, std::vector<store::Change>{q}, "w1", 2).is_ok()) {
                    die("commit 2 old store failed");
                }
            }
            ::unlink((topDir / "store" / store::kGateName).c_str());
            oldSnapBytes = readHostFile(topDir / "store" / "snapshot");
            oldLogBytes = readHostFile(topDir / "store" / "log");
            if (oldSnapBytes.empty() || oldLogBytes.empty()) {
                die("expected both snapshot and log in old store");
            }
            if (initTop.count("drivers") != 0) {
                writeHostFile(topDir / "drivers", oldDriversBytes, false, 0600);
            }
        }
        if (initTopSock) {
            writeHostFile(topDir / "server.sock", "", false, 0600);
        }
        if (oldSrv == "locked") {
            oldHoldFs = store::make_posix_fs();
            if (!oldHoldFs->lock((topDir / "store").string(), store::LockMode::Exclusive, oldHoldLock).is_ok()) {
                die("failed to lock old store for InitOldSrv=locked");
            }
        }
        if (initDir.count("log") != 0) {
            auto fs = store::make_posix_fs();
            std::unique_ptr<store::Store> s;
            if (!store::Store::open(*fs, (serverDir / "store").string(), store::StoreOptions{}, s).is_ok()) {
                die("open initDir store failed");
            }
            store::Change mk;
            mk.kind = store::Change::Kind::SetInfo;
            mk.info = store::Info{"ada", "claude", "main", "opus", "high", ""};
            store::Change msg;
            msg.kind = store::Change::Kind::Append;
            msg.entry = store::Entry{0, "entry-1", true};
            if (!s->commit(1, std::vector<store::Change>{mk, msg}, "w1", 1).is_ok()) {
                die("commit initDir store failed");
            }
            newLogBytes = readHostFile(serverDir / "store" / "log");
            liveMaxReader = 3;
            liveBaseEntries = 1;
        }

        actors[0].name = "a1";
        actors[1].name = "a2";
        for (Actor& a : actors) {
            a.thread = std::thread([&a] { actorLoop(a); });
        }
    }

    int dirPartValue(const std::string& p) const {
        fs::path path = p == "snap" ? (serverDir / "store" / "snapshot") :
                        p == "log" ? (serverDir / "store" / "log") :
                        (serverDir / p);
        std::error_code ec;
        if (!fs::exists(path, ec)) {
            return 0;
        }
        const std::string bytes = readHostFile(path);
        if (p == "snap") {
            return initTop.count("snap") ? 1 : 2;
        }
        if (p == "log") {
            if (!oldLogBytes.empty() && bytes == oldLogBytes) {
                return 1;
            }
            if (!newLogBytes.empty() && bytes == newLogBytes) {
                return 2;
            }
            return initTop.count("log") ? 1 : 2;
        }
        if (p == "drivers") {
            if (!oldDriversBytes.empty() && bytes == oldDriversBytes) {
                return 1;
            }
            return initTop.count("drivers") ? 1 : 2;
        }
        return 1;
    }

    void refreshDiskMetrics() {
        if (srvUp) {
            return;
        }
        std::error_code ec;
        if (!fs::exists(serverDir / "store" / "log", ec) && !fs::exists(serverDir / "store" / "snapshot", ec)) {
            liveMaxReader = 0;
            liveBaseEntries = 0;
            return;
        }
        auto fs = store::make_posix_fs(store::PosixOptions{
            [] { return std::uint64_t(1000); }, [](std::uint64_t) {}, 0, {}, false, false});
        store::StoreReaders sr;
        const store::Status s = store::Store::inspect_readers(*fs, (serverDir / "store").string(), sr);
        if (s.is_ok() && sr.exists) {
            const std::uint32_t snapR = sr.snapshot_reader >= 5 ? 5 : (sr.snapshot_reader > 0 ? 3 : 0);
            const std::uint32_t recR = sr.record_reader >= 5 ? 5 : (sr.record_reader > 0 ? 3 : 0);
            liveMaxReader = std::max(snapR, recR);
            if (liveBaseEntries == 0) {
                liveBaseEntries = 1;
            }
        } else if (s.code() == store::Status::Code::Busy) {
            // `migrateOldLayout` holds `serverDir/store` exclusively while placing the old store (reader 3, 1 session).
            if (liveMaxReader == 0) {
                liveMaxReader = 3;
            }
            if (liveBaseEntries == 0) {
                liveBaseEntries = 1;
            }
        }
    }

    void record(const std::string& op, const std::string& p = "", const std::string& part = "none", int b = 0) {
        refreshDiskMetrics();
        std::set<std::string> topSet;
        std::error_code ec;
        if (fs::exists(topDir / "store" / "snapshot", ec)) {
            topSet.insert("snap");
        }
        if (fs::exists(topDir / "store" / "log", ec)) {
            topSet.insert("log");
        }
        if (fs::exists(topDir / "drivers", ec)) {
            topSet.insert("drivers");
        }
        const bool topSock = fs::exists(topDir / "server.sock", ec);

        std::set<std::string> planSet;
        if (fs::exists(serverDir / migration::kPlanFile, ec)) {
            planSet = migration::parsePlan(readHostFile(serverDir / migration::kPlanFile));
            planSet.erase("server.log");
            planSet.erase("config");
        }

        const auto [cb, pb] = activeBuilds();
        const std::string lk = probeLockHolder();

        std::string dirRec = "[";
        bool first = true;
        for (const std::string& ap : activeParts) {
            if (!first) {
                dirRec += ", ";
            }
            first = false;
            dirRec += ap + " |-> " + std::to_string(dirPartValue(ap));
        }
        dirRec += "]";

        std::ostringstream s;
        s << "[op |-> \"" << op << "\", p |-> \"" << p << "\", part |-> \"" << part << "\", b |-> " << b
          << ", pcs |-> <<\"" << actors[0].at << "\", \"" << actors[1].at << "\">>"
          << ", lock |-> \"" << lk << "\", oldSrv |-> \"" << oldSrv << "\""
          << ", top |-> " << strSet(topSet) << ", topSock |-> " << (topSock ? "TRUE" : "FALSE")
          << ", dir |-> " << dirRec << ", plan |-> " << strSet(planSet)
          << ", refused |-> " << (refusedConflict ? "TRUE" : "FALSE")
          << ", curBuild |-> " << cb << ", prevBuild |-> " << pb
          << ", srvUp |-> " << (srvUp ? "TRUE" : "FALSE")
          << ", maxReader |-> " << liveMaxReader << ", baseEntries |-> " << liveBaseEntries
          << ", openFailed |-> " << (openFailed ? "TRUE" : "FALSE") << "]";
        records.push_back(s.str());
    }

    void startActor(Actor& a) {
        advance(a, true, [this, &a] {
            auto fs = store::make_posix_fs(store::PosixOptions{
                [] { return std::uint64_t(1000); }, [](std::uint64_t) {}, 0, {}, false, false});
            migration::Options o;
            o.fault = migFault();
            o.old_server_live = [this] { return oldSrv == "sock"; };
            o.hook = [&a](const std::string& ph, const std::string& pt) { park(a, ph, pt); };
            a.lastStatus = migration::migrateOldLayout(*fs, topDir.string(), serverDir.string(), o, &a.lastReport);
        });
        record("start", a.name);
    }

    void stepActor(Actor& a) {
        const std::string op = a.at;
        const std::string pt = a.part;
        advance(a, false);
        if (op == "lock" && a.at == "probe" && migFault() != migration::Fault::NoLock) {
            lockOwnerHint = a.name;
        }
        if (a.at == "idle" && lockOwnerHint == a.name) {
            lockOwnerHint = "none";
        }
        if (op == "plan" && a.at == "unlock" && a.lastReport.conflict) {
            refusedConflict = true;
        }
        record(op, a.name, pt);
    }

    void closeSrvHandles() {
        srvStore.reset();
        srvRawJournal.reset();
        srvRawLock.reset();
        srvFs.reset();
        srvUp = false;
    }

    void crashAll() {
        for (Actor& a : actors) {
            killActor(a);
        }
        lockOwnerHint = "none";
        closeSrvHandles();
        record("crash");
    }

    bool topCleanOnDisk() const {
        std::error_code ec;
        return !fs::exists(topDir / "store" / "snapshot", ec) &&
               !fs::exists(topDir / "store" / "log", ec) &&
               !fs::exists(topDir / "drivers", ec) &&
               !fs::exists(topDir / "server.sock", ec);
    }

    bool hasConflictInit() const {
        return !initTop.empty() && !initDir.empty();
    }

    void trySwitch(int target) {
        const auto [cb, pb] = activeBuilds();
        (void)pb;
        if (target == cb) {
            return;
        }
        builds::Options bo;
        bo.fault = buildsFault();
        builds::Builds b(serverDir.string(), bo);
        builds::Result r;
        if (b.makeCurrent(hashOfBuild[target], r) == builds::kExitOk) {
            record("switch", "", "none", target);
        }
    }

    void tryRollback() {
        builds::Options bo;
        bo.fault = buildsFault();
        builds::Builds b(serverDir.string(), bo);
        builds::Result r;
        if (b.back(r) == builds::kExitOk) {
            record("rollback");
        }
    }

    void doSrvOpen() {
        const auto [cb, pb] = activeBuilds();
        (void)pb;
        srvFs = store::make_posix_fs(store::PosixOptions{
            [] { return std::uint64_t(1000); }, [](std::uint64_t) {}, 0, {}, false, false});
        if (cb == 1) {
            store::StoreOptions so;
            so.test_refuse_unknown_change = (fault == WalkFault::RefuseUnknownChange);
            const store::Status s = store::Store::open(*srvFs, (serverDir / "store").string(), so, srvStore);
            if (!s.is_ok()) {
                openFailed = true;
                srvFs.reset();
            } else {
                srvUp = true;
                const auto* ses = srvStore->session(1);
                liveBaseEntries = ses == nullptr ? 0 : ses->transcript.size();
                liveMaxReader = 3;
            }
        } else {
            store::StoreReaders sr;
            if (!store::Store::inspect_readers(*srvFs, (serverDir / "store").string(), sr).is_ok() ||
                sr.record_reader > 5 || sr.snapshot_reader > 5) {
                openFailed = true;
                srvFs.reset();
            } else {
                if (!srvFs->make_dir((serverDir / "store").string()).is_ok() ||
                    !srvFs->lock((serverDir / "store").string(), store::LockMode::Exclusive, srvRawLock).is_ok()) {
                    die("failed locking store for build >= 2");
                }
                srvRawJournal = std::make_unique<store::Journal>(*srvFs, (serverDir / "store").string());
                store::Journal::Recovery rec;
                if (!srvRawJournal->open(rec).is_ok()) {
                    openFailed = true;
                    closeSrvHandles();
                } else {
                    srvUp = true;
                    const std::uint32_t snapR = sr.snapshot_reader >= 5 ? 5 : (sr.snapshot_reader > 0 ? 3 : 0);
                    const std::uint32_t recR = sr.record_reader >= 5 ? 5 : (sr.record_reader > 0 ? 3 : 0);
                    liveMaxReader = std::max(snapR, recR);
                }
            }
        }
        record("srv_open");
    }

    void doSrvWrite() {
        const auto [cb, pb] = activeBuilds();
        (void)pb;
        if (cb == 1 && srvStore) {
            store::Change msg;
            msg.kind = store::Change::Kind::Append;
            msg.entry = store::Entry{0, "entry-" + std::to_string(liveBaseEntries + 1), true};
            if (!srvStore->commit(1, std::vector<store::Change>{msg}).is_ok()) {
                die("commit on build 1 failed");
            }
            ++liveBaseEntries;
            liveMaxReader = std::max<std::uint32_t>(liveMaxReader, 3);
        } else if (srvRawJournal) {
            const std::uint8_t rdr = cb == 3 ? 5 : 3;
            const store::Bytes a = makeFormat5Append(rdr, "entry-" + std::to_string(liveBaseEntries + 1));
            if (!srvRawJournal->append(a).is_ok()) {
                die("raw append failed");
            }
            if (cb == 2) {
                const store::Bytes u = makeFormat5UnknownChangeOnly(3);
                if (!srvRawJournal->append(u).is_ok()) {
                    die("raw unknown-change append failed");
                }
            }
            if (!srvRawJournal->sync().is_ok()) {
                die("raw sync failed");
            }
            ++liveBaseEntries;
            liveMaxReader = std::max<std::uint32_t>(liveMaxReader, rdr);
        }
        record("srv_write");
    }

    void doSrvCompact() {
        const auto [cb, pb] = activeBuilds();
        (void)pb;
        if (cb == 1 && srvStore) {
            // Re-open with compact_bytes = 1 or compact journal through Store: easiest is closing and compacting the
            // journal under a fresh Store or writing a format-4 snapshot directly.
            const std::size_t n = liveBaseEntries;
            srvStore.reset();
            if (!srvFs->lock((serverDir / "store").string(), store::LockMode::Exclusive, srvRawLock).is_ok()) {
                die("relock for compact failed");
            }
            store::Journal j(*srvFs, (serverDir / "store").string());
            store::Journal::Recovery rec;
            if (!j.open(rec).is_ok()) {
                die("journal open for compact failed");
            }
            if (j.last_seq() == 0) {
                const store::Bytes dummy = makeFormat5UnknownChangeOnly(3);
                (void)j.append(dummy);
                (void)j.sync();
            }
            if (!j.compact(makeSnapshotPayload(4, 4, n)).is_ok()) {
                die("compact build 1 failed");
            }
            srvRawLock.reset();
            if (!store::Store::open(*srvFs, (serverDir / "store").string(), store::StoreOptions{}, srvStore).is_ok()) {
                die("reopen after compact build 1 failed");
            }
            liveMaxReader = 3;
        } else if (srvRawJournal) {
            const std::uint8_t fmt = cb == 3 ? 5 : 5;
            const std::uint8_t rdr = cb == 3 ? 5 : 4;
            if (srvRawJournal->last_seq() == 0) {
                const store::Bytes a = makeFormat5Append(cb == 3 ? 5 : 3, "seed");
                (void)srvRawJournal->append(a);
                (void)srvRawJournal->sync();
            }
            if (!srvRawJournal->compact(makeSnapshotPayload(fmt, rdr, liveBaseEntries)).is_ok()) {
                die("raw compact failed");
            }
            liveMaxReader = cb == 3 ? 5 : 3;
        }
        record("srv_compact");
    }

    void doSrvStop() {
        closeSrvHandles();
        record("srv_stop");
    }

    void run(std::size_t steps) {
        record("init");

        // Deterministic fault sequences first when a specific fault is requested, so the fault is always reached
        // promptly regardless of RNG stream.
        if (fault == WalkFault::NoLock && steps >= 4) {
            startActor(actors[0]);
            stepActor(actors[0]);  // Fault::NoLock advances to `probe` with migLock = "none"!
        } else if (fault == WalkFault::IgnoreLiveOld && steps >= 5) {
            startActor(actors[0]);
            stepActor(actors[0]);
            stepActor(actors[0]);  // Fault::IgnoreLiveOld advances `probe -> plan` while oldSrv = "sock"!
        } else if (fault == WalkFault::BlindBack && steps >= 8) {
            trySwitch(3);
            doSrvOpen();
            doSrvWrite();
            doSrvStop();
            tryRollback();  // Fault::BlindBack allows 3 -> 1 with maxReader = 5!
        } else if (fault == WalkFault::MultiHop && steps >= 9) {
            trySwitch(3);
            doSrvOpen();
            doSrvWrite();
            doSrvStop();
            trySwitch(2);
            trySwitch(1);   // Fault::MultiHopNoStoreCheck allows 2 -> 1 with maxReader = 5!
        } else if (fault == WalkFault::RefuseUnknownChange && steps >= 8) {
            trySwitch(2);
            doSrvOpen();
            doSrvWrite();
            doSrvStop();
            tryRollback();
            doSrvOpen();    // test_refuse_unknown_change fails Store::open on build 1!
        }

        while (records.size() < steps && !openFailed) {
            std::vector<std::pair<int, std::function<void()>>> c;

            if (oldSrv != "off") {
                c.push_back({3, [this] {
                    oldHoldLock.reset();
                    oldHoldFs.reset();
                    oldSrv = "off";
                    record("old_stop");
                }});
            }

            const bool planExists = fs::exists(serverDir / migration::kPlanFile);
            for (Actor& a : actors) {
                if (!a.busy) {
                    if (!topCleanOnDisk() || planExists || below(6) == 0) {
                        c.push_back({4, [this, &a] { startActor(a); }});
                    }
                } else {
                    c.push_back({20, [this, &a] { stepActor(a); }});
                    c.push_back({1, [this, &a] {
                        killActor(a);
                        if (lockOwnerHint == a.name) {
                            lockOwnerHint = "none";
                        }
                        record("die", a.name);
                    }});
                }
            }

            c.push_back({1, [this] { crashAll(); }});

            const bool noMigActors = !actors[0].busy && !actors[1].busy && !planExists;
            const bool storeReady = topCleanOnDisk() ||
                                    (fault == WalkFault::LeaveOld && fs::exists(serverDir / "store" / "log"));
            if (noMigActors && !hasConflictInit() && storeReady) {
                if (!srvUp) {
                    c.push_back({5, [this] { doSrvOpen(); }});
                } else {
                    c.push_back({6, [this] { doSrvWrite(); }});
                    c.push_back({3, [this] { doSrvCompact(); }});
                    c.push_back({4, [this] { doSrvStop(); }});
                }
                if (topCleanOnDisk() && !srvUp) {
                    for (int b : {1, 2, 3}) {
                        c.push_back({3, [this, b] { trySwitch(b); }});
                    }
                    if (activeBuilds().second != 0) {
                        c.push_back({4, [this] { tryRollback(); }});
                    }
                }
            }

            int total = 0;
            for (const auto& x : c) {
                total += x.first;
            }
            if (total == 0) {
                break;
            }
            int pick = int(below(std::uint64_t(total)));
            for (const auto& x : c) {
                if (pick < x.first) {
                    x.second();
                    break;
                }
                pick -= x.first;
            }
        }
    }

    void teardown() {
        for (Actor& a : actors) {
            if (a.thread.joinable()) {
                killActor(a);
                {
                    std::unique_lock<std::mutex> lk(gm);
                    a.quit = true;
                }
                gcv.notify_all();
                a.thread.join();
            }
        }
        closeSrvHandles();
        oldHoldLock.reset();
        oldHoldFs.reset();
        removeAll(work);
    }

    void write(const std::string& out) const {
        std::ofstream f(out);
        f << "---- MODULE MigrationTraceData ----\n";
        f << "\\* Written by tests/server/migration_walk.cpp; checked by MigrationTrace.tla.\n";
        f << "TActiveParts == " << strSet(activeParts) << "\n";
        f << "TInitTop == " << strSet(initTop) << "\n";
        f << "TInitTopSock == " << (initTopSock ? "TRUE" : "FALSE") << "\n";
        f << "TInitOldSrv == \"" << initOldSrv << "\"\n";
        f << "TInitDir == " << strSet(initDir) << "\n";
        f << "Trace == <<\n";
        for (std::size_t i = 0; i < records.size(); ++i) {
            f << "  " << records[i] << (i + 1 < records.size() ? ",\n" : "\n");
        }
        f << ">>\n====\n";
        f.close();
        if (!f) {
            die("cannot write " + out);
        }
    }
};

} // namespace

int main(int argc, char** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);
    std::string faultName;
    std::vector<std::string> plain;
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--fault" && i + 1 < args.size()) {
            faultName = args[++i];
        } else {
            plain.push_back(args[i]);
        }
    }
    if (plain.size() != 3) {
        die("usage: migration_walk <seed> <steps> [--fault <name>] <out.tla>");
    }
    static const std::map<std::string, WalkFault> faults{
        {"", WalkFault::None},
        {"no-lock", WalkFault::NoLock},
        {"ignore-live-old", WalkFault::IgnoreLiveOld},
        {"clobber-new", WalkFault::ClobberNew},
        {"no-sync", WalkFault::NoSync},
        {"no-plan", WalkFault::NoPlan},
        {"leave-old", WalkFault::LeaveOld},
        {"blind-back", WalkFault::BlindBack},
        {"multihop", WalkFault::MultiHop},
        {"refuse-unknown-change", WalkFault::RefuseUnknownChange},
    };
    const auto f = faults.find(faultName);
    if (f == faults.end()) {
        die("no fault `" + faultName + "`");
    }

    static Walk w;
    struct Guard {
        Walk& walk;
        ~Guard() { walk.teardown(); }
    } guard{w};

    w.fault = f->second;
    const std::uint64_t seed = std::strtoull(plain[0].c_str(), nullptr, 10);
    rngState = seed;
    const std::size_t steps = std::strtoull(plain[1].c_str(), nullptr, 10);
    w.work = fs::path(plain[2]).parent_path() / "walk-files";
    w.topDir = w.work / ".nexus";
    w.serverDir = w.topDir / "server";
    w.setup(seed);
    w.run(steps);
    guard.walk.teardown();
    w.write(plain[2]);
    std::printf("migration_walk: seed %s, %zu steps, fault %s, written to %s\n", plain[0].c_str(), w.records.size(),
                faultName.empty() ? "none" : faultName.c_str(), plain[2].c_str());
    return 0;
}
