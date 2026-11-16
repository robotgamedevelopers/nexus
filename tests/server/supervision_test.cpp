// Unit tests for src/server/supervision.{h,cpp} and src/server/supervise_state.{h,cpp} (label `unit`: no real files,
// sockets, processes, or sleeps).

#include "server/supervision.h"
#include "store/fs.h"

#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace store = nexus::store;
using namespace nexus::server;
using namespace nexus::server::supervision;

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

const std::string kB1(64, '1');
const std::string kB2(64, '2');
const std::string kB3(64, '3');

class FakePlacement final : public handover::PlacementPort {
public:
    std::string cur = kB1;
    std::string prev;
    std::set<std::string> placed{kB1, kB2, kB3};
    std::set<std::string> tested{kB1, kB2, kB3};
    int switchCount = 0;
    std::vector<std::string> history;

    int active(std::string* current, std::string* previous, builds::Result& r) override {
        r = {};
        *current = cur;
        *previous = prev;
        return builds::kExitOk;
    }

    int checkSwitch(const std::string& hash, builds::Result& r, bool allowUntested) override {
        r = {};
        if (placed.count(hash) == 0) {
            r.err.push_back("build " + hash + " is not placed");
            r.exit = builds::kExitRefused;
            return r.exit;
        }
        if (!allowUntested && tested.count(hash) == 0) {
            r.err.push_back("build " + hash + " has no test evidence");
            r.exit = builds::kExitRefused;
            return r.exit;
        }
        return builds::kExitOk;
    }

    int makeCurrent(const std::string& hash, builds::Result& r, bool allowUntested) override {
        if (checkSwitch(hash, r, allowUntested) != builds::kExitOk) {
            return r.exit;
        }
        if (cur != hash) {
            prev = cur;
            cur = hash;
            ++switchCount;
            history.push_back(hash);
        }
        return builds::kExitOk;
    }
};

class FakeHandoverGate final : public HandoverGate {
public:
    explicit FakeHandoverGate(store::Fs& fs, std::string dir = "/handover") : fs_(fs), dir_(std::move(dir)) {}

    bool externalLocked = false;
    int resumeCalls = 0;
    std::set<std::string> picks;

    bool isLockedByOther() override { return externalLocked; }

    store::Status tryLock(std::unique_ptr<store::FileLock>& out) override {
        if (externalLocked) {
            return store::Status::busy("external handover holds lock");
        }
        const store::Status s = fs_.private_dir(dir_);
        if (!s.is_ok()) {
            return s;
        }
        return fs_.lock(dir_, store::LockMode::Exclusive, out);
    }

    store::Status resumeIfNeeded() override {
        ++resumeCalls;
        return store::Status::ok();
    }

    bool hasExplicitPick(const std::string& build) override { return picks.count(build) > 0; }
    void clearExplicitPick(const std::string& build) override { picks.erase(build); }

private:
    store::Fs& fs_;
    std::string dir_;
};

class FakeChildPort final : public ChildPort {
public:
    enum class Mode { Healthy, StartupFail, CrashOnWatch };

    std::map<std::string, Mode> modes{{kB1, Mode::Healthy}, {kB2, Mode::Healthy}, {kB3, Mode::Healthy}};
    std::map<std::string, int> crashQuota;  // optional transient crash count before becoming healthy
    ChildObs current;
    std::vector<std::string> spawned;
    int drainCalls = 0;
    int stopCalls = 0;

    store::Status inspect(ChildObs* out) override {
        *out = current;
        return store::Status::ok();
    }

    store::Status spawn(const std::string& expectedBuild, std::uint64_t, ChildObs* out) override {
        spawned.push_back(expectedBuild);
        const Mode m = modes.count(expectedBuild) ? modes[expectedBuild] : Mode::Healthy;
        if (m == Mode::StartupFail) {
            current = ChildObs{ChildState::Exited, expectedBuild, 1};
            *out = current;
            return store::Status::ok();
        }
        current = ChildObs{ChildState::Serving, expectedBuild, 0};
        *out = current;
        return store::Status::ok();
    }

