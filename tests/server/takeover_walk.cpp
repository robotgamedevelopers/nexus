// takeover_walk: end-to-end trace walk of all five takeover layers operating together on real POSIX files and OS locks
// (`migrateOldLayout` + `Builds` + `Handover` + `SupervisorLoop` + `Store`), checked against
// `models/takeover/TakeoverTrace.tla`.
//
// Usage:
//   takeover_walk <seed> <steps>
//     [--fault <ignore-live-old|skip-migration|serve-untested|no-handover-lock|blind-rollback|drain-cuts-turn|
//               stop-restarts|drain-stops|version-only-rollback|spin-on-format-refusal|blame-build-for-live-old>]
//     <out.tla>

#include "server/builds.h"
#include "server/drain.h"
#include "server/handover.h"
#include "server/migration.h"
#include "server/supervise_state.h"
#include "server/supervision.h"
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
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
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
    IgnoreLiveOld,
    SkipMigration,
    ServeUntested,
    NoHandoverLock,
    BlindRollback,
    DrainCutsTurn,
    StopRestarts,
    DrainStops,
    VersionOnlyRollback,
    SpinOnFormatRefusal,
    BlameBuildForLiveOld,
};

std::uint64_t gRng = 0;
std::uint64_t nextRng() {
    std::uint64_t z = (gRng += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}
std::uint64_t below(std::uint64_t n) {
    return n == 0 ? 0 : nextRng() % n;
}

fs::path gWorkRoot;

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
    if (root.empty() || fs::symlink_status(root, ec).type() == fs::file_type::not_found) {
        return;
    }
    makeWritableTree(root);
    fs::remove_all(root, ec);
}

[[noreturn]] void die(const std::string& why) {
    std::fprintf(stderr, "takeover_walk: %s\n", why.c_str());
    std::fflush(stderr);
    removeAllSafe(gWorkRoot);
    std::_Exit(1);
}

void writeHostFile(const fs::path& p, const std::string& content, bool exec) {
    fs::create_directories(p.parent_path());
    std::ofstream out(p, std::ios::binary);
    out << content;
    out.close();
    if (!out) {
        die("cannot write " + p.string());
    }
    fs::permissions(p, exec ? fs::perms(0755) : fs::perms(0644));
}

std::string buildFormatsText(int id) {
    // Build 1: 3/3 (base)
    // Build 2: 5/3 (appended fields & unknown change kinds; rollback-compatible with 1)
    // Build 3: 5/5 (breaking reader version 5 once written)
    const std::uint32_t writes = (id == 1) ? 3 : 5;
    const std::uint32_t reader = (id == 3) ? 5 : 3;
    const std::uint32_t snapWrites = (id == 1) ? 4 : 5;
    const std::uint32_t snapReader = (id == 3) ? 5 : 4;
    return builds::formatsText({
        {"store-record", writes, reader},
        {"store-snapshot", snapWrites, snapReader},
        {"config", 1, 1},
        {"link", 1, 1},
        {"driver-protocol", 1, 1},
    });
}

std::string stageBuildSource(const fs::path& stagingRoot, int id, bool withEvidence, std::string* evidencePathOut) {
    const fs::path tree = stagingRoot / ("src-" + std::to_string(id) + (withEvidence ? "-t" : "-u"));
    removeAllSafe(tree);
    writeHostFile(tree / "bin/nexus-server", "#!/bin/sh\n# build " + std::to_string(id) + "\nexit 0\n", true);
    writeHostFile(tree / "formats", buildFormatsText(id), false);
    writeHostFile(tree / "drivers/id.txt", "build-" + std::to_string(id) + "\n", false);

    std::string hash;
    const store::Status hs = builds::hashTree(tree.string(), &hash);
    if (!hs.is_ok()) {
        die("hashTree failed: " + hs.message());
    }
    if (withEvidence) {
        const fs::path ev = stagingRoot / ("ev-" + std::to_string(id) + ".txt");
        writeHostFile(ev, builds::evidenceText({hash, "pass", "takeover_walk build " + std::to_string(id)}), false);
        *evidencePathOut = ev.string();
    } else {
        evidencePathOut->clear();
    }
    return tree.string();
}

// Appends a framed format-5 record to `<storeDir>` using the real `Journal` (when `Store` is temporarily closed and
// immediately reopened under `<storeDir>`'s lock).
void writeFormat5Record(store::Fs& fs, const std::string& storeDir, int buildId, std::uint64_t sessionId) {
    const std::uint8_t readerByte = (buildId == 3) ? 5 : 3;
    store::Writer rec;
    rec.u8(5);            // format 5
    rec.u8(readerByte);   // min_reader: 3 for Build 2 (5/3), 5 for Build 3 (5/5)
    rec.u64(sessionId);
    rec.u32(2);           // 2 changes: 1 standard Append + 1 future unknown change kind 99

    {
        store::Writer entryInner;
        entryInner.u8(1);
        entryInner.u8(1); // closed = true
        entryInner.str("format5-entry-from-build-" + std::to_string(buildId));
        entryInner.str("future-entry-tail-field");
        store::Writer changeFrame;
        changeFrame.bytes(entryInner.out());
        rec.u8(static_cast<std::uint8_t>(store::Change::Kind::Append));
        rec.bytes(changeFrame.out());
    }
    {
        store::Writer unknownFrame;
        unknownFrame.str("future-unknown-change");
        rec.u8(99);
        rec.bytes(unknownFrame.out());
    }
    rec.str("");          // client
    rec.u64(0);           // counter
    rec.str("future-record-tail");

    std::unique_ptr<store::FileLock> lock;
    const store::Status ls = fs.lock(storeDir, store::LockMode::Exclusive, lock);
    if (!ls.is_ok()) {
        die("cannot lock store for format-5 write: " + ls.message());
    }
    store::Journal j(fs, storeDir);
    store::Journal::Recovery recov;
    const store::Status os = j.open(recov);
    if (!os.is_ok()) {
        die("cannot open journal for format-5 write: " + os.message());
    }
    if (!j.append(rec.out()).is_ok() || !j.sync().is_ok()) {
        die("cannot append/sync format-5 record");
    }
}

struct LiveServer {
    std::string state = "down"; // "down", "serving", "draining", "exited"
    int b = 0;
    bool turn = false;
    int code = 0;
    std::unique_ptr<store::Fs> storeFs;
    std::unique_ptr<store::Store> openStore;
    std::uint32_t cachedMaxReader = 0;
    std::size_t cachedSessions = 0;

    void closeStore() {
        openStore.reset();
        storeFs.reset();
    }

    bool startBuild(int id, const std::string& serverDir, const std::string& topDir, bool oldLive,
                    const std::set<int>& startupBroken, WalkFault fault, std::string* why) {
        closeStore();
        const bool hasOldFiles = oldLive || fs::exists(fs::path(topDir) / "store") ||
                                 fs::exists(fs::path(topDir) / "drivers") || fs::exists(fs::path(topDir) / "config");
        if (hasOldFiles && fault != WalkFault::SkipMigration) {
            if (why) {
                *why = "unmigrated old layout remains in " + topDir;
            }
            return false;
        }
        if (startupBroken.count(id) > 0) {
            if (why) {
                *why = "build " + std::to_string(id) + " failed startup check";
            }
            return false;
        }
        storeFs = store::make_posix_fs();
        const std::string storeDir = serverDir + "/store";
        if (!storeFs->private_dir(serverDir).is_ok() || !storeFs->private_dir(storeDir).is_ok()) {
            if (why) {
                *why = "cannot create private store dir";
            }
            closeStore();
            return false;
        }
        store::StoreOptions sopts;
        sopts.max_record_reader = (id == 1) ? 3 : 5;
        sopts.max_snapshot_reader = (id == 1) ? 4 : 5;
        const store::Status st = store::Store::open(*storeFs, storeDir, sopts, openStore);
        if (!st.is_ok()) {
            if (why) {
                *why = st.message();
            }
            closeStore();
            return false;
        }
        if (cachedMaxReader == 0) {
            cachedMaxReader = 3;
        }
        cachedSessions = openStore->sessions().size();
        state = "serving";
        b = id;
        turn = false;
        code = 0;
        return true;
    }

