// Unit tests of drain and handover (src/server/drain.h, src/server/handover.h): pure logic and MemFs only — no real
// files, processes, sockets, sleeps or clock.

#include "server/drain.h"
#include "server/handover.h"
#include "store/fs.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace nexus::server;
using namespace nexus::server::handover;
namespace store = nexus::store;

namespace {

int gChecks = 0;

#define CHECK(cond)                                                                             \
    do {                                                                                        \
        ++gChecks;                                                                              \
        if (!(cond)) {                                                                          \
            std::fprintf(stderr, "handover_test FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);  \
            std::exit(1);                                                                       \
        }                                                                                       \
    } while (0)

std::string makeHash(char fill) {
    return std::string(64, fill);
}

const std::string kB1 = makeHash('1');
const std::string kB2 = makeHash('2');
const std::string kB3 = makeHash('3');
const std::string kBad = makeHash('b');
const std::string kUntested = makeHash('d');

struct FakePlacement final : PlacementPort {
    std::string cur;
    std::string prev;
    std::set<std::string> placed;
    std::set<std::string> tested;
    std::set<std::string> badFormats;

    FakePlacement() {
        cur = kB1;
        placed = {kB1, kB2, kB3, kBad, kUntested};
        tested = {kB1, kB2, kB3, kBad};
    }

    int active(std::string* current, std::string* previous, builds::Result& r) override {
        *current = cur;
        *previous = prev;
        r.exit = builds::kExitOk;
        return r.exit;
    }

    int checkSwitch(const std::string& hash, builds::Result& r, bool allowUntested) override {
        if (!builds::isHash(hash)) {
            r.err.push_back("not a hash");
            return r.exit = builds::kExitRefused;
        }
        if (placed.find(hash) == placed.end()) {
            r.err.push_back("build " + hash + " is not placed");
            return r.exit = builds::kExitRefused;
        }
        if (!allowUntested && tested.find(hash) == tested.end()) {
            r.err.push_back("build " + hash + " is untested");
            return r.exit = builds::kExitRefused;
        }
        if (!cur.empty() && hash != cur && badFormats.find(hash) != badFormats.end()) {
            r.err.push_back("build " + hash + " cannot read current formats");
            return r.exit = builds::kExitRefused;
        }
        return r.exit = builds::kExitOk;
    }

    int makeCurrent(const std::string& hash, builds::Result& r, bool allowUntested) override {
        const int c = checkSwitch(hash, r, allowUntested);
        if (c != builds::kExitOk) {
            return c;
        }
        if (cur != hash) {
            prev = cur;
            cur = hash;
        }
        return r.exit = builds::kExitOk;
    }
};

struct FakeServer final : ServerPort {
    FakePlacement& placement;
    std::uint64_t& now;
    std::set<std::string> broken = {kBad};
    bool down = false;
    bool legacy = false;
    bool unknownDrain = false;
    std::string build;
    bool draining = false;
    bool turnOpen = false;
    std::uint64_t turnEndsAt = 0;
    std::uint64_t drainLimitMs = 1000;
    std::uint64_t drainDeadline = 0;
    int drainRequests = 0;
    int stopCalls = 0;
    int startCalls = 0;
    int aborts = 0;

    FakeServer(FakePlacement& p, std::uint64_t& clock) : placement(p), now(clock), build(kB1) {}

    void tick() {
        if (turnOpen && turnEndsAt > 0 && now >= turnEndsAt) {
            turnOpen = false;
        }
        if (!down && draining && (!turnOpen || now >= drainDeadline)) {
            down = true;
            draining = false;
            build.clear();
        }
    }

    store::Status probe(ServerSeen* out) override {
        tick();
        *out = ServerSeen{};
        if (down) {
            return store::Status::ok();
        }
        out->serving = true;
        out->legacy = legacy;
        out->build = legacy ? std::string() : build;
        out->draining = legacy ? false : draining;
        return store::Status::ok();
    }

    DrainReply requestDrain(std::string*) override {
        ++drainRequests;
        tick();
        if (down) {
            return DrainReply::Closed;
        }
        if (unknownDrain) {
            return DrainReply::UnknownRequest;
        }
        if (!draining) {
            draining = true;
            drainDeadline = now + drainLimitMs;
        }
        tick();
        return DrainReply::Accepted;
    }

    store::Status stop() override {
        ++stopCalls;
        down = true;
        legacy = false;
        draining = false;
        turnOpen = false;
        build.clear();
        return store::Status::ok();
    }

    StartReply stepStart(const std::string& expectedBuild, std::uint64_t elapsed_ms, std::uint64_t limit_ms,
                         std::string* why) override {
        ++startCalls;
        const std::string launch = placement.cur;
        if (broken.find(launch) != broken.end()) {
            if (elapsed_ms >= limit_ms) {
                *why = "build " + expectedBuild + " timed out before serving";
                return StartReply::Failed;
            }
            *why = "build " + expectedBuild + " exited with code 1 without serving";
            return StartReply::Failed;
        }
        down = false;
        legacy = false;
        draining = false;
        build = launch;
        return StartReply::Waiting;
    }

    void keepStarted() override {}
    void abortStarted() override { ++aborts; }
};

void testDrain() {
    Drain d(500);
    CHECK(!d.draining());
    CHECK(!d.late(0));
    CHECK(!d.stopNow(false, 0));
    CHECK(!d.untilDeadline(0).has_value());

    d.request(100);
    CHECK(d.draining());
    CHECK(!d.late(599));
    CHECK(d.late(600));
    CHECK(d.stopNow(false, 100));
    CHECK(!d.stopNow(true, 100));
    CHECK(!d.stopNow(true, 599));
    CHECK(d.stopNow(true, 600));
    CHECK(d.untilDeadline(100) == std::optional<std::uint64_t>(500));
    CHECK(d.untilDeadline(599) == std::optional<std::uint64_t>(1));
    CHECK(d.untilDeadline(600) == std::optional<std::uint64_t>(0));
    CHECK(d.untilDeadline(900) == std::optional<std::uint64_t>(0));

    // A second request preserves the first deadline.
    d.request(400);
    CHECK(d.late(600));

    CHECK(statusBuildLine("", false) == "build none");
    CHECK(statusBuildLine("", true) == "build none draining");
    CHECK(statusBuildLine(kB1, false) == "build " + kB1);
    CHECK(statusBuildLine(kB1, true) == "build " + kB1 + " draining");

    ServerSeen seen;
    CHECK(!parseStatusLines("STOPPING", "build none", true, &seen));
    CHECK(parseStatusLines("SERVING", "", false, &seen));
    CHECK(seen.serving && seen.legacy && seen.build.empty() && !seen.draining);

    CHECK(parseStatusLines("SERVING", "build none", true, &seen));
    CHECK(seen.serving && !seen.legacy && seen.build.empty() && !seen.draining);

    CHECK(parseStatusLines("SERVING", "build none draining", true, &seen));
    CHECK(seen.serving && !seen.legacy && seen.build.empty() && seen.draining);

    CHECK(parseStatusLines("SERVING", "build " + kB2, true, &seen));
    CHECK(seen.serving && !seen.legacy && seen.build == kB2 && !seen.draining);

    CHECK(parseStatusLines("SERVING", "build " + kB2 + " draining", true, &seen));
    CHECK(seen.serving && !seen.legacy && seen.build == kB2 && seen.draining);

    CHECK(!parseStatusLines("SERVING", "build not-a-hash", true, &seen));
    CHECK(!parseStatusLines("SERVING", "build " + kB2 + " extra", true, &seen));
    CHECK(!parseStatusLines("SERVING", "other", true, &seen));
}

void testState() {
    for (Stage st : {Stage::Switching, Stage::Draining, Stage::Starting, Stage::RollingBack}) {
        for (const std::string& prev : {std::string(), kB1}) {
            const StateRecord rec{kB2, prev, st};
            const std::string text = formatState(rec);
            StateRecord back;
            std::string why;
            CHECK(readState(text, &back, &why));
            CHECK(back == rec);
        }
    }

    StateRecord bad;
    std::string why;
    CHECK(!readState("", &bad, &why));
    CHECK(!readState("nexus-handover 2\ntarget " + kB2 + "\nprevious none\nstage switching\nend\n", &bad, &why));
    // Torn write missing `end`.
    CHECK(!readState("nexus-handover 1\ntarget " + kB2 + "\nprevious none\nstage switching\n", &bad, &why));
    // Trailing content past `end`.
    CHECK(!readState(formatState(StateRecord{kB2, kB1, Stage::Draining}) + "extra\n", &bad, &why));
    // Invalid target or previous hash.
    CHECK(!readState("nexus-handover 1\ntarget short\nprevious none\nstage switching\nend\n", &bad, &why));
    CHECK(!readState("nexus-handover 1\ntarget " + kB2 + "\nprevious bad\nstage switching\nend\n", &bad, &why));
    // Unknown stage or duplicate field.
    CHECK(!readState("nexus-handover 1\ntarget " + kB2 + "\nprevious none\nstage paused\nend\n", &bad, &why));
    CHECK(!readState("nexus-handover 1\ntarget " + kB2 + "\ntarget " + kB2 + "\nprevious none\nstage switching\nend\n",
                     &bad, &why));
    CHECK(!readState("nexus-handover 1\ntarget " + kB2 + "\nstage switching\nend\n", &bad, &why));
}

void testHandover() {
    // 1. Idle server drains immediately and hands over B1 -> B2.
    {
        store::MemFs fs;
        FakePlacement pl;
        std::uint64_t now = 0;
        FakeServer srv(pl, now);
        Options opts;
        opts.now_ms = [&] { return now; };
        opts.sleep_ms = [&](std::uint64_t ms) { now += ms; };
        Handover h(fs, pl, srv, "handover", opts);

        Result r;
        CHECK(h.to(kB2, r) == kExitOk);
        CHECK(pl.cur == kB2 && pl.prev == kB1);
        CHECK(!srv.down && srv.build == kB2 && !srv.draining);
        CHECK(srv.drainRequests == 1 && srv.stopCalls == 0);
        CHECK(!fs.has("handover/state"));

        // Already serving B2: second call is an immediate no-op.
        Result r2;
        CHECK(h.to(kB2, r2) == kExitOk);
        CHECK(!r2.out.empty() && r2.out[0] == "already serving " + kB2);
        CHECK(srv.drainRequests == 1);

        // Status reports idle and serving B2.
        Result st;
        CHECK(h.status(st) == kExitOk);
        CHECK(st.out.size() == 3);
        CHECK(st.out[0] == "handover idle");
        CHECK(st.out[2] == "server serving " + kB2);
    }

    // 2. Open turn finishes before drain deadline: never cut by stop().
    {
        store::MemFs fs;
        FakePlacement pl;
        std::uint64_t now = 0;
        FakeServer srv(pl, now);
        srv.turnOpen = true;
        srv.turnEndsAt = 120;
        srv.drainLimitMs = 1000;
        Options opts;
        opts.now_ms = [&] { return now; };
        opts.sleep_ms = [&](std::uint64_t ms) { now += ms; };
        Handover h(fs, pl, srv, "handover", opts);

        Result r;
        CHECK(h.to(kB2, r) == kExitOk);
        CHECK(now >= 120 && now < 1000);
        CHECK(srv.stopCalls == 0);
        CHECK(!srv.turnOpen && srv.build == kB2);
    }

    // 3. Stuck turn hits server's drain deadline and still hands over within bounded time.
    {
        store::MemFs fs;
        FakePlacement pl;
        std::uint64_t now = 0;
        FakeServer srv(pl, now);
        srv.turnOpen = true;
        srv.turnEndsAt = 0; // never finishes on its own
        srv.drainLimitMs = 200;
        Options opts;
        opts.now_ms = [&] { return now; };
        opts.sleep_ms = [&](std::uint64_t ms) { now += ms; };
        Handover h(fs, pl, srv, "handover", opts);

        Result r;
        CHECK(h.to(kB2, r) == kExitOk);
        CHECK(now >= 200);
        CHECK(srv.build == kB2);
    }

    // 4. Pre-drain legacy server (1-line status) and intermediate server (UnknownRequest on drain) use stop().
    for (int mode = 0; mode < 2; ++mode) {
        store::MemFs fs;
        FakePlacement pl;
        std::uint64_t now = 0;
        FakeServer srv(pl, now);
        srv.legacy = (mode == 0);
        srv.unknownDrain = (mode == 1);
        Options opts;
        opts.now_ms = [&] { return now; };
        opts.sleep_ms = [&](std::uint64_t ms) { now += ms; };
        Handover h(fs, pl, srv, "handover", opts);

        Result r;
        CHECK(h.to(kB2, r) == kExitOk);
        CHECK(srv.stopCalls == 1);
        CHECK(srv.build == kB2);
    }

    // 5. Cold start (`resume` when no server is running brings `current` up).
    {
        store::MemFs fs;
        FakePlacement pl;
        std::uint64_t now = 0;
        FakeServer srv(pl, now);
        srv.down = true;
        srv.build.clear();
        Options opts;
        opts.now_ms = [&] { return now; };
        opts.sleep_ms = [&](std::uint64_t ms) { now += ms; };
        Handover h(fs, pl, srv, "handover", opts);

        Result r;
        CHECK(h.resume(r) == kExitOk);
        CHECK(!srv.down && srv.build == kB1);
    }
}

void testRollback() {
    // 1. Handover to broken build kBad rolls back to kB1, leaves kB1 serving, and exits kExitRolledBack.
    {
        store::MemFs fs;
        FakePlacement pl;
        std::uint64_t now = 0;
        FakeServer srv(pl, now);
        Options opts;
        opts.now_ms = [&] { return now; };
        opts.sleep_ms = [&](std::uint64_t ms) { now += ms; };
        Handover h(fs, pl, srv, "handover", opts);

        Result r;
        CHECK(h.to(kBad, r) == kExitRolledBack);
        CHECK(pl.cur == kB1);
        CHECK(pl.prev == kBad); // builds/active pair is (kB1, kBad)
        CHECK(!srv.down && srv.build == kB1);
        CHECK(!fs.has("handover/state"));

        // Post-rollback crash with ~HasState: pl.prev is kBad (broken!), pl.cur is kB1 (working).
        // `resume` when server is down MUST NOT record pl.prev (kBad) as rprev in handover/state!
        srv.down = true;
        srv.build.clear();
        bool checkedState = false;
        opts.hook = [&](const std::string& step) {
            if (step == "start") {
                store::Bytes raw;
                CHECK(fs.read_all("handover/state", raw).is_ok());
                StateRecord rec;
                std::string why;
                CHECK(readState(std::string(reinterpret_cast<const char*>(raw.data()), raw.size()), &rec, &why));
                CHECK(rec.target == kB1);
                CHECK(rec.previous.empty());
                checkedState = true;
            }
        };
        Handover h2(fs, pl, srv, "handover", opts);
        Result r2;
        CHECK(h2.resume(r2) == kExitOk);
        CHECK(checkedState);
        CHECK(!srv.down && srv.build == kB1);
    }

    // 2. First install (`previous` empty) to broken build fails cleanly and clears state.
    {
        store::MemFs fs;
        FakePlacement pl;
        pl.cur.clear();
        pl.prev.clear();
        std::uint64_t now = 0;
        FakeServer srv(pl, now);
        srv.down = true;
        srv.build.clear();
        Options opts;
        opts.now_ms = [&] { return now; };
        opts.sleep_ms = [&](std::uint64_t ms) { now += ms; };
        Handover h(fs, pl, srv, "handover", opts);

        Result r;
        CHECK(h.to(kBad, r) == kExitFailed);
        CHECK(!fs.has("handover/state"));
    }
}

struct Cut {};

void testCrash() {
    // 1. Cut at EVERY hook step of both a good handover (kB1 -> kB2) and a broken handover (kB1 -> kBad -> kB1),
    //    then `resume()` finishes from what is on disk.
    for (const std::string& target : {kB2, kBad}) {
        std::vector<std::string> steps;
        {
            store::MemFs fs;
            FakePlacement pl;
            std::uint64_t now = 0;
            FakeServer srv(pl, now);
            Options opts;
            opts.now_ms = [&] { return now; };
            opts.sleep_ms = [&](std::uint64_t ms) { now += ms; };
            opts.hook = [&](const std::string& step) {
                if (step != "idle") {
                    steps.push_back(step);
                }
            };
            Handover h(fs, pl, srv, "handover", opts);
            Result r;
            (void)h.to(target, r);
        }
        CHECK(!steps.empty());

        for (std::size_t cutAt = 0; cutAt < steps.size(); ++cutAt) {
            store::MemFs fs;
            FakePlacement pl;
            std::uint64_t now = 0;
            FakeServer srv(pl, now);
            std::size_t seen = 0;
            Options opts;
            opts.now_ms = [&] { return now; };
            opts.sleep_ms = [&](std::uint64_t ms) { now += ms; };
            opts.hook = [&](const std::string& step) {
                if (step != "idle" && seen++ == cutAt) {
                    throw Cut{};
                }
            };
            {
                Handover h1(fs, pl, srv, "handover", opts);
                Result r1;
                bool cut = false;
                try {
                    (void)h1.to(target, r1);
                } catch (const Cut&) {
                    cut = true;
                }
                CHECK(cut);
                // Lock was released when h1's Run unwound.
                CHECK(fs.lock_holders("handover", store::LockMode::Exclusive) == 0);
            }

            // Simulate power-cut variant too: server down, then resume!
            for (bool powerCut : {false, true}) {
                store::MemFs fsCopy(fs);
                FakePlacement plCopy = pl;
                std::uint64_t nowCopy = now;
                FakeServer srvCopy(plCopy, nowCopy);
                srvCopy.down = powerCut ? true : srv.down;
                srvCopy.build = powerCut ? std::string() : srv.build;
                srvCopy.draining = powerCut ? false : srv.draining;
                if (powerCut) {
                    fsCopy.crash(fsCopy.plan_all(false));
                }

                Options resOpts;
                resOpts.now_ms = [&] { return nowCopy; };
                resOpts.sleep_ms = [&](std::uint64_t ms) { nowCopy += ms; };
                Handover h2(fsCopy, plCopy, srvCopy, "handover", resOpts);
                Result r2;
                CHECK(h2.resume(r2) == kExitOk);
                CHECK(!fsCopy.has("handover/state"));
                CHECK(!srvCopy.down && !srvCopy.draining);
                CHECK(srvCopy.build == plCopy.cur);
                CHECK(plCopy.cur == kB1 || plCopy.cur == kB2);
            }
        }
    }

    // 2. Unfinished handover to broken kBad cut at `switch` (so pl.cur == kBad!), followed by `handover kB3`
    //    (and kB3 is ALSO broken!): must first resume kBad -> rollback kB1, then try kB3 -> rollback kB1!
    {
        store::MemFs fs;
        FakePlacement pl;
        std::uint64_t now = 0;
        FakeServer srv(pl, now);
        srv.broken = {kBad, kB3};

        Options cutOpts;
        cutOpts.now_ms = [&] { return now; };
        cutOpts.sleep_ms = [&](std::uint64_t ms) { now += ms; };
        cutOpts.hook = [&](const std::string& step) {
            if (step == "wdrain") {
                throw Cut{};
            }
        };
        {
            Handover h1(fs, pl, srv, "handover", cutOpts);
            Result r1;
            try {
                (void)h1.to(kBad, r1);
            } catch (const Cut&) {
            }
        }
        CHECK(pl.cur == kBad); // active is currently broken kBad!

        Options opts2;
        opts2.now_ms = [&] { return now; };
        opts2.sleep_ms = [&](std::uint64_t ms) { now += ms; };
        Handover h2(fs, pl, srv, "handover", opts2);
        Result r2;
        CHECK(h2.to(kB3, r2) == kExitRolledBack);
        // Because h2 resumed kBad -> kB1 before starting kB3, kB3's rollback restored working kB1, NEVER broken kBad!
        CHECK(pl.cur == kB1);
        CHECK(!srv.down && srv.build == kB1);
        CHECK(!fs.has("handover/state"));
    }

    // 3. Exhaustive MemFs::fail_at(k) + MemFs::crash sweep at every mutating call, followed by resume().
    for (const std::string& target : {kB2, kBad}) {
        std::uint64_t totalCalls = 0;
        {
            store::MemFs probeFs;
            FakePlacement pl;
            std::uint64_t now = 0;
            FakeServer srv(pl, now);
            Options opts;
            opts.now_ms = [&] { return now; };
            opts.sleep_ms = [&](std::uint64_t ms) { now += ms; };
            Handover h(probeFs, pl, srv, "handover", opts);
            Result r;
            (void)h.to(target, r);
            totalCalls = probeFs.calls();
        }
        CHECK(totalCalls > 0);

        store::Rng rng(42);
        for (std::uint64_t k = 1; k <= totalCalls; ++k) {
            store::MemFs fs;
            fs.fail_at(k);
            FakePlacement pl;
            std::uint64_t now = 0;
            FakeServer srv(pl, now);
            Options opts;
            opts.now_ms = [&] { return now; };
            opts.sleep_ms = [&](std::uint64_t ms) { now += ms; };
            {
                Handover h1(fs, pl, srv, "handover", opts);
                Result r1;
                (void)h1.to(target, r1);
            }
            // Crash with random subset of unsynced ops surviving, server down, then resume.
            fs.crash(fs.plan(rng));
            srv.down = true;
            srv.draining = false;
            srv.build.clear();

            // Leftover `.state-*` files, if any, must be cleaned by resume().
            Handover h2(fs, pl, srv, "handover", opts);
            Result r2;
            CHECK(h2.resume(r2) == kExitOk);
            CHECK(!fs.has("handover/state"));
            std::vector<std::string> leftNames;
            if (fs.list("handover", leftNames).is_ok()) {
                for (const std::string& n : leftNames) {
                    CHECK(n.rfind(kStateTmpPrefix, 0) != 0);
                }
            }
            CHECK(!srv.down && srv.build == pl.cur);
            CHECK(pl.cur == kB1 || pl.cur == kB2);
        }
    }
}

void testRefusals() {
    store::MemFs fs;
    FakePlacement pl;
    pl.badFormats.insert(kB3);
    std::uint64_t now = 0;
    FakeServer srv(pl, now);
    Options opts;
    opts.now_ms = [&] { return now; };
    opts.sleep_ms = [&](std::uint64_t ms) { now += ms; };
    Handover h(fs, pl, srv, "handover", opts);

    Result r;
    // Malformed hash.
    CHECK(h.to("not-a-hash", r) == kExitRefused);
    // Unplaced build.
    CHECK(h.to(makeHash('f'), r) == kExitRefused);
    // Untested build.
    CHECK(h.to(kUntested, r) == kExitRefused);
    // Incompatible formats.
    CHECK(h.to(kB3, r) == kExitRefused);
    CHECK(!fs.has("handover/state"));

    // Lock busy for the entire lock_wait_ms (instant via fake clock).
    std::unique_ptr<store::FileLock> otherHold;
    CHECK(fs.private_dir("handover").is_ok());
    CHECK(fs.lock("handover", store::LockMode::Exclusive, otherHold).is_ok());
    const std::uint64_t before = now;
    CHECK(h.to(kB2, r) == kExitBusy);
    CHECK(now - before >= kLockWaitMs);
    otherHold.reset();

    // Resume when no build is current.
    pl.cur.clear();
    pl.prev.clear();
    CHECK(h.resume(r) == kExitRefused);

    // Corrupt handover/state on disk fails cleanly without overwriting anything.
    const std::string badText = "corrupt\n";
    CHECK(fs.write_new("handover/state",
                       std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(badText.data()),
                                                     badText.size()))
              .is_ok());
    CHECK(h.resume(r) == kExitFailed);
    CHECK(h.status(r) == kExitFailed);
}

} // namespace

int main(int argc, char** argv) {
    const std::string only = argc > 1 ? argv[1] : std::string();
    const struct {
        const char* name;
        void (*fn)();
    } groups[] = {
        {"drain", testDrain},
        {"state", testState},
        {"handover", testHandover},
        {"rollback", testRollback},
        {"crash", testCrash},
        {"refusals", testRefusals},
    };
    int ran = 0;
    for (const auto& g : groups) {
        if (only.empty() || only == g.name) {
            g.fn();
            ++ran;
        }
    }
    if (ran == 0) {
        std::fprintf(stderr, "handover_test: unknown group `%s`\n", only.c_str());
        return 2;
    }
    std::printf("handover_test (%s): %d checks passed\n", only.empty() ? "all" : only.c_str(), gChecks);
    return 0;
}