    store::Status drainToExit() override {
        ++drainCalls;
        current = ChildObs{ChildState::Exited, current.build, kDrainExit};
        return store::Status::ok();
    }

    store::Status stopToExit() override {
        ++stopCalls;
        current = ChildObs{ChildState::Exited, current.build, 0};
        return store::Status::ok();
    }

    int consumeExit() override {
        const int c = current.exitCode;
        current = ChildObs{};
        return c;
    }

    void triggerWatchEvent() {
        if (current.state != ChildState::Serving) {
            return;
        }
        const std::string b = current.build;
        auto q = crashQuota.find(b);
        if (q != crashQuota.end() && q->second > 0) {
            --q->second;
            current = ChildObs{ChildState::Exited, b, 1};
            return;
        }
        if (modes[b] == Mode::CrashOnWatch) {
            current = ChildObs{ChildState::Exited, b, 139};  // simulated post-start SIGSEGV
        }
    }
};

SuperviseState loadStateFromMem(store::Fs& fs, const std::string& dir = "/supervise") {
    store::Bytes raw;
    CHECK(fs.read_all(dir + "/" + kStateFile, raw).is_ok());
    const std::string text(reinterpret_cast<const char*>(raw.data()), raw.size());
    SuperviseState st;
    std::string why;
    CHECK(readState(text, &st, &why));
    return st;
}

void testState() {
    SuperviseState st;
    st.build = kB2;
    st.crashes = 2;
    st.halted = false;
    st.stopped = true;
    st.picked = kB2;
    st.shunned = {kB1, kB3};

    const std::string encoded = formatState(st);
    SuperviseState parsed;
    std::string why;
    CHECK(readState(encoded, &parsed, &why));
    CHECK(parsed == st);

    // Empty build ("none") and no shunned/picked entries.
    SuperviseState empty;
    CHECK(readState(formatState(empty), &parsed, &why));
    CHECK(parsed == empty);

    // Reject damaged / malformed inputs.
    CHECK(!readState("", &parsed, &why));
    CHECK(!readState("nexus-supervise 99\nbuild none\ncrashes 0\nhalted 0\nstopped 0\nend\n", &parsed, &why));
    CHECK(!readState("nexus-supervise 1\nbuild none\ncrashes 0\nhalted 0\nstopped 0\n", &parsed, &why));  // no `end`
    CHECK(!readState("nexus-supervise 1\nbuild not-a-hash\ncrashes 0\nhalted 0\nstopped 0\nend\n", &parsed, &why));
    CHECK(!readState("nexus-supervise 1\nbuild none\ncrashes -1\nhalted 0\nstopped 0\nend\n", &parsed, &why));
    CHECK(!readState("nexus-supervise 1\nbuild none\ncrashes 0\nhalted 2\nstopped 0\nend\n", &parsed, &why));
    CHECK(!readState("nexus-supervise 1\nbuild none\ncrashes 0\nhalted 0\nstopped 0\nshun bad\nend\n", &parsed, &why));
    CHECK(!readState(encoded + "extra\n", &parsed, &why));

    // noteHandoverBegin / noteHandoverTarget / noteIntentionalStop are no-ops when /supervise does not exist.
    store::MemFs fs;
    const std::uint64_t callsBefore = fs.calls();
    CHECK(noteHandoverBegin(fs, "/supervise").is_ok());
    CHECK(noteHandoverTarget(fs, "/supervise", kB2).is_ok());
    CHECK(noteIntentionalStop(fs, "/supervise").is_ok());
    CHECK(fs.calls() == callsBefore);

    // Once /supervise exists, updates persist atomically and unshun explicitly chosen target.
    CHECK(fs.private_dir("/supervise").is_ok());
    SuperviseState initial;
    initial.build = kB1;
    initial.crashes = 3;
    initial.halted = true;
    initial.shunned = {kB2, kB3};
    CHECK(writeStateFile(fs, "/supervise", initial).is_ok());

    CHECK(noteHandoverBegin(fs, "/supervise").is_ok());
    SuperviseState afterBegin = loadStateFromMem(fs);
    CHECK(!afterBegin.halted);
    CHECK(!afterBegin.stopped);
    CHECK(afterBegin.shunned.count(kB2) == 1);

    CHECK(noteHandoverTarget(fs, "/supervise", kB2).is_ok());
    SuperviseState afterTarget = loadStateFromMem(fs);
    CHECK(afterTarget.build == kB2);
    CHECK(afterTarget.picked == kB2);
    CHECK(afterTarget.crashes == 0);
    CHECK(afterTarget.shunned.count(kB2) == 0);
    CHECK(afterTarget.shunned.count(kB3) == 1);

    CHECK(noteIntentionalStop(fs, "/supervise").is_ok());
    CHECK(loadStateFromMem(fs).stopped);
}