    void commitSessionOrEntry(int buildId, const std::set<int>& format5Writers, const std::string& serverDir) {
        if (!openStore) {
            die("commitSessionOrEntry without openStore");
        }
        std::uint64_t sid = openStore->next_session();
        store::Change mk;
        mk.kind = store::Change::Kind::SetInfo;
        mk.info.friend_name = "friend-" + std::to_string(sid);
        mk.info.harness = "claude";
        store::Change app;
        app.kind = store::Change::Kind::Append;
        app.entry = store::Entry{1, "turn-on-build-" + std::to_string(buildId), true};
        const store::Change batch[] = {mk, app};
        const store::Status cs = openStore->commit(sid, batch);
        if (!cs.is_ok()) {
            die("Store::commit failed: " + cs.message());
        }
        if (format5Writers.count(buildId) > 0) {
            openStore.reset();
            writeFormat5Record(*storeFs, serverDir + "/store", buildId, sid);
            store::StoreOptions sopts;
            sopts.max_record_reader = (buildId == 1) ? 3 : 5;
            sopts.max_snapshot_reader = (buildId == 1) ? 4 : 5;
            const store::Status rs = store::Store::open(*storeFs, serverDir + "/store", sopts, openStore);
            if (!rs.is_ok()) {
                die("Store reopen after format-5 write failed: " + rs.message());
            }
            if (buildId == 3 && cachedMaxReader < 5) {
                cachedMaxReader = 5;
            }
        }
        if (cachedMaxReader == 0) {
            cachedMaxReader = 3;
        }
        cachedSessions = openStore->sessions().size();
    }
};

// Wraps real `Builds` for `Handover` and `SupervisorLoop`, injecting only `version-only-rollback` or `blind-rollback`
// when explicitly requested via `--fault`.
class WalkPlacement final : public handover::PlacementPort {
public:
    std::string serverDir;
    WalkFault fault;

    WalkPlacement(std::string dir, WalkFault f) : serverDir(std::move(dir)), fault(f) {}

    builds::Options makeOpts() const {
        builds::Options o;
        if (fault == WalkFault::BlindRollback) {
            o.fault = builds::Fault::BlindBack;
        }
        return o;
    }

    int active(std::string* current, std::string* previous, builds::Result& r) override {
        builds::Builds b(serverDir, makeOpts());
        return b.active(current, previous, r);
    }

    int checkSwitch(const std::string& hash, builds::Result& r, bool allowUntested) override {
        builds::Builds b(serverDir, makeOpts());
        return b.checkSwitch(hash, r, allowUntested);
    }

    int makeCurrent(const std::string& hash, builds::Result& r, bool allowUntested) override {
        builds::Builds b(serverDir, makeOpts());
        return b.makeCurrent(hash, r, allowUntested);
    }

    int rollback(const std::string& hash, builds::Result& r) override {
        builds::Builds b(serverDir, makeOpts());
        if (fault == WalkFault::VersionOnlyRollback) {
            return b.makeCurrent(hash, r, false);
        }
        return b.rollbackTo(hash, r);
    }
};

class WalkServerPort final : public handover::ServerPort {
public:
    LiveServer& srv;
    std::string serverDir;
    std::string topDir;
    bool& oldLive;
    bool& supLocked;
    std::string& supPcRef;
    const std::map<std::string, int>& idByHash;
    const std::map<int, std::string>& hashById;
    const std::set<int>& startupBroken;
    WalkFault fault;

    WalkServerPort(LiveServer& s, std::string sdir, std::string tdir, bool& ol, bool& sl, std::string& spc,
                   const std::map<std::string, int>& toId, const std::map<int, std::string>& toHash,
                   const std::set<int>& sb, WalkFault f)
        : srv(s),
          serverDir(std::move(sdir)),
          topDir(std::move(tdir)),
          oldLive(ol),
          supLocked(sl),
          supPcRef(spc),
          idByHash(toId),
          hashById(toHash),
          startupBroken(sb),
          fault(f) {}

    store::Status probe(ServerSeen* out) override {
        *out = ServerSeen{};
        if (srv.state == "serving" || srv.state == "draining") {
            out->serving = true;
            out->draining = (srv.state == "draining");
            const auto it = hashById.find(srv.b);
            out->build = (it == hashById.end()) ? std::string() : it->second;
        }
        return store::Status::ok();
    }

    handover::DrainReply requestDrain(std::string*) override {
        if (srv.state == "serving") {
            if (!srv.turn) {
                srv.closeStore();
                if (supLocked && supPcRef == "watch") {
                    srv.state = "exited";
                    srv.code = kDrainExit;
                } else {
                    srv.state = "down";
                    srv.b = 0;
                    srv.code = 0;
                }
            } else {
                srv.state = "draining";
            }
        }
        return handover::DrainReply::Accepted;
    }

    store::Status stop() override {
        if (srv.state == "serving" || srv.state == "draining") {
            srv.closeStore();
            srv.turn = false;
            if (supLocked && supPcRef == "watch") {
                srv.state = "exited";
                srv.code = kDrainExit;
            } else {
                srv.state = "down";
                srv.b = 0;
                srv.code = 0;
            }
        }
        return store::Status::ok();
    }

    handover::StartReply stepStart(const std::string& expectedBuild, std::uint64_t, std::uint64_t,
                                   std::string* why) override {
        const auto it = idByHash.find(expectedBuild);
        const int id = (it == idByHash.end()) ? 0 : it->second;
        if (!srv.startBuild(id, serverDir, topDir, oldLive, startupBroken, fault, why)) {
            return handover::StartReply::Failed;
        }
        return handover::StartReply::Waiting;
    }

    void keepStarted() override {}
    void abortStarted() override {}
};

class WalkHandoverGate final : public supervision::HandoverGate {
public:
    store::Fs& fs;
    std::string serverDir;
    bool& hoLocked;

    WalkHandoverGate(store::Fs& f, std::string dir, bool& hl) : fs(f), serverDir(std::move(dir)), hoLocked(hl) {}

    bool isLockedByOther() override {
        if (hoLocked) {
            return true;
        }
        std::uint64_t sz = 0;
        return fs.size(serverDir + "/handover/state", sz).is_ok();
    }

    store::Status tryLock(std::unique_ptr<store::FileLock>& out) override {
        const std::string hdir = serverDir + "/handover";
        const store::Status ds = fs.private_dir(hdir);
        if (!ds.is_ok()) {
            return ds;
        }
        return fs.lock(hdir, store::LockMode::Exclusive, out);
    }

    store::Status resumeIfNeeded(bool) override { return store::Status::ok(); }

    bool hasExplicitPick(const std::string& build) override {
        return supervision::hasPickedBuild(fs, serverDir + "/supervise", build);
    }

    void clearExplicitPick(const std::string& build) override {
        (void)supervision::clearPickedBuild(fs, serverDir + "/supervise", build);
    }
};

class WalkChildPort final : public supervision::ChildPort {
public:
    LiveServer& srv;
    std::string serverDir;
    std::string topDir;
    bool& oldLive;
    const std::map<std::string, int>& idByHash;
    const std::map<int, std::string>& hashById;
    const std::set<int>& startupBroken;
    WalkFault fault;

