// Unit tests for cross-layer takeover composition (layer 6, `takeover-binding`):
//   - Handover startup-failure rollback from a 5/5 build to a 3/3 build when the disk store is still at reader 3, vs
//     refusal + clean state clear when disk reader is 5.
//   - Supervision post-start-crash rollback from a 5/5 build to a 3/3 build when the disk store is still at reader 3.
//   - Supervision refusal + shunning (zero spin!) + bounded clean halt (78) when a 5/5 build wrote reader 5 to the
//     store before crashing repeatedly.
//   - Old-layout migration guard in MemFs refusing a live old server cleanly before supervision starts, then migrating
//     once and preserving sessions across Store::open.

#include "server/builds.h"
#include "server/handover.h"
#include "server/migration.h"
#include "server/supervision.h"
#include "store/fs.h"
#include "store/store.h"

#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace store = nexus::store;
using namespace nexus::server;

namespace {

int gChecks = 0;

#define CHECK(cond)                                                                                      \
    do {                                                                                                 \
        ++gChecks;                                                                                       \
        if (!(cond)) {                                                                                   \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                         \
            std::exit(1);                                                                                \
        }                                                                                                \
    } while (0)

const std::string kB1(64, '1'); // 3/3 base build
const std::string kB2(64, '2'); // 5/3 rollback-compatible build
const std::string kB3(64, '3'); // 5/5 breaking-reader build

struct BuildSpec {
    std::uint32_t writes = 3;
    std::uint32_t reader = 3;
    bool tested = true;
};

// Models Layer 2 (`Builds`) + Layer 5 (`readDiskFormats` / `diskFormatsRefusal`) in memory:
//   - Forward `checkSwitch` / `makeCurrent`: enforces `Writes(target) >= Reader(cur)` (so 5/5 -> 3/3 is refused!).
//   - `rollback`: enforces `diskMaxReader <= Writes(target)` (so 5/5 -> 3/3 is ALLOWED when diskMaxReader == 3, and
//     REFUSED when diskMaxReader == 5!).
class FormatAwarePlacement final : public handover::PlacementPort {
public:
    std::string cur = kB1;
    std::string prev;
    std::uint32_t diskMaxReader = 3;
    std::map<std::string, BuildSpec> specs{
        {kB1, {3, 3, true}},
        {kB2, {5, 3, true}},
        {kB3, {5, 5, true}},
    };

    int active(std::string* current, std::string* previous, builds::Result& r) override {
        r = {};
        *current = cur;
        *previous = prev;
        return builds::kExitOk;
    }

    int checkSwitch(const std::string& hash, builds::Result& r, bool allowUntested) override {
        r = {};
        const auto it = specs.find(hash);
        if (it == specs.end()) {
            r.err.push_back("build " + hash + " is not placed");
            return r.exit = builds::kExitRefused;
        }
        if (!allowUntested && !it->second.tested) {
            r.err.push_back("build " + hash + " is untested");
            return r.exit = builds::kExitRefused;
        }
        if (!cur.empty()) {
            const auto cit = specs.find(cur);
            if (cit != specs.end() && it->second.writes < cit->second.reader) {
                r.err.push_back("forward switch refuses downgrading below current build's reader version");
                return r.exit = builds::kExitRefused;
            }
        }
        return r.exit = builds::kExitOk;
    }

    int makeCurrent(const std::string& hash, builds::Result& r, bool allowUntested) override {
        const int rc = checkSwitch(hash, r, allowUntested);
        if (rc != builds::kExitOk) {
            return rc;
        }
        if (hash != cur) {
            prev = cur;
            cur = hash;
        }
        return r.exit = builds::kExitOk;
    }

    int rollback(const std::string& hash, builds::Result& r) override {
        r = {};
        const std::string target = hash.empty() ? prev : hash;
        const auto it = specs.find(target);
        if (it == specs.end()) {
            r.err.push_back("build " + target + " is not placed");
            return r.exit = builds::kExitRefused;
        }
        if (!it->second.tested) {
            r.err.push_back("build " + target + " is untested");
            return r.exit = builds::kExitRefused;
        }
        if (diskMaxReader > it->second.writes) {
            r.err.push_back("store on disk holds format reader " + std::to_string(diskMaxReader) +
                            ", which build " + target + " (writes " + std::to_string(it->second.writes) +
                            ") cannot read");
            return r.exit = builds::kExitRefused;
        }
        if (target != cur) {
            prev = cur;
            cur = target;
        }
        return r.exit = builds::kExitOk;
    }
};

class FakeHandoverServer final : public handover::ServerPort {
public:
    bool running = true;
    bool kept = true;
    std::string servingHash = kB1;
    std::set<std::string> startupBroken;