void testUnits() {
    const ServiceUnitPaths def = serviceUnitPaths("/home/operator/.nexus/server/", "/home/operator/",
                                                  "/home/operator/.nexus/server");
    CHECK(def.launchdLabel == "com.robotgamedevelopers.nexus-server");
    CHECK(def.launchdPlistPath == "/home/operator/Library/LaunchAgents/com.robotgamedevelopers.nexus-server.plist");
    CHECK(def.systemdUnitName == "nexus-server");
    CHECK(def.systemdUnitPath == "/home/operator/.config/systemd/user/nexus-server.service");
    CHECK(def.programPath == "/home/operator/.nexus/server/current/bin/nexus-server");
    CHECK(def.logPath == "/home/operator/.nexus/server/server.log");

    const ServiceUnitPaths custom =
        serviceUnitPaths("/srv/nexus-staging", "/home/operator", "/home/operator/.nexus/server");
    CHECK(custom.launchdLabel != def.launchdLabel);
    CHECK(custom.launchdLabel.rfind("com.robotgamedevelopers.nexus-server.", 0) == 0);
    CHECK(custom.systemdUnitName.rfind("nexus-server-", 0) == 0);
    CHECK(custom.programPath == "/srv/nexus-staging/current/bin/nexus-server");

    // XML escaping in launchd plist.
    const std::string plist = launchdPlist("com.robotgamedevelopers.nexus-server",
                                           "/opt/a&b/current/bin/nexus-server", "/opt/a&b", "/opt/a&b/server.log",
                                           "/Users/op<1>", "/usr/local/bin:/usr/bin");
    CHECK(plist.find("<string>/opt/a&amp;b/current/bin/nexus-server</string>") != std::string::npos);
    CHECK(plist.find("<string>supervise</string>") != std::string::npos);
    CHECK(plist.find("<string>--dir</string>") != std::string::npos);
    CHECK(plist.find("<key>SuccessfulExit</key>\n        <false/>") != std::string::npos);
    CHECK(plist.find("<string>/Users/op&lt;1&gt;</string>") != std::string::npos);

    // systemd specifier (`%`), space (`\x20` in WorkingDirectory), quote (`"`), and backslash (`\`) escaping.
    const std::string unit = systemdUnit("/srv/100%/current/bin/nexus-server", "/srv/100%", "/home/op\"x\\y",
                                         "/usr/local/bin:/usr/bin");
    CHECK(unit.find("WorkingDirectory=/srv/100%%\n") != std::string::npos);
    CHECK(unit.find("ExecStart=\"/srv/100%%/current/bin/nexus-server\" supervise --dir \"/srv/100%%\"") !=
          std::string::npos);
    CHECK(unit.find("Environment=\"HOME=/home/op\\\"x\\\\y\"") != std::string::npos);
    CHECK(unit.find("Restart=on-failure\n") != std::string::npos);
    CHECK(unit.find("RestartPreventExitStatus=3 78\n") != std::string::npos);
    CHECK(unit.find("WantedBy=default.target\n") != std::string::npos);
}