    WalkChildPort(LiveServer& s, std::string sdir, std::string tdir, bool& ol,
                  const std::map<std::string, int>& toId, const std::map<int, std::string>& toHash,
                  const std::set<int>& sb, WalkFault f)
        : srv(s),
          serverDir(std::move(sdir)),
          topDir(std::move(tdir)),
          oldLive(ol),
          idByHash(toId),
          hashById(toHash),
          startupBroken(sb),
          fault(f) {}

    store::Status inspect(supervision::ChildObs* out) override {
        *out = supervision::ChildObs{};
        if (srv.state == "serving" || srv.state == "draining") {
            out->state = supervision::ChildState::Serving;
            const auto it = hashById.find(srv.b);
            out->build = (it == hashById.end()) ? std::string() : it->second;
        } else if (srv.state == "exited") {
            out->state = supervision::ChildState::Exited;
            const auto it = hashById.find(srv.b);
            out->build = (it == hashById.end()) ? std::string() : it->second;
            out->exitCode = srv.code;
        } else {
            out->state = supervision::ChildState::Down;
        }
        return store::Status::ok();
    }

    store::Status spawn(const std::string& expectedBuild, std::uint64_t, supervision::ChildObs* out) override {
        const auto it = idByHash.find(expectedBuild);
        const int id = (it == idByHash.end()) ? 0 : it->second;
        std::string why;
        if (!srv.startBuild(id, serverDir, topDir, oldLive, startupBroken, fault, &why)) {
            srv.state = "exited";
            srv.b = id;
            srv.turn = false;
            srv.code = 1;
        }
        return inspect(out);
    }

    store::Status drainToExit() override {
        if (srv.state == "serving") {
            if (!srv.turn) {
                srv.closeStore();
                srv.state = "exited";
                srv.code = kDrainExit;
            } else {
                srv.state = "draining";
            }
        }
        return store::Status::ok();
    }

    store::Status stopToExit() override {
        srv.closeStore();
        srv.turn = false;
        srv.state = "exited";
        srv.code = 0;
        return store::Status::ok();
    }

    int consumeExit() override {
        const int c = srv.code;
        srv.closeStore();
        srv.state = "down";
        srv.b = 0;
        srv.turn = false;
        srv.code = 0;
        return c;
    }
};

struct TraceRow {
    std::string op;
    int b = 0;
    int arg = 0;
    std::string oldLayout;
    bool migrated = false;
    std::set<int> placed;
    std::set<int> tested;
    int cur = 0;
    int prev = 0;
    std::string hlock;
    std::string hstage;
    int htarget = 0;
    int hprev = 0;
    bool supLock = false;
    std::string supPc;
    int sbuild = 0;
    std::uint32_t scrashes = 0;
    std::set<int> sshunned;
    bool shalted = false;
    bool sstopped = false;
    int explicitPick = 0;
    std::string srvState;
    int srvB = 0;
    bool srvTurn = false;
    int srvCode = 0;
    std::uint32_t maxReader = 0;
    std::size_t sessions = 0;
};

std::string tlaIntSet(const std::set<int>& s) {
    std::ostringstream os;
    os << "{";
    bool first = true;
    for (int x : s) {
        if (!first) {
            os << ", ";
        }
        first = false;
        os << x;
    }
    os << "}";
    return os.str();
}

std::string tlaBool(bool v) { return v ? "TRUE" : "FALSE"; }

struct ActorDriver {
    std::mutex mu;
    std::condition_variable cv;
    bool running = false;
    bool paused = false;
    bool killNow = false;
    std::string atStep;
    std::set<std::string> stopSteps;
    std::thread worker;

    void waitUntilPausedOrDone() {
        std::unique_lock<std::mutex> lk(mu);
        cv.wait(lk, [&] { return paused || !running; });
    }

    void stepTo(const std::set<std::string>& stops) {
        {
            std::lock_guard<std::mutex> lk(mu);
            stopSteps = stops;
            paused = false;
        }
        cv.notify_all();
        waitUntilPausedOrDone();
    }

    void kill() {
        {
            std::lock_guard<std::mutex> lk(mu);
            killNow = true;
            paused = false;
        }
        cv.notify_all();
        if (worker.joinable()) {
            worker.join();
        }
    }

    void hook(const std::string& step) {
        std::unique_lock<std::mutex> lk(mu);
        atStep = step;
        if (killNow) {
            throw Killed{};
        }
        if (stopSteps.count(step) > 0) {
            paused = true;
            cv.notify_all();
            cv.wait(lk, [&] { return !paused || killNow; });
            if (killNow) {
                throw Killed{};
            }
        }
    }
};

} // namespace