    store::Status probe(ServerSeen* out) override {
        *out = ServerSeen{};
        out->serving = running;
        out->build = running ? servingHash : std::string();
        return store::Status::ok();
    }

    handover::DrainReply requestDrain(std::string*) override {
        running = false;
        kept = false;
        servingHash.clear();
        return handover::DrainReply::Accepted;
    }

    store::Status stop() override {
        running = false;
        kept = false;
        servingHash.clear();
        return store::Status::ok();
    }

    handover::StartReply stepStart(const std::string& expectedBuild, std::uint64_t, std::uint64_t,
                                   std::string* why) override {
        if (startupBroken.count(expectedBuild) > 0) {
            if (why) {
                *why = "simulated startup failure on " + expectedBuild;
            }
            return handover::StartReply::Failed;
        }
        running = true;
        kept = false;
        servingHash = expectedBuild;
        return handover::StartReply::Waiting;
    }

    void keepStarted() override { kept = true; }
    void abortStarted() override {
        if (!kept) {
            running = false;
            servingHash.clear();
        }
    }
};

class FakeGate final : public supervision::HandoverGate {
public:
    store::Fs& fs;
    explicit FakeGate(store::Fs& f) : fs(f) {}

    bool isLockedByOther() override { return false; }

    store::Status tryLock(std::unique_ptr<store::FileLock>& out) override {
        const store::Status s = fs.private_dir("/handover");
        if (!s.is_ok()) {
            return s;
        }
        return fs.lock("/handover", store::LockMode::Exclusive, out);
    }

    store::Status resumeIfNeeded(bool) override { return store::Status::ok(); }
};

class FakeSupChild final : public supervision::ChildPort {
public:
    supervision::ChildObs current;
    std::set<std::string> crashAfterServe;
    int spawns = 0;

    store::Status inspect(supervision::ChildObs* out) override {
        *out = current;
        return store::Status::ok();
    }

    store::Status spawn(const std::string& expectedBuild, std::uint64_t, supervision::ChildObs* out) override {
        ++spawns;
        current.state = supervision::ChildState::Serving;
        current.build = expectedBuild;
        current.exitCode = 0;
        *out = current;
        return store::Status::ok();
    }

    store::Status drainToExit() override {
        current.state = supervision::ChildState::Exited;
        current.exitCode = kDrainExit;
        return store::Status::ok();
    }

    store::Status stopToExit() override {
        current.state = supervision::ChildState::Exited;
        current.exitCode = 0;
        return store::Status::ok();
    }