void testLifecycle() {
    store::MemFs fs;
    FakePlacement placement;
    FakeHandoverGate gate(fs);
    FakeChildPort child;
    std::uint64_t clockMs = 1'000;

    // 1. Cold start -> drain (exit 75) to kB2 -> intentional stop (exit 0).
    int watchSeen = 0;
    Options opts;
    opts.now_ms = [&] { return clockMs; };
    opts.sleep_ms = [&](std::uint64_t d) { clockMs += d; };
    opts.hook = [&](const std::string& step) {
        if (step == "watch") {
            ++watchSeen;
            if (watchSeen == 1) {
                CHECK(child.current.build == kB1);
                // Simulate external handover switching builds/active to kB2 and draining kB1.
                builds::Result br;
                CHECK(placement.makeCurrent(kB2, br, false) == builds::kExitOk);
                CHECK(noteHandoverTarget(fs, "/supervise", kB2).is_ok());
                child.current = ChildObs{ChildState::Exited, kB1, kDrainExit};
            } else if (watchSeen == 2) {
                CHECK(child.current.build == kB2);
                // Simulate `nexus-server stop`.
                CHECK(noteIntentionalStop(fs, "/supervise").is_ok());
                child.current = ChildObs{ChildState::Exited, kB2, 0};
            }
        }
    };

    SupervisorLoop loop(fs, placement, gate, child, "/supervise", opts);
    Result r;
    CHECK(loop.run(r) == kExitOk);
    CHECK(watchSeen == 2);
    CHECK((child.spawned == std::vector<std::string>{kB1, kB2}));
    CHECK(loadStateFromMem(fs).stopped);

    // 2. Restarting supervisor while `stopped == true` exits 0 immediately without spawning.
    child.spawned.clear();
    Options restartOpts;
    restartOpts.now_ms = [&] { return clockMs; };
    restartOpts.sleep_ms = [&](std::uint64_t d) { clockMs += d; };
    SupervisorLoop restarted(fs, placement, gate, child, "/supervise", restartOpts);
    CHECK(restarted.run(r) == kExitOk);
    CHECK(child.spawned.empty());

    // 3. `--start` (`clear_stopped_on_start = true`) clears `stopped` and starts kB2.
    bool stopSig = false;
    Options startOpts = restartOpts;
    startOpts.clear_stopped_on_start = true;
    startOpts.stop_requested = [&] { return stopSig; };
    startOpts.hook = [&](const std::string& step) {
        if (step == "watch") {
            CHECK(child.current.build == kB2);
            stopSig = true;  // SIGTERM to supervisor: stops cleanly WITHOUT setting `stopped = true` on disk!
        }
    };
    SupervisorLoop resumed(fs, placement, gate, child, "/supervise", startOpts);
    CHECK(resumed.run(r) == kExitOk);
    CHECK((child.spawned == std::vector<std::string>{kB2}));
    CHECK(!loadStateFromMem(fs).stopped);

    // 4. Busy lock returns kExitBusy (4).
    std::unique_ptr<store::FileLock> held;
    CHECK(fs.lock("/supervise", store::LockMode::Exclusive, held).is_ok());
    Options busyOpts = restartOpts;
    busyOpts.lock_wait_ms = 50;
    SupervisorLoop contender(fs, placement, gate, child, "/supervise", busyOpts);
    CHECK(contender.run(r) == kExitBusy);
    held.reset();

    // 5. Untested current build refuses with kExitRefused (3).
    placement.tested.erase(kB2);
    SupervisorLoop untested(fs, placement, gate, child, "/supervise", restartOpts);
    CHECK(untested.run(r) == kExitRefused);
}