int main(int argc, char** argv) {
    if (argc != 4 && argc != 6) {
        die("usage: takeover_walk <seed> <steps> [--fault <name>] <out.tla>");
    }
    const std::uint64_t seed = std::strtoull(argv[1], nullptr, 10);
    const std::size_t maxSteps = std::strtoull(argv[2], nullptr, 10);
    WalkFault fault = WalkFault::None;
    std::string faultName = "none";
    std::string outPath;
    if (argc == 6) {
        if (std::strcmp(argv[3], "--fault") != 0) {
            die("expected --fault");
        }
        faultName = argv[4];
        if (faultName == "ignore-live-old") {
            fault = WalkFault::IgnoreLiveOld;
        } else if (faultName == "skip-migration") {
            fault = WalkFault::SkipMigration;
        } else if (faultName == "serve-untested") {
            fault = WalkFault::ServeUntested;
        } else if (faultName == "no-handover-lock") {
            fault = WalkFault::NoHandoverLock;
        } else if (faultName == "blind-rollback") {
            fault = WalkFault::BlindRollback;
        } else if (faultName == "drain-cuts-turn") {
            fault = WalkFault::DrainCutsTurn;
        } else if (faultName == "stop-restarts") {
            fault = WalkFault::StopRestarts;
        } else if (faultName == "drain-stops") {
            fault = WalkFault::DrainStops;
        } else if (faultName == "version-only-rollback") {
            fault = WalkFault::VersionOnlyRollback;
        } else if (faultName == "spin-on-format-refusal") {
            fault = WalkFault::SpinOnFormatRefusal;
        } else if (faultName == "blame-build-for-live-old") {
            fault = WalkFault::BlameBuildForLiveOld;
        } else {
            die("unknown fault: " + faultName);
        }
        outPath = argv[5];
    } else {
        outPath = argv[3];
    }

    gRng = seed * 0xD1B54A32D192ED03ULL + 1;

    const fs::path outFile(outPath);
    if (!outFile.parent_path().empty()) {
        fs::create_directories(outFile.parent_path());
    }
    const fs::path workRoot = (outFile.parent_path().empty() ? fs::path(".") : outFile.parent_path()) /
                              (outFile.filename().string() + ".walk-files");
    removeAllSafe(workRoot);
    fs::create_directories(workRoot);
    gWorkRoot = workRoot;

    struct Cleanup {
        fs::path root;
        ~Cleanup() {
            removeAllSafe(root);
            gWorkRoot.clear();
        }
    } cleanup{workRoot};

    const fs::path staging = workRoot / "staging";
    const fs::path homeDir = workRoot / "home";
    const fs::path topDir = homeDir / ".nexus";
    const fs::path serverDir = topDir / "server";
    fs::create_directories(staging);
    fs::create_directories(topDir);
    fs::permissions(topDir, fs::perms(0700));

    // Pre-compute deterministic hashes for builds 1..3 (tested & untested).
    std::map<int, std::string> testedTree, untestedTree, evidenceFile, hashById, untestedHashById;
    std::map<std::string, int> idByHash;
    for (int id = 1; id <= 3; ++id) {
        std::string ev;
        testedTree[id] = stageBuildSource(staging, id, true, &ev);
        evidenceFile[id] = ev;
        std::string h;
        if (!builds::hashTree(testedTree[id], &h).is_ok()) {
            die("hashTree tested failed");
        }
        hashById[id] = h;
        idByHash[h] = id;

        std::string noEv;
        untestedTree[id] = stageBuildSource(staging, id, false, &noEv);
        std::string uh;
        if (!builds::hashTree(untestedTree[id], &uh).is_ok()) {
            die("hashTree untested failed");
        }
        untestedHashById[id] = uh;
        idByHash[uh] = id;
    }

    // Configure scenario constants by seed / fault.
    std::string initOldLayout = "none";
    std::set<int> initPlaced = {1};
    std::set<int> initTested = {1};
    int initCur = 1;
    std::set<int> startupBroken;
    std::set<int> lateBroken;
    std::set<int> format5Writers;
    const std::uint32_t maxCrashesBeforeRollback = 2;
    const std::uint32_t maxRestarts = 3;

    if (fault != WalkFault::None) {
        switch (fault) {
        case WalkFault::IgnoreLiveOld:
        case WalkFault::BlameBuildForLiveOld:
            initOldLayout = "live";
            initPlaced = {1};
            initTested = {1};
            initCur = 1;
            break;
        case WalkFault::SkipMigration:
            initOldLayout = "cold";
            initPlaced = {1};
            initTested = {1};
            initCur = 1;
            break;
        case WalkFault::ServeUntested:
            initOldLayout = "none";
            initPlaced = {1};
            initTested = {1};
            initCur = 1;
            break;
        case WalkFault::NoHandoverLock:
            initOldLayout = "none";
            initPlaced = {1, 2};
            initTested = {1, 2};
            initCur = 1;
            lateBroken = {2};
            break;
        case WalkFault::BlindRollback:
        case WalkFault::SpinOnFormatRefusal:
            initOldLayout = "none";
            initPlaced = {1, 3};
            initTested = {1, 3};
            initCur = 1;
            lateBroken = {3};
            format5Writers = {3};
            break;
        case WalkFault::VersionOnlyRollback:
            initOldLayout = "none";
            initPlaced = {1, 3};
            initTested = {1, 3};
            initCur = 1;
            lateBroken = {3};
            format5Writers = {};
            break;
        case WalkFault::DrainCutsTurn:
        case WalkFault::DrainStops:
            initOldLayout = "none";
            initPlaced = {1, 2};
            initTested = {1, 2};
            initCur = 1;
            break;
        case WalkFault::StopRestarts:
            initOldLayout = "none";
            initPlaced = {1};
            initTested = {1};
            initCur = 1;
            break;
        case WalkFault::None:
            break;
        }
    } else {
        // 4 rich cross-layer seeds:
        //   seed 1: live old layout -> stop -> migrate -> place 2 (5/3) & 3 (startup-broken) -> supervise -> handover
        //           with open turn -> format-5 compat write -> handover to 3 (startup rollback) -> user stop -> resume
        //   seed 2: cold old layout -> supervise auto-migrates -> handover to 2 (late-broken) -> cut mid-handover ->
        //           resume -> post-start crashes -> rollback to 1 -> supervisor death & recovery
        //   seed 3: 1 (3/3) -> handover to 3 (5/5, late-broken, BEFORE writing format 5) -> post-start crashes roll
        //           back cleanly to 1 via Builds::rollbackTo -> handover to 2 (5/3) -> format-5 compat write ->
        //           downgrade handover back to 1
        //   seed 4: 1 (3/3) -> handover to 3 (5/5, late-broken) -> writes breaking reader = 5 -> post-start crashes ->
        //           Builds::rollbackTo refuses 1 -> supervisor shuns 1 without spinning and halts cleanly (exit 78)
        const std::uint64_t kind = (seed - 1) % 4;
        if (kind == 0) {
            initOldLayout = "live";
            initPlaced = {1};
            initTested = {1};
            initCur = 0;
            startupBroken = {3};
            format5Writers = {2};
        } else if (kind == 1) {
            initOldLayout = "cold";
            initPlaced = {1, 2};
            initTested = {1, 2};
            initCur = 1;
            lateBroken = {2};
            format5Writers = {2};
        } else if (kind == 2) {
            initOldLayout = "none";
            initPlaced = {1, 2, 3};
            initTested = {1, 2, 3};
            initCur = 1;
            lateBroken = {3};
            format5Writers = {2};
        } else {
            initOldLayout = "none";
            initPlaced = {1, 3};
            initTested = {1, 3};
            initCur = 1;
            lateBroken = {3};
            format5Writers = {3};
        }
    }

    std::unique_ptr<store::Fs> obsFs = store::make_posix_fs();
    bool oldLive = (initOldLayout == "live");
    bool migrated = (initOldLayout == "none");

    // Populate old ~/.nexus layout if requested (before touching serverDir/store!).
    if (initOldLayout != "none") {
        std::unique_ptr<store::Store> oldSt;
        if (!store::Store::open(*obsFs, (topDir / "store").string(), {}, oldSt).is_ok()) {
            die("cannot create initial old store");
        }
        store::Change mk;
        mk.kind = store::Change::Kind::SetInfo;
        mk.info.friend_name = "legacy-friend";
        mk.info.harness = "claude";
        store::Change app;
        app.kind = store::Change::Kind::Append;
        app.entry = store::Entry{1, "migrated-entry", true};
        const store::Change batch[] = {mk, app};
        if (!oldSt->commit(oldSt->next_session(), batch).is_ok()) {
            die("cannot seed initial old store");
        }
        oldSt.reset();
        const std::string drv = "{\"drivers\":[]}\n";
        if (!obsFs->write_new((topDir / "drivers").string(), store::Bytes(drv.begin(), drv.end())).is_ok()) {
            die("cannot seed old drivers");
        }
    }

    fs::create_directories(serverDir);
    fs::permissions(serverDir, fs::perms(0700));
    fs::create_directories(serverDir / "supervise");
    fs::permissions(serverDir / "supervise", fs::perms(0700));

    // Place initial builds in `<serverDir>/builds`.
    {
        builds::Builds b(serverDir.string(), {});
        for (int id : initPlaced) {
            builds::Result r;
            const bool isT = initTested.count(id) > 0;
            if (b.place(isT ? testedTree[id] : untestedTree[id], isT ? evidenceFile[id] : "", r) != builds::kExitOk) {
                die("initial place failed");
            }
        }
        if (initCur != 0) {
            builds::Result r;
            if (b.makeCurrent(hashById[initCur], r) != builds::kExitOk) {
                die("initial makeCurrent failed");
            }
        }
    }

    LiveServer liveSrv;
    bool supLocked = false;
    bool hoLocked = false;
    std::string supPc = "idle";

    ActorDriver supActor;
    std::unique_ptr<store::Fs> supFs;
    std::unique_ptr<WalkPlacement> supPlacement;
    std::unique_ptr<WalkHandoverGate> supGate;
    std::unique_ptr<WalkChildPort> supChild;
    std::unique_ptr<supervision::SupervisorLoop> supLoop;

    ActorDriver hoActor;
    std::unique_ptr<store::Fs> hoFs;
    std::unique_ptr<WalkPlacement> hoPlacement;
    std::unique_ptr<WalkServerPort> hoServer;
    std::unique_ptr<handover::Handover> hoLoop;

    auto readBuildId = [&](const std::string& h) -> int {
        if (h.empty()) {
            return 0;
        }
        const auto it = idByHash.find(h);
        return (it == idByHash.end()) ? 0 : it->second;
    };

    bool diskStoreDirty = true;
    std::vector<TraceRow> trace;
    auto snapshot = [&](const std::string& op, int b = 0, int arg = 0) {
        TraceRow row;
        row.op = op;
        row.b = b;
        row.arg = arg;

        const bool hasOldFiles = fs::exists(topDir / "store") || fs::exists(topDir / "drivers") ||
                                 fs::exists(topDir / "config");
        row.oldLayout = oldLive ? "live" : (hasOldFiles ? "cold" : "none");
        row.migrated = migrated;

        // Read placed & tested & active directly from `<serverDir>/builds` on disk without observer fsyncs.
        for (int id = 1; id <= 3; ++id) {
            builds::Manifest m;
            if (builds::checkBuild((serverDir / "builds").string(), hashById[id], &m, builds::Fault::NoSync).is_ok()) {
                row.placed.insert(id);
                if (m.tested) {
                    row.tested.insert(id);
                }
            }
            builds::Manifest um;
            if (builds::checkBuild((serverDir / "builds").string(), untestedHashById[id], &um, builds::Fault::NoSync)
                    .is_ok()) {
                row.placed.insert(id);
            }
        }

        {
            builds::Builds bld(serverDir.string(), {});
            std::string ch, ph;
            builds::Result br;
            if (bld.active(&ch, &ph, br) == builds::kExitOk) {
                row.cur = readBuildId(ch);
                row.prev = readBuildId(ph);
            }
        }

        supLocked = supActor.running && supLoop && supLoop->holdsLock();
        hoLocked = hoActor.running && hoLoop && hoLoop->holdsLock();
        const bool supHoldsHo = supActor.running && supLoop && supLoop->holdsHandoverLock();
        row.hlock = hoLocked ? "ho" : (supHoldsHo ? "sup" : "none");

        {
            store::Bytes raw;
            if (obsFs->read_all((serverDir / "handover/state").string(), raw).is_ok()) {
                handover::StateRecord rec;
                std::string why;
                if (handover::readState(std::string(raw.begin(), raw.end()), &rec, &why)) {
                    row.hstage = handover::stageName(rec.stage);
                    row.htarget = readBuildId(rec.target);
                    row.hprev = readBuildId(rec.previous);
                } else {
                    row.hstage = "none";
                }
            } else {
                row.hstage = "none";
            }
        }

        row.supLock = supLocked;
        row.supPc = supLocked ? supPc : "idle";

        {
            supervision::SuperviseState st;
            if (supervision::loadEffectiveState(*obsFs, (serverDir / "supervise").string(), &st).is_ok()) {
                row.sbuild = readBuildId(st.build);
                row.scrashes = st.crashes;
                for (const std::string& sh : st.shunned) {
                    const int sid = readBuildId(sh);
                    if (sid != 0) {
                        row.sshunned.insert(sid);
                    }
                }
                row.shalted = st.halted;
                row.sstopped = st.stopped;
                row.explicitPick = readBuildId(st.picked);
            }
        }

        row.srvState = liveSrv.state;
        row.srvB = liveSrv.b;
        row.srvTurn = liveSrv.turn;
        row.srvCode = liveSrv.code;

        if (liveSrv.openStore) {
            row.maxReader = liveSrv.cachedMaxReader;
            row.sessions = liveSrv.cachedSessions;
            diskStoreDirty = false;
        } else if (diskStoreDirty && fs::exists(serverDir / "store")) {
            store::StoreReaders sr;
            if (store::Store::inspect_readers(*obsFs, (serverDir / "store").string(), sr).is_ok() && sr.exists) {
                std::uint32_t r = sr.record_reader;
                if (sr.snapshot_reader >= 5 && r < 5) {
                    r = 5;
                } else if (sr.snapshot_reader >= 4 && r < 3) {
                    r = 3;
                }
                row.maxReader = r;
                liveSrv.cachedMaxReader = r;

                std::unique_ptr<store::Store> tmpSt;
                store::StoreOptions sopts;
                sopts.max_record_reader = 5;
                sopts.max_snapshot_reader = 5;
                if (store::Store::open(*obsFs, (serverDir / "store").string(), sopts, tmpSt).is_ok()) {
                    row.sessions = tmpSt->sessions().size();
                    liveSrv.cachedSessions = row.sessions;
                } else {
                    row.sessions = liveSrv.cachedSessions;
                }
            } else {
                row.maxReader = liveSrv.cachedMaxReader;
                row.sessions = liveSrv.cachedSessions;
            }
            diskStoreDirty = false;
        } else {
            row.maxReader = liveSrv.cachedMaxReader;
            row.sessions = liveSrv.cachedSessions;
        }

        trace.push_back(std::move(row));
    };

    snapshot("init");

    auto joinHoIfDone = [&]() {
        if (!hoActor.running && hoActor.worker.joinable()) {
            hoActor.worker.join();
            hoLoop.reset();
            hoServer.reset();
            hoPlacement.reset();
            hoFs.reset();
        }
    };
    auto joinSupIfDone = [&]() {
        if (!supActor.running && supActor.worker.joinable()) {
            supActor.worker.join();
            supLoop.reset();
            supChild.reset();
            supGate.reset();
            supPlacement.reset();
            supFs.reset();
            supPc = "idle";
        }
    };

    auto doOldStop = [&]() {
        oldLive = false;
        snapshot("old_stop");
    };

    auto doMigrate = [&]() {
        migration::Options mopts;
        if (fault == WalkFault::IgnoreLiveOld) {
            mopts.old_server_live = [] { return false; };
        } else {
            mopts.old_server_live = [&] { return oldLive; };
        }
        const store::Status ms = migration::migrateOldLayout(*obsFs, topDir.string(), serverDir.string(), mopts);
        if (ms.is_ok()) {
            migrated = true;
            diskStoreDirty = true;
        }
        snapshot("migrate");
    };

    auto doPlace = [&](int id, bool isTested) {
        builds::Builds bld(serverDir.string(), {});
        builds::Result br;
        if (bld.place(isTested ? testedTree[id] : untestedTree[id], isTested ? evidenceFile[id] : "", br) !=
            builds::kExitOk) {
            die("place failed");
        }
        snapshot("place", id, isTested ? 1 : 0);
    };

    auto doSetInitialCurrent = [&](int id) {
        builds::Builds bld(serverDir.string(), {});
        builds::Result br;
        std::string h = hashById[id];
        const bool allowU = (fault == WalkFault::ServeUntested);
        if (allowU && trace.back().tested.count(id) == 0) {
            if (!builds::hashTree(untestedTree[id], &h).is_ok()) {
                die("untested hashTree failed");
            }
        }
        if (bld.makeCurrent(h, br, allowU) != builds::kExitOk) {
            die("setInitialCurrent failed");
        }
        snapshot("init_cur", id);
    };

    auto startSupervisor = [&]() -> bool {
        supActor.kill();
        supLoop.reset();
        supChild.reset();
        supGate.reset();
        supPlacement.reset();
        supFs.reset();
        supPc = "idle";
        if (oldLive) {
            if (fault == WalkFault::BlameBuildForLiveOld) {
                supervision::SuperviseState st;
                (void)supervision::loadEffectiveState(*obsFs, (serverDir / "supervise").string(), &st);
                st.build = hashById[trace.back().cur];
                st.crashes = maxRestarts;
                st.halted = true;
                (void)obsFs->private_dir((serverDir / "supervise").string());
                (void)supervision::writeStateFile(*obsFs, (serverDir / "supervise").string(), st);
                snapshot("sup_start");
                return true;
            }
            if (fault == WalkFault::IgnoreLiveOld) {
                migration::Options mopts;
                mopts.old_server_live = [] { return false; };
                if (migration::migrateOldLayout(*obsFs, topDir.string(), serverDir.string(), mopts).is_ok()) {
                    migrated = true;
                    diskStoreDirty = true;
                }
            } else {
                // Clean refusal before touching supervise/state.
                snapshot("sup_start");
                return false;
            }
        } else if (trace.back().oldLayout == "cold" && fault != WalkFault::SkipMigration) {
            migration::Options mopts;
            mopts.old_server_live = [] { return false; };
            if (!migration::migrateOldLayout(*obsFs, topDir.string(), serverDir.string(), mopts).is_ok()) {
                die("supervise cold migration failed");
            }
            migrated = true;
            diskStoreDirty = true;
        }

        supFs = store::make_posix_fs();
        supPlacement = std::make_unique<WalkPlacement>(serverDir.string(), fault);
        supGate = std::make_unique<WalkHandoverGate>(*supFs, serverDir.string(), hoLocked);
        supChild = std::make_unique<WalkChildPort>(liveSrv, serverDir.string(), topDir.string(), oldLive, idByHash,
                                                   hashById, startupBroken, fault);

        supervision::Options opts;
        opts.max_crashes_before_rollback = maxCrashesBeforeRollback;
        opts.max_restarts = maxRestarts;
        opts.now_ms = [] { return 1000ULL; };
        opts.sleep_ms = [](std::uint64_t) {};
        if (fault == WalkFault::NoHandoverLock) {
            opts.fault = supervision::Fault::NoHandoverLock;
        } else if (fault == WalkFault::StopRestarts) {
            opts.fault = supervision::Fault::StopRestarts;
        } else if (fault == WalkFault::DrainStops) {
            opts.fault = supervision::Fault::DrainStops;
        } else if (fault == WalkFault::ServeUntested) {
            opts.fault = supervision::Fault::ServeUntested;
        } else if (fault == WalkFault::SpinOnFormatRefusal) {
            opts.fault = supervision::Fault::SpinOnFormatRefusal;
        }
        opts.hook = [&](const std::string& step) { supActor.hook(step); };

        supLoop = std::make_unique<supervision::SupervisorLoop>(*supFs, *supPlacement, *supGate, *supChild,
                                                                (serverDir / "supervise").string(), opts);

        supActor.running = true;
        supActor.paused = false;
        supActor.killNow = false;
        supActor.stopSteps = {"check"};
        supActor.worker = std::thread([&]() {
            try {
                supervision::Result r;
                (void)supLoop->run(r);
            } catch (const Killed&) {
            }
            std::lock_guard<std::mutex> lk(supActor.mu);
            supActor.running = false;
            supActor.paused = false;
            supActor.cv.notify_all();
        });
        supActor.waitUntilPausedOrDone();
        supPc = (supActor.running && supActor.atStep == "check") ? "check" : "idle";
        joinSupIfDone();
        snapshot("sup_start");
        return supActor.running;
    };

    auto stepSupCheck = [&]() {
        supActor.stepTo({"watch", "reap", "rollback"});
        if (!supActor.running) {
            supPc = "idle";
            joinSupIfDone();
        } else {
            supPc = supActor.atStep;
        }
        snapshot("sup_check");
    };

    auto stepSupWatch = [&]() {
        supActor.stepTo({"watch", "reap", "check"});
        if (!supActor.running) {
            supPc = "idle";
            joinSupIfDone();
        } else {
            supPc = supActor.atStep;
        }
        snapshot("sup_watch");
    };

    auto stepSupReap = [&]() {
        supActor.stepTo({"check", "rollback"});
        if (!supActor.running) {
            supPc = "idle";
            joinSupIfDone();
        } else {
            supPc = supActor.atStep;
        }
        snapshot("sup_reap");
    };

    auto stepSupRollback = [&]() {
        supActor.stepTo({"check"});
        if (!supActor.running) {
            supPc = "idle";
            joinSupIfDone();
        } else {
            supPc = supActor.atStep;
        }
        snapshot("sup_rollback");
    };

    auto doSupDie = [&]() {
        supActor.kill();
        supLoop.reset();
        supChild.reset();
        supGate.reset();
        supPlacement.reset();
        supFs.reset();
        supPc = "idle";
        liveSrv.closeStore();
        liveSrv = LiveServer{
            "down", 0, false, 0, nullptr, nullptr, liveSrv.cachedMaxReader, liveSrv.cachedSessions};
        snapshot("sup_die");
    };

    auto startHandoverTo = [&](int targetId) {
        hoActor.kill();
        hoLoop.reset();
        hoServer.reset();
        hoPlacement.reset();
        hoFs.reset();
        hoFs = store::make_posix_fs();
        hoPlacement = std::make_unique<WalkPlacement>(serverDir.string(), fault);
        hoServer = std::make_unique<WalkServerPort>(liveSrv, serverDir.string(), topDir.string(), oldLive, supLocked,
                                                    supPc, idByHash, hashById, startupBroken, fault);

        handover::Options opts;
        opts.now_ms = [] { return 1000ULL; };
        opts.sleep_ms = [](std::uint64_t) {};
        if (fault == WalkFault::NoHandoverLock) {
            opts.fault = handover::Fault::NoLock;
        } else if (fault == WalkFault::ServeUntested) {
            opts.fault = handover::Fault::AllowUntested;
        } else if (fault == WalkFault::DrainCutsTurn) {
            opts.fault = handover::Fault::StopCuts;
        }
        opts.hook = [&](const std::string& step) { hoActor.hook(step); };

        hoLoop = std::make_unique<handover::Handover>(*hoFs, *hoPlacement, *hoServer,
                                                      (serverDir / "handover").string(), opts);

        std::string targetHash = hashById[targetId];
        if (fault == WalkFault::ServeUntested && trace.back().tested.count(targetId) == 0) {
            if (!builds::hashTree(untestedTree[targetId], &targetHash).is_ok()) {
                die("untested target hash failed");
            }
        }

        hoActor.running = true;
        hoActor.paused = false;
        hoActor.killNow = false;
        hoActor.stopSteps = {"drain"};
        hoActor.worker = std::thread([&, targetHash]() {
            try {
                handover::Result r;
                (void)hoLoop->to(targetHash, r);
            } catch (const Killed&) {
            }
            std::lock_guard<std::mutex> lk(hoActor.mu);
            hoActor.running = false;
            hoActor.paused = false;
            hoActor.cv.notify_all();
        });
        hoActor.waitUntilPausedOrDone();
        joinHoIfDone();
        snapshot("ho_begin", targetId);
    };

    auto startHandoverResume = [&]() {
        hoActor.kill();
        hoLoop.reset();
        hoServer.reset();
        hoPlacement.reset();
        hoFs.reset();
        hoFs = store::make_posix_fs();
        hoPlacement = std::make_unique<WalkPlacement>(serverDir.string(), fault);
        hoServer = std::make_unique<WalkServerPort>(liveSrv, serverDir.string(), topDir.string(), oldLive, supLocked,
                                                    supPc, idByHash, hashById, startupBroken, fault);

        handover::Options opts;
        opts.now_ms = [] { return 1000ULL; };
        opts.sleep_ms = [](std::uint64_t) {};
        if (fault == WalkFault::NoHandoverLock) {
            opts.fault = handover::Fault::NoLock;
        } else if (fault == WalkFault::DrainCutsTurn) {
            opts.fault = handover::Fault::StopCuts;
        }
        opts.hook = [&](const std::string& step) { hoActor.hook(step); };

        hoLoop = std::make_unique<handover::Handover>(*hoFs, *hoPlacement, *hoServer,
                                                      (serverDir / "handover").string(), opts);

        hoActor.running = true;
        hoActor.paused = false;
        hoActor.killNow = false;
        hoActor.stopSteps = {"inspect"};
        hoActor.worker = std::thread([&]() {
            try {
                handover::Result r;
                (void)hoLoop->resume(r);
            } catch (const Killed&) {
            }
            std::lock_guard<std::mutex> lk(hoActor.mu);
            hoActor.running = false;
            hoActor.paused = false;
            hoActor.cv.notify_all();
        });
        hoActor.waitUntilPausedOrDone();
        joinHoIfDone();
        snapshot("ho_resume");
    };

    auto stepHoDrain = [&]() {
        const std::set<std::string> drainStops =
            (liveSrv.turn && fault != WalkFault::DrainCutsTurn)
                ? std::set<std::string>{"drain", "start"}
                : std::set<std::string>{"start"};
        if (hoActor.atStep == "inspect") {
            hoActor.stepTo({"drain", "start", "rollback"});
        }
        if (hoActor.running && hoActor.atStep == "drain") {
            hoActor.stepTo(drainStops);
        }
        joinHoIfDone();
        snapshot("ho_drain");
    };

    auto stepHoStart = [&]() {
        if (hoActor.atStep == "inspect") {
            hoActor.stepTo({"start", "rollback"});
        }
        if (hoActor.running && hoActor.atStep == "start") {
            hoActor.stepTo({"rollback"});
        }
        joinHoIfDone();
        snapshot("ho_start");
    };

    auto stepHoRollback = [&]() {
        if (hoActor.atStep == "inspect") {
            hoActor.stepTo({"rollback"});
        }
        if (hoActor.running && hoActor.atStep == "rollback") {
            hoActor.stepTo({});
        }
        joinHoIfDone();
        snapshot("ho_rollback");
    };

    auto doHoCut = [&]() {
        hoActor.kill();
        hoLoop.reset();
        hoServer.reset();
        hoPlacement.reset();
        hoFs.reset();
        snapshot("ho_cut");
    };

    auto doTurnOpen = [&]() {
        liveSrv.turn = true;
        snapshot("turn_open");
    };

    auto doTurnDone = [&]() {
        liveSrv.commitSessionOrEntry(liveSrv.b, format5Writers, serverDir.string());
        liveSrv.turn = false;
        if (liveSrv.state == "draining") {
            liveSrv.closeStore();
            if (supLocked && supPc == "watch") {
                liveSrv.state = "exited";
                liveSrv.code = kDrainExit;
            } else {
                liveSrv.state = "down";
                liveSrv.b = 0;
                liveSrv.code = 0;
            }
        }
        snapshot("turn_done");
    };

    auto doSrvWrite = [&]() {
        liveSrv.commitSessionOrEntry(liveSrv.b, format5Writers, serverDir.string());
        snapshot("srv_write");
    };

    auto doSrvCrash = [&]() {
        liveSrv.closeStore();
        liveSrv.turn = false;
        if (supLocked && supPc == "watch") {
            liveSrv.state = "exited";
            liveSrv.code = 1;
        } else {
            liveSrv.state = "down";
            liveSrv.b = 0;
            liveSrv.code = 0;
        }
        snapshot("srv_crash");
    };

    auto doUserStop = [&]() {
        (void)obsFs->private_dir((serverDir / "supervise").string());
        supervision::noteIntentionalStop(*obsFs, (serverDir / "supervise").string());
        liveSrv.closeStore();
        liveSrv.turn = false;
        if (supLocked && supPc == "watch") {
            liveSrv.state = "exited";
            liveSrv.code = 0;
        } else {
            liveSrv.state = "down";
            liveSrv.b = 0;
            liveSrv.code = 0;
        }
        snapshot("user_stop");
    };

    // Execute targeted fault walks deterministically so every `--fault` exercises the exact real C++ code path and
    // leaves the spec cleanly.
    if (fault != WalkFault::None) {
        switch (fault) {
        case WalkFault::IgnoreLiveOld:
            doMigrate();
            break;
        case WalkFault::BlameBuildForLiveOld:
            startSupervisor();
            break;
        case WalkFault::SkipMigration:
            startSupervisor();
            stepSupCheck();
            break;
        case WalkFault::ServeUntested:
            doPlace(2, false);
            startHandoverTo(2);
            break;
        case WalkFault::NoHandoverLock:
            startSupervisor();
            stepSupCheck(); // serving 1
            startHandoverTo(2);
            stepHoDrain();
            stepSupWatch();
            stepSupReap();
            stepHoStart();  // serving 2 (lateBroken)
            stepSupCheck(); // watch 2
            doSrvCrash();
            stepSupWatch();
            stepSupReap();
            stepSupCheck(); // respawn 2
            doSrvCrash();
            stepSupWatch();
            stepSupReap();  // enters rollback WITHOUT handover lock!
            break;
        case WalkFault::BlindRollback:
        case WalkFault::SpinOnFormatRefusal:
            startSupervisor();
            stepSupCheck(); // serving 1
            startHandoverTo(3);
            stepHoDrain();
            stepSupWatch();
            stepSupReap();
            stepHoStart();  // serving 3 (5/5)
            stepSupCheck(); // watch 3
            doSrvWrite();   // raises maxReader = 5!
            doSrvCrash();
            stepSupWatch();
            stepSupReap();
            stepSupCheck(); // respawn 3
            doSrvCrash();
            stepSupWatch();
            stepSupReap();  // rollback
            stepSupRollback();
            break;
        case WalkFault::VersionOnlyRollback:
            startSupervisor();
            stepSupCheck(); // serving 1
            startHandoverTo(3);
            stepHoDrain();
            stepSupWatch();
            stepSupReap();
            stepHoStart();  // serving 3 (5/5, WITHOUT writing format 5!)
            stepSupCheck(); // watch 3
            doSrvCrash();
            stepSupWatch();
            stepSupReap();
            stepSupCheck(); // respawn 3
            doSrvCrash();
            stepSupWatch();
            stepSupReap();  // rollback
            stepSupRollback();
            break;
        case WalkFault::DrainCutsTurn:
            startSupervisor();
            stepSupCheck(); // serving 1
            doTurnOpen();
            startHandoverTo(2);
            stepHoDrain();  // cuts open turn via stop()!
            break;
        case WalkFault::DrainStops:
            startSupervisor();
            stepSupCheck(); // serving 1
            startHandoverTo(2);
            stepHoDrain();
            stepSupWatch();
            stepSupReap();  // marks stopped on ExitDrain!
            break;
        case WalkFault::StopRestarts:
            startSupervisor();
            stepSupCheck(); // serving 1
            doUserStop();
            stepSupWatch();
            stepSupReap();  // clears stopped & returns to check!
            break;
        case WalkFault::None:
            break;
        }
    } else {
        // Seeded walk across all enabled cross-layer transitions!
        bool didCutOnce = false;
        bool didSupDieOnce = false;
        bool didUserStopOnce = false;
        for (std::size_t step = 0; step < maxSteps; ++step) {
            const TraceRow cur = trace.back();
            std::vector<std::function<void()>> enabled;

            if (cur.oldLayout == "live") {
                enabled.push_back(doOldStop);
                if (below(3) == 0) {
                    enabled.push_back(doMigrate);
                }
            } else if (cur.oldLayout == "cold" && cur.srvState == "down") {
                enabled.push_back(doMigrate);
            }

            for (int b = 1; b <= 3; ++b) {
                if (cur.placed.count(b) == 0) {
                    enabled.push_back([&, b]() { doPlace(b, true); });
                }
            }

            if (cur.cur == 0 && cur.hlock == "none" && cur.hstage == "none") {
                for (int b : cur.tested) {
                    enabled.push_back([&, b]() { doSetInitialCurrent(b); });
                }
            }

            // Progress in-flight handover (`ho` or cut `handover/state`).
            if (cur.hstage != "none") {
                if (cur.hlock == "none") {
                    enabled.push_back(startHandoverResume);
                } else if (cur.hlock == "ho") {
                    if (!didCutOnce && below(4) == 0) {
                        enabled.push_back([&]() {
                            didCutOnce = true;
                            doHoCut();
                        });
                    }
                    if (cur.hstage == "draining" && cur.srvState != "draining") {
                        enabled.push_back(stepHoDrain);
                    } else if (cur.hstage == "starting" && (cur.srvState == "down" ||
                               (cur.srvState == "serving" && cur.srvB == cur.htarget))) {
                        enabled.push_back(stepHoStart);
                    } else if (cur.hstage == "rolling-back" && cur.srvState == "down") {
                        enabled.push_back(stepHoRollback);
                    }
                }
            }

            // Start new handover when settled on a running/current build.
            if (cur.hlock == "none" && cur.hstage == "none" && cur.cur != 0 && cur.oldLayout == "none" &&
                cur.srvState != "exited") {
                for (int t : cur.tested) {
                    if (t == cur.cur) {
                        continue;
                    }
                    const int wt = (t == 1) ? 3 : 5;
                    const int rc = (cur.cur == 3) ? 5 : 3;
                    const int wc = (cur.cur == 1) ? 3 : 5;
                    if (wt < rc) {
                        continue;
                    }
                    if (wt < wc && (cur.srvState == "serving" || cur.srvState == "draining" ||
                                    cur.maxReader > static_cast<std::uint32_t>(wt))) {
                        continue;
                    }
                    enabled.push_back([&, t]() { startHandoverTo(t); });
                }
            }

            // Server runtime actions.
            if (cur.srvState == "serving" && !cur.srvTurn && cur.sessions < 6) {
                enabled.push_back(doTurnOpen);
                if (format5Writers.count(cur.srvB) > 0) {
                    enabled.push_back(doSrvWrite);
                }
            }
            if ((cur.srvState == "serving" || cur.srvState == "draining") && cur.srvTurn) {
                enabled.push_back(doTurnDone);
            }
            if (cur.srvState == "serving" && !cur.srvTurn && lateBroken.count(cur.srvB) > 0) {
                enabled.push_back(doSrvCrash);
            }
            if (!didUserStopOnce && cur.srvState == "serving" && !cur.srvTurn &&
                (!cur.supLock || cur.supPc == "watch") && cur.hlock == "none" && cur.hstage == "none" &&
                below(6) == 0) {
                enabled.push_back([&]() {
                    didUserStopOnce = true;
                    doUserStop();
                });
            }

            // Supervisor actions.
            if (!cur.supLock && cur.cur != 0 && !cur.sstopped &&
                (!cur.shalted || cur.sbuild != cur.cur || cur.explicitPick == cur.cur) && cur.oldLayout != "live") {
                enabled.push_back([&]() { (void)startSupervisor(); });
            }
            if (cur.supLock) {
                if (!didSupDieOnce && below(8) == 0) {
                    enabled.push_back([&]() {
                        didSupDieOnce = true;
                        doSupDie();
                    });
                }
                if (cur.supPc == "check") {
                    if (cur.hlock != "ho" && cur.hstage == "none") {
                        enabled.push_back(stepSupCheck);
                    }
                } else if (cur.supPc == "watch") {
                    if (cur.srvState == "exited" || cur.srvState == "down" ||
                        (cur.hlock == "none" && cur.hstage == "none" && cur.srvState == "serving" &&
                         !cur.srvTurn && cur.srvB != cur.cur)) {
                        enabled.push_back(stepSupWatch);
                    }
                } else if (cur.supPc == "reap" && cur.srvState == "exited") {
                    if (cur.srvCode == kDrainExit || cur.srvCode == 0 ||
                        (cur.hlock == "none" && cur.hstage == "none")) {
                        enabled.push_back(stepSupReap);
                    }
                } else if (cur.supPc == "rollback") {
                    enabled.push_back(stepSupRollback);
                }
            }

            if (enabled.empty()) {
                break;
            }
            enabled[below(enabled.size())]();
        }
    }

    // Cleanly stop any paused worker threads before exiting.
    hoActor.kill();
    supActor.kill();
    hoLoop.reset();
    supLoop.reset();
    liveSrv.closeStore();

    // Emit `TakeoverTraceData.tla`.
    std::ostringstream tla;
    tla << "-------------------------- MODULE TakeoverTraceData --------------------------\n";
    tla << "EXTENDS Integers, Sequences\n\n";
    tla << "TInitPlaced == " << tlaIntSet(initPlaced) << "\n";
    tla << "TInitTested == " << tlaIntSet(initTested) << "\n";
    tla << "TInitCur == " << initCur << "\n";
    tla << "TInitOldLayout == \"" << initOldLayout << "\"\n";
    tla << "TStartupBroken == " << tlaIntSet(startupBroken) << "\n";
    tla << "TLateBroken == " << tlaIntSet(lateBroken) << "\n";
    tla << "TFormat5Writers == " << tlaIntSet(format5Writers) << "\n";
    tla << "TMaxCrashesBeforeRollback == " << maxCrashesBeforeRollback << "\n";
    tla << "TMaxRestarts == " << maxRestarts << "\n\n";
    tla << "Trace == <<\n";
    for (std::size_t i = 0; i < trace.size(); ++i) {
        const TraceRow& r = trace[i];
        tla << "  [op |-> \"" << r.op << "\", b |-> " << r.b << ", arg |-> " << r.arg
            << ", oldLayout |-> \"" << r.oldLayout << "\", migrated |-> " << tlaBool(r.migrated)
            << ", placed |-> " << tlaIntSet(r.placed) << ", tested |-> " << tlaIntSet(r.tested)
            << ", cur |-> " << r.cur << ", prev |-> " << r.prev
            << ", hlock |-> \"" << r.hlock << "\""
            << ", hstate |-> [stage |-> \"" << r.hstage << "\", target |-> " << r.htarget
            << ", previous |-> " << r.hprev << "]"
            << ", supLock |-> " << tlaBool(r.supLock) << ", supPc |-> \"" << r.supPc << "\""
            << ", supDisk |-> [build |-> " << r.sbuild << ", crashes |-> " << r.scrashes
            << ", shunned |-> " << tlaIntSet(r.sshunned) << ", halted |-> " << tlaBool(r.shalted)
            << ", stopped |-> " << tlaBool(r.sstopped) << "]"
            << ", explicitPick |-> " << r.explicitPick
            << ", srv |-> [state |-> \"" << r.srvState << "\", b |-> " << r.srvB
            << ", turn |-> " << tlaBool(r.srvTurn) << ", code |-> " << r.srvCode << "]"
            << ", maxReader |-> " << r.maxReader << ", sessions |-> " << r.sessions << "]"
            << (i + 1 < trace.size() ? "," : "") << "\n";
    }
    tla << ">>\n";
    tla << "=============================================================================\n";

    if (const fs::path parent = fs::path(outPath).parent_path(); !parent.empty()) {
        fs::create_directories(parent);
    }
    std::ofstream out(outPath, std::ios::binary);
    out << tla.str();
    out.close();
    if (!out) {
        die("cannot write " + outPath);
    }
    return 0;
}