    int consumeExit() override {
        const int c = current.exitCode;
        current = {};
        return c;
    }
};

void testFormatRollbackHandover() {
    // Case 1: Handover from B1 (3/3) to startup-broken B3 (5/5) while diskMaxReader == 3 MUST roll back to B1!
    {
        store::MemFs fs;
        FormatAwarePlacement placement;
        placement.cur = kB1;
        placement.diskMaxReader = 3;
        FakeHandoverServer srv;
        srv.running = true;
        srv.servingHash = kB1;
        srv.startupBroken.insert(kB3);

        std::uint64_t clockMs = 1000;
        handover::Options opts;
        opts.now_ms = [&] { return clockMs; };
        opts.sleep_ms = [&](std::uint64_t d) { clockMs += d; };

        handover::Handover ho(fs, placement, srv, "/handover", opts);
        handover::Result r;
        const int rc = ho.to(kB3, r);
        CHECK(rc == handover::kExitRolledBack);
        CHECK(placement.cur == kB1);
        CHECK(srv.running && srv.servingHash == kB1);

        std::uint64_t sz = 0;
        CHECK(fs.size("/handover/state", sz).code() == store::Status::Code::NotFound);
    }

    // Case 2: Interrupted handover in `rolling-back` where B3 already raised diskMaxReader == 5 refuses rollback to B1
    // AND clears `/handover/state` so handover is not permanently wedged.
    {
        store::MemFs fs;
        CHECK(fs.private_dir("/handover").is_ok());
        handover::StateRecord rec{kB3, kB1, handover::Stage::RollingBack};
        const std::string text = handover::formatState(rec);
        CHECK(fs.write_new("/handover/state", store::Bytes(text.begin(), text.end())).is_ok());

        FormatAwarePlacement placement;
        placement.cur = kB3;
        placement.prev = kB1;
        placement.diskMaxReader = 5;
        FakeHandoverServer srv;
        srv.running = false;
        srv.startupBroken.insert(kB3);

        std::uint64_t clockMs = 1000;
        handover::Options opts;
        opts.now_ms = [&] { return clockMs; };
        opts.sleep_ms = [&](std::uint64_t d) { clockMs += d; };

        handover::Handover ho(fs, placement, srv, "/handover", opts);
        handover::Result r;
        const int rc = ho.resume(r);
        CHECK(rc == handover::kExitRefused);
        CHECK(placement.cur == kB3);
        std::uint64_t sz = 0;
        CHECK(fs.size("/handover/state", sz).code() == store::Status::Code::NotFound);
    }
}

void testFormatRollbackSupervision() {
    // B3 (5/5) is active with B1 (3/3) as previous, and diskMaxReader == 3 (B3 never wrote format 5).
    // When B3 crashes twice after SERVING, SupervisorLoop MUST roll back to B1 without spinning or halting!
    store::MemFs fs;
    FormatAwarePlacement placement;
    placement.cur = kB3;
    placement.prev = kB1;
    placement.diskMaxReader = 3;
    FakeGate gate(fs);
    FakeSupChild child;
    child.crashAfterServe.insert(kB3);

    std::uint64_t clockMs = 1000;
    bool stopSig = false;
    bool servedB1AfterRollback = false;
    int steps = 0;

    supervision::Options opts;
    opts.max_crashes_before_rollback = 2;
    opts.max_restarts = 4;
    opts.now_ms = [&] { return clockMs; };
    opts.sleep_ms = [&](std::uint64_t d) { clockMs += d; };
    opts.stop_requested = [&] { return stopSig; };
    opts.hook = [&](const std::string& step) {
        if (++steps > 80) {
            std::fprintf(stderr, "FAIL: SupervisorLoop spun past 80 hook steps on 5/5 -> 3/3 rollback!\n");
            std::exit(1);
        }
        if (step == "watch") {
            if (child.current.build == kB3) {
                child.current.state = supervision::ChildState::Exited;
                child.current.exitCode = 1;
            } else if (child.current.build == kB1) {
                servedB1AfterRollback = true;
                stopSig = true;
            }
        }
    };

    supervision::SupervisorLoop sup(fs, placement, gate, child, "/supervise", opts);
    supervision::Result r;
    const int rc = sup.run(r);
    CHECK(rc == supervision::kExitOk);
    CHECK(placement.cur == kB1);
    CHECK(servedB1AfterRollback);

    supervision::SuperviseState st;
    CHECK(supervision::loadEffectiveState(fs, "/supervise", &st).is_ok());
    CHECK(st.shunned.count(kB3) == 1);
    CHECK(!st.halted);
}

void testFormatRefusedShunsAndHalts() {
    // B3 (5/5) wrote diskMaxReader = 5 before crashing repeatedly with B1 (3/3) as previous.
    // At crash #2 (`max_crashes_before_rollback = 2 < max_restarts = 4`), rollback to B1 is refused: SupervisorLoop
    // MUST shun B1 (zero spin!), finish B3's remaining restarts (4 total), and halt cleanly with exit 78 while keeping
    // `cur == B3`!
    store::MemFs fs;
    FormatAwarePlacement placement;
    placement.cur = kB3;
    placement.prev = kB1;
    placement.diskMaxReader = 5;
    FakeGate gate(fs);
    FakeSupChild child;

    std::uint64_t clockMs = 1000;
    int steps = 0;
    int rollbacks = 0;

    supervision::Options opts;
    opts.max_crashes_before_rollback = 2;
    opts.max_restarts = 4;
    opts.now_ms = [&] { return clockMs; };
    opts.sleep_ms = [&](std::uint64_t d) { clockMs += d; };
    opts.hook = [&](const std::string& step) {
        if (++steps > 80) {
            std::fprintf(stderr, "FAIL: SupervisorLoop spun past 80 hook steps when format rollback was refused!\n");
            std::exit(1);
        }
        if (step == "rollback") {
            ++rollbacks;
        }
        if (step == "watch" && child.current.build == kB3) {
            child.current.state = supervision::ChildState::Exited;
            child.current.exitCode = 1;
        }
    };

    supervision::SupervisorLoop sup(fs, placement, gate, child, "/supervise", opts);
    supervision::Result r;
    const int rc = sup.run(r);
    CHECK(rc == supervision::kExitHalted);
    CHECK(rollbacks == 1);
    CHECK(child.spawns == 4);
    CHECK(placement.cur == kB3);

    supervision::SuperviseState st;
    CHECK(supervision::loadEffectiveState(fs, "/supervise", &st).is_ok());
    CHECK(st.halted);
    CHECK(st.crashes == 4);
    CHECK(st.shunned.count(kB1) == 1);
}

void testMigrationGuardsInMemory() {
    store::MemFs fs;
    {
        std::unique_ptr<store::Store> oldSt;
        CHECK(store::Store::open(fs, "/home/.nexus/store", {}, oldSt).is_ok());
        store::Change mk;
        mk.kind = store::Change::Kind::SetInfo;
        mk.info.ai_name = "alice";
        mk.info.harness = "claude";
        store::Change app;
        app.kind = store::Change::Kind::Append;
        app.entry = store::Entry{1, "hello-from-old-layout", true};
        const store::Change batch[] = {mk, app};
        CHECK(oldSt->commit(oldSt->next_session(), batch).is_ok());
    }
    const std::string drv = "{\"drivers\":[]}\n";
    CHECK(fs.write_new("/home/.nexus/drivers", store::Bytes(drv.begin(), drv.end())).is_ok());

    // Live old server answers on ~/.nexus/server.sock -> Failed (without conflict), nothing moved.
    {
        migration::Options mopts;
        mopts.old_server_live = [] { return true; };
        migration::Report rep;
        const store::Status s = migration::migrateOldLayout(fs, "/home/.nexus", "/home/.nexus/server", mopts, &rep);
        CHECK(!s.is_ok() && !rep.conflict && !rep.migrated);
        std::uint64_t sz = 0;
        CHECK(fs.size("/home/.nexus/drivers", sz).is_ok());
        CHECK(fs.size("/home/.nexus/server/drivers", sz).code() == store::Status::Code::NotFound);
    }

    // Old server stops -> migrates once cleanly and preserves session in /home/.nexus/server/store.
    {
        migration::Options mopts;
        mopts.old_server_live = [] { return false; };
        migration::Report rep;
        CHECK(migration::migrateOldLayout(fs, "/home/.nexus", "/home/.nexus/server", mopts, &rep).is_ok());
        CHECK(rep.migrated);

        std::unique_ptr<store::Store> newSt;
        CHECK(store::Store::open(fs, "/home/.nexus/server/store", {}, newSt).is_ok());
        CHECK(newSt->sessions().size() == 1);
        CHECK(newSt->sessions().begin()->second.transcript.size() == 1);
        CHECK(newSt->sessions().begin()->second.transcript[0].text == "hello-from-old-layout");
    }
}

} // namespace

int main(int argc, char** argv) {
    const std::string group = argc > 1 ? argv[1] : "all";
    if (group == "all" || group == "format_rollback_handover") {
        testFormatRollbackHandover();
    }
    if (group == "all" || group == "format_rollback_supervision") {
        testFormatRollbackSupervision();
    }
    if (group == "all" || group == "format_refused_shuns_and_halts") {
        testFormatRefusedShunsAndHalts();
    }
    if (group == "all" || group == "migration_guards") {
        testMigrationGuardsInMemory();
    }
    std::printf("takeover_test (%s): %d checks passed\n", group.c_str(), gChecks);
    return 0;
}