void testRollback() {
    store::MemFs fs;
    FakePlacement placement;
    placement.cur = kB2;
    placement.prev = kB1;
    FakeHandoverGate gate(fs);
    FakeChildPort child;
    // kB2 starts SERVING cleanly, then crashes on watch (post-start failure!).
    child.modes[kB2] = FakeChildPort::Mode::CrashOnWatch;
    child.modes[kB1] = FakeChildPort::Mode::Healthy;
    // Also give kB1 one transient crash so we verify stability window resets `crashes` without clearing `shunned`!
    child.crashQuota[kB1] = 1;

    std::uint64_t clockMs = 10'000;
    bool sawHandoverLockOnRollback = false;
    bool stopSig = false;
    int b1HealthyWatches = 0;

    SupervisorLoop* loopPtr = nullptr;
    Options opts;
    opts.max_crashes_before_rollback = 2;
    opts.max_restarts = 5;
    opts.stable_ms = 60'000;
    opts.backoff_ms = 500;
    opts.now_ms = [&] { return clockMs; };
    opts.sleep_ms = [&](std::uint64_t d) { clockMs += d; };
    opts.stop_requested = [&] { return stopSig; };
    opts.hook = [&](const std::string& step) {
        if (step == "rollback") {
            sawHandoverLockOnRollback = loopPtr->holdsHandoverLock();
        }
        if (step == "watch") {
            if (child.current.build == kB2 || child.crashQuota[kB1] > 0) {
                child.triggerWatchEvent();
            } else if (child.current.build == kB1) {
                ++b1HealthyWatches;
                if (b1HealthyWatches == 1) {
                    // Advance past stability window; next watch iteration persists `crashes = 0`.
                    clockMs += 60'000;
                } else if (b1HealthyWatches == 3) {
                    const SuperviseState st = loadStateFromMem(fs);
                    CHECK(st.build == kB1);
                    CHECK(st.crashes == 0);
                    CHECK(st.shunned.count(kB2) == 1);
                    stopSig = true;
                }
            }
        }
    };

    SupervisorLoop loop(fs, placement, gate, child, "/supervise", opts);
    loopPtr = &loop;
    Result r;
    CHECK(loop.run(r) == kExitOk);
    CHECK(sawHandoverLockOnRollback);
    CHECK(placement.cur == kB1);
    CHECK(placement.prev == kB2);
    // kB2 spawned twice (post-start crash x2 -> rollback), kB1 spawned twice (1 transient crash -> stable!).
    CHECK((child.spawned == std::vector<std::string>{kB2, kB2, kB1, kB1}));
}

void testDualFail() {
    store::MemFs fs;
    FakePlacement placement;
    placement.cur = kB2;
    placement.prev = kB1;
    FakeHandoverGate gate(fs);
    FakeChildPort child;
    // BOTH kB2 and kB1 crash after reaching SERVING!
    child.modes[kB2] = FakeChildPort::Mode::CrashOnWatch;
    child.modes[kB1] = FakeChildPort::Mode::CrashOnWatch;

    std::uint64_t clockMs = 1'000;
    Options opts;
    opts.max_crashes_before_rollback = 2;
    opts.max_restarts = 3;
    opts.now_ms = [&] { return clockMs; };
    opts.sleep_ms = [&](std::uint64_t d) { clockMs += d; };
    opts.hook = [&](const std::string& step) {
        if (step == "watch") {
            child.triggerWatchEvent();
        }
    };

    SupervisorLoop loop(fs, placement, gate, child, "/supervise", opts);
    Result r;
    CHECK(loop.run(r) == kExitHalted);
    // Rolled back once from kB2 -> kB1; NEVER ping-ponged back to shunned kB2!
    CHECK(placement.switchCount == 1);
    CHECK(placement.cur == kB1);
    CHECK(placement.prev == kB2);
    CHECK((child.spawned == std::vector<std::string>{kB2, kB2, kB1, kB1, kB1}));

    const SuperviseState haltedSt = loadStateFromMem(fs);
    CHECK(haltedSt.halted);
    CHECK(haltedSt.crashes == 3);
    CHECK(haltedSt.shunned == std::set<std::string>{kB2});

    // A restarted supervisor sees `halted == true` on disk and exits kExitHalted (78) without spawning anything!
    child.spawned.clear();
    SupervisorLoop second(fs, placement, gate, child, "/supervise", opts);
    CHECK(second.run(r) == kExitHalted);
    CHECK(child.spawned.empty());
}

void testCrashSweep() {
    // Measure total MemFs steps during a post-start rollback run, then inject fail_at(k) + crash() at every k.
    auto runOnce = [](store::MemFs& fs, bool inject, std::uint64_t failAt, bool* completedOk) {
        FakePlacement placement;
        placement.cur = kB2;
        placement.prev = kB1;
        FakeHandoverGate gate(fs);
        FakeChildPort child;
        child.modes[kB2] = FakeChildPort::Mode::CrashOnWatch;
        child.modes[kB1] = FakeChildPort::Mode::Healthy;
        std::uint64_t clockMs = 5'000;
        bool stopSig = false;

        if (inject) {
            fs.fail_at(failAt);
        }
        Options opts;
        opts.max_crashes_before_rollback = 2;
        opts.max_restarts = 4;
        opts.now_ms = [&] { return clockMs; };
        opts.sleep_ms = [&](std::uint64_t d) { clockMs += d; };
        opts.stop_requested = [&] { return stopSig; };
        opts.hook = [&](const std::string& step) {
            if (step == "watch") {
                if (child.current.build == kB2) {
                    child.triggerWatchEvent();
                } else {
                    stopSig = true;
                }
            }
        };
        SupervisorLoop loop(fs, placement, gate, child, "/supervise", opts);
        Result r;
        const int rc = loop.run(r);
        *completedOk = (rc == kExitOk && placement.cur == kB1);
        return placement.cur;
    };

    store::MemFs baseline;
    bool ok = false;
    (void)runOnce(baseline, false, 0, &ok);
    CHECK(ok);
    const std::uint64_t totalCalls = baseline.calls();
    CHECK(totalCalls >= 8);

    for (bool keepVolatile : {false, true}) {
        for (std::uint64_t k = 1; k <= totalCalls; ++k) {
            store::MemFs fs;
            bool firstOk = false;
            const std::string curAfterCrash = runOnce(fs, true, k, &firstOk);
            fs.crash(fs.plan_all(keepVolatile));

            // Now recover on the crashed filesystem: regardless of where power was lost, a fresh supervisor recovers to
            // SERVING kB1!
            FakePlacement recoveryPlacement;
            recoveryPlacement.cur = curAfterCrash;
            recoveryPlacement.prev = (curAfterCrash == kB2) ? kB1 : kB2;
            FakeHandoverGate gate(fs);
            FakeChildPort child;
            child.modes[kB2] = FakeChildPort::Mode::CrashOnWatch;
            child.modes[kB1] = FakeChildPort::Mode::Healthy;
            std::uint64_t clockMs = 20'000;
            bool stopSig = false;
            Options opts;
            opts.max_crashes_before_rollback = 2;
            opts.max_restarts = 4;
            opts.now_ms = [&] { return clockMs; };
            opts.sleep_ms = [&](std::uint64_t d) { clockMs += d; };
            opts.stop_requested = [&] { return stopSig; };
            opts.hook = [&](const std::string& step) {
                if (step == "watch") {
                    if (child.current.build == kB2) {
                        child.triggerWatchEvent();
                    } else {
                        CHECK(child.current.build == kB1);
                        stopSig = true;
                    }
                }
            };
            SupervisorLoop recover(fs, recoveryPlacement, gate, child, "/supervise", opts);
            Result r;
            CHECK(recover.run(r) == kExitOk);
            CHECK(recoveryPlacement.cur == kB1);
        }
    }
}

} // namespace

int main(int argc, char** argv) {
    const std::string group = argc > 1 ? argv[1] : "all";
    if (group == "all" || group == "state") {
        testState();
    }
    if (group == "all" || group == "units") {
        testUnits();
    }
    if (group == "all" || group == "lifecycle") {
        testLifecycle();
    }
    if (group == "all" || group == "rollback") {
        testRollback();
    }
    if (group == "all" || group == "dual_fail") {
        testDualFail();
    }
    if (group == "all" || group == "crash") {
        testCrashSweep();
    }
    std::printf("supervision_test (%s): %d checks passed\n", group.c_str(), gChecks);
    return 0;
}
