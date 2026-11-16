// supervision_walk: a seeded random walk of the real supervision engine (src/server/supervision.cpp) over real placed
// builds (`Builds`) and real POSIX files and OS locks (`PosixFs`), written as a trace for
// models/supervision/SupervisionTrace.tla.
//
// Two supervisor actors (s1, s2), each on its own thread with its own PosixFs and SupervisorLoop instance, coordinate
// over real `<work>/builds`, `<work>/supervise`, and `<work>/handover` directories while external handovers, post-start
// crashes, intentional `stop` requests, concurrent lock races, and supervisor deaths (`SIGKILL` unwinding) interleave
// at every hook step. All read-only placed build trees under `<work>` are made writable and removed unconditionally
// before exit.
//
// Usage: supervision_walk <seed> <steps> [--fault <no-sup-lock|no-handover-lock|drain-stops|stop-restarts|
//                                                  no-post-start-rollback|ping-pong-rollback|no-halt-bound|
//                                                  in-memory-state|serve-untested>] <out.tla>

#include "server/builds.h"
#include "server/supervision.h"
#include "store/fs.h"

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
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using namespace nexus::server;
using namespace nexus::server::supervision;

namespace {

struct Killed {};

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

void writeTreeFile(const fs::path& p, const std::string& content, bool exec) {
    fs::create_directories(p.parent_path());
    std::ofstream out(p, std::ios::binary);
    out << content;
    out.close();
    if (!out) {
        throw std::runtime_error("cannot write " + p.string());
    }
    fs::permissions(p, exec ? fs::perms(0755) : fs::perms(0644));
}

struct SimChild {
    ChildState state = ChildState::Down;
    int b = 0;
    int code = 0;
};

class WalkHandoverGate final : public HandoverGate {
public:
    nexus::store::Fs& fs;
    std::string handoverDir;
    bool& extLocked;
    std::string& explicitPick;

    WalkHandoverGate(nexus::store::Fs& f, std::string dir, bool& ext, std::string& pick)
        : fs(f), handoverDir(std::move(dir)), extLocked(ext), explicitPick(pick) {}

    bool isLockedByOther() override { return extLocked; }

    nexus::store::Status tryLock(std::unique_ptr<nexus::store::FileLock>& out) override {
        const nexus::store::Status ds = fs.private_dir(handoverDir);
        if (!ds.is_ok()) {
            return ds;
        }
        return fs.lock(handoverDir, nexus::store::LockMode::Exclusive, out);
    }

    nexus::store::Status resumeIfNeeded() override { return nexus::store::Status::ok(); }

    bool hasExplicitPick(const std::string& build) override { return !build.empty() && explicitPick == build; }

    void clearExplicitPick(const std::string& build) override {
        if (explicitPick == build) {
            explicitPick.clear();
        }
    }
};

class WalkChildPort final : public ChildPort {
public:
    SimChild& sim;
    const std::map<std::string, int>& idByHash;
    const std::map<int, std::string>& hashById;
    const std::set<int>& startupBroken;

    WalkChildPort(SimChild& s, const std::map<std::string, int>& toId, const std::map<int, std::string>& toHash,
                  const std::set<int>& sb)
        : sim(s), idByHash(toId), hashById(toHash), startupBroken(sb) {}

    nexus::store::Status inspect(ChildObs* out) override {
        *out = ChildObs{};
        out->state = sim.state;
        const auto it = hashById.find(sim.b);
        out->build = (it == hashById.end()) ? std::string() : it->second;
        out->exitCode = sim.code;
        return nexus::store::Status::ok();
    }

    nexus::store::Status spawn(const std::string& expectedBuild, std::uint64_t, ChildObs* out) override {
        const auto it = idByHash.find(expectedBuild);
        const int id = (it == idByHash.end()) ? 0 : it->second;
        if (startupBroken.count(id) > 0) {
            sim.state = ChildState::Exited;
            sim.b = id;
            sim.code = 1;
        } else {
            sim.state = ChildState::Serving;
            sim.b = id;
            sim.code = 0;
        }
        return inspect(out);
    }

    nexus::store::Status drainToExit() override {
        sim.state = ChildState::Exited;
        sim.code = kDrainExit;
        return nexus::store::Status::ok();
    }

    nexus::store::Status stopToExit() override {
        sim.state = ChildState::Exited;
        sim.code = 0;
        return nexus::store::Status::ok();
    }

    int consumeExit() override {
        const int c = sim.code;
        sim = SimChild{};
        return c;
    }
};

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
    std::uint64_t clockMs = 0;
    std::string at = "idle";
    std::function<void()> job;
    bool hasJob = false;
    std::unique_ptr<nexus::store::Fs> fs;
    std::unique_ptr<handover::PlacementPort> placement;
    std::unique_ptr<HandoverGate> gate;
    std::unique_ptr<ChildPort> child;
    std::unique_ptr<SupervisorLoop> loop;
    std::thread thread;
};

void park(Actor& a, const std::string& step) {
    if (step == "idle") {
        return;
    }
    std::unique_lock<std::mutex> lk(gm);
    a.at = step;
    a.parked = true;
    ++a.events;
    gcv.notify_all();
    gcv.wait(lk, [&] { return a.go; });
    a.go = false;
    a.parked = false;
    if (a.kill) {
        a.kill = false;
        a.at = "idle";
        throw Killed{};
    }
}

void actorMain(Actor& a) {
    for (;;) {
        std::function<void()> fn;
        {
            std::unique_lock<std::mutex> lk(gm);
            gcv.wait(lk, [&] { return a.hasJob || a.quit; });
            if (a.quit) {
                return;
            }
            fn = std::move(a.job);
            a.hasJob = false;
        }
        try {
            fn();
        } catch (const Killed&) {
        }
        {
            std::unique_lock<std::mutex> lk(gm);
            a.busy = false;
            a.parked = false;
            a.at = "idle";
            ++a.events;
            gcv.notify_all();
        }
    }
}

void startJob(Actor& a, std::function<void()> fn) {
    std::uint64_t before = 0;
    {
        std::unique_lock<std::mutex> lk(gm);
        before = a.events;
        a.busy = true;
        a.kill = false;
        a.job = std::move(fn);
        a.hasJob = true;
        gcv.notify_all();
        gcv.wait(lk, [&] { return a.events > before && (a.parked || !a.busy); });
    }
}

void stepParked(Actor& a, std::uint64_t addClockMs = 0) {
    std::unique_lock<std::mutex> lk(gm);
    const std::uint64_t before = a.events;
    a.clockMs += addClockMs;
    a.go = true;
    gcv.notify_all();
    gcv.wait(lk, [&] { return a.events > before && (a.parked || !a.busy); });
}

void killParked(Actor& a) {
    std::unique_lock<std::mutex> lk(gm);
    const std::uint64_t before = a.events;
    a.kill = true;
    a.go = true;
    gcv.notify_all();
    gcv.wait(lk, [&] { return a.events > before && !a.busy; });
}

std::string tlaSet(const std::set<int>& s) {
    std::ostringstream out;
    out << "{";
    bool first = true;
    for (int x : s) {
        if (!first) {
            out << ", ";
        }
        first = false;
        out << x;
    }
    out << "}";
    return out.str();
}

std::string tlaBool(bool v) {
    return v ? "TRUE" : "FALSE";
}

bool parseFault(const std::string& name, Fault* out) {
    if (name == "no-sup-lock") {
        *out = Fault::NoSupLock;
        return true;
    }
    if (name == "no-handover-lock") {
        *out = Fault::NoHandoverLock;
        return true;
    }
    if (name == "drain-stops") {
        *out = Fault::DrainStops;
        return true;
    }
    if (name == "stop-restarts") {
        *out = Fault::StopRestarts;
        return true;
    }
    if (name == "no-post-start-rollback") {
        *out = Fault::NoPostStartRollback;
        return true;
    }
    if (name == "ping-pong-rollback") {
        *out = Fault::PingPongRollback;
        return true;
    }
    if (name == "no-halt-bound") {
        *out = Fault::NoHaltBound;
        return true;
    }
    if (name == "in-memory-state") {
        *out = Fault::InMemoryState;
        return true;
    }
    if (name == "serve-untested") {
        *out = Fault::ServeUntested;
        return true;
    }
    return false;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 4 && argc != 6) {
        std::fprintf(stderr, "usage: supervision_walk <seed> <steps> [--fault <name>] <out.tla>\n");
        return 2;
    }
    const std::uint64_t seed = std::strtoull(argv[1], nullptr, 10);
    const std::size_t maxSteps = static_cast<std::size_t>(std::strtoull(argv[2], nullptr, 10));
    Fault fault = Fault::None;
    std::string outPath;
    if (argc == 6) {
        if (std::strcmp(argv[3], "--fault") != 0 || !parseFault(argv[4], &fault)) {
            std::fprintf(stderr, "supervision_walk: unknown fault `%s`\n", argv[4]);
            return 2;
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
    const fs::path work = (outFile.parent_path().empty() ? fs::path(".") : outFile.parent_path()) /
                          (outFile.filename().string() + ".walk-files");
    removeAllSafe(work);

    int rc = 0;
    try {
        fs::create_directories(work);
        const std::string serverDir = work.string();
        const std::string superviseDir = serverDir + "/" + kSuperviseDir;
        const std::string handoverDir = serverDir + "/" + handover::kHandoverDir;

        std::set<int> tested{1, 2};
        std::set<int> startupBroken;
        std::set<int> lateBroken{2};
        if (seed == 2 || fault == Fault::PingPongRollback || fault == Fault::NoHaltBound) {
            tested = {1, 2};
            startupBroken = {};
            lateBroken = {1, 2};
        } else if (seed == 3) {
            tested = {1, 2, 3};
            startupBroken = {3};
            lateBroken = {2};
        }

        // Place 3 real builds in `<work>/builds`.
        std::map<int, std::string> hashById;
        std::map<std::string, int> idByHash;
        {
            builds::Builds setup(serverDir, {});
            const std::string formats = builds::thisBuildFormats();
            for (int id = 1; id <= 3; ++id) {
                const fs::path src = work / ("src-" + std::to_string(id));
                writeTreeFile(src / "bin/nexus-server", "#!/bin/sh\n# build " + std::to_string(id) + "\nexit 0\n",
                              true);
                writeTreeFile(src / "drivers/claude/dist/main.js", "// build " + std::to_string(id) + "\n", false);
                writeTreeFile(src / "formats", formats, false);
                std::string h;
                const nexus::store::Status hs = builds::hashTree(src.string(), &h);
                if (!hs.is_ok()) {
                    throw std::runtime_error("hashTree failed: " + hs.message());
                }
                std::string evPath;
                if (tested.count(id) > 0) {
                    const fs::path ep = work / ("ev-" + std::to_string(id));
                    writeTreeFile(ep, builds::evidenceText({h, "pass", "supervision_walk"}), false);
                    evPath = ep.string();
                }
                builds::Result br;
                if (setup.place(src.string(), evPath, br) != builds::kExitOk) {
                    throw std::runtime_error("cannot place build " + std::to_string(id));
                }
                hashById[id] = h;
                idByHash[h] = id;
                fs::remove_all(src);
                if (!evPath.empty()) {
                    fs::remove(evPath);
                }
            }
            builds::Result br;
            if (setup.makeCurrent(hashById[1], br, false) != builds::kExitOk) {
                throw std::runtime_error("cannot make initial build 1 current");
            }
        }

        std::unique_ptr<nexus::store::Fs> envFs = nexus::store::make_posix_fs();
        (void)envFs->private_dir(superviseDir);
        (void)envFs->private_dir(handoverDir);

        SimChild simChild;
        bool svcStopped = false;
        bool svcHalted = false;
        bool extHoLocked = false;
        int extHoTarget = 0;
        std::unique_ptr<nexus::store::FileLock> extHoHold;
        std::string explicitPick;

        Actor a1;
        a1.name = "s1";
        Actor a2;
        a2.name = "s2";
        Actor* actors[2] = {&a1, &a2};

        for (Actor* ap : actors) {
            nexus::store::PosixOptions po;
            po.now_ms = [ap] { return ap->clockMs; };
            po.sleep_ms = [ap](std::uint64_t d) { ap->clockMs += d; };
            ap->fs = nexus::store::make_posix_fs(po);

            builds::Options bo;
            bo.now_ms = [ap] { return ap->clockMs; };
            bo.sleep_ms = [ap](std::uint64_t d) { ap->clockMs += d; };
            ap->placement = handover::makeBuildsPlacement(serverDir, bo);
            ap->gate = std::make_unique<WalkHandoverGate>(*ap->fs, handoverDir, extHoLocked, explicitPick);
            ap->child = std::make_unique<WalkChildPort>(simChild, idByHash, hashById, startupBroken);

            Options opts;
            opts.hook = [ap](const std::string& step) { park(*ap, step); };
            opts.now_ms = [ap] { return ap->clockMs; };
            opts.sleep_ms = [ap](std::uint64_t d) { ap->clockMs += d; };
            opts.max_crashes_before_rollback = 2;
            opts.max_restarts = 3;
            opts.lock_wait_ms = 5'000;
            opts.service_stopped = &svcStopped;
            opts.service_halted = &svcHalted;
            opts.fault = fault;
            ap->loop = std::make_unique<SupervisorLoop>(*ap->fs, *ap->placement, *ap->gate, *ap->child, superviseDir,
                                                        opts);
            ap->thread = std::thread(actorMain, std::ref(*ap));
        }

        auto cleanupThreads = [&] {
            for (Actor* ap : actors) {
                if (ap->busy && ap->parked) {
                    killParked(*ap);
                }
                {
                    std::unique_lock<std::mutex> lk(gm);
                    ap->quit = true;
                    gcv.notify_all();
                }
                if (ap->thread.joinable()) {
                    ap->thread.join();
                }
            }
            extHoHold.reset();
        };

        auto readCurPrev = [&](int* curOut, int* prevOut) {
            builds::Builds b(serverDir, {});
            std::string ch;
            std::string ph;
            builds::Result br;
            if (b.active(&ch, &ph, br) != builds::kExitOk) {
                throw std::runtime_error("cannot read builds/active");
            }
            *curOut = ch.empty() ? 0 : idByHash[ch];
            *prevOut = ph.empty() ? 0 : idByHash[ph];
        };

        auto currentSupLock = [&]() -> std::string {
            if (a1.loop->holdsLock()) {
                return "\"s1\"";
            }
            if (a2.loop->holdsLock()) {
                return "\"s2\"";
            }
            return "\"none\"";
        };

        auto currentHLock = [&]() -> std::string {
            if (extHoLocked) {
                return "\"ho\"";
            }
            if (a1.loop->holdsHandoverLock()) {
                return "\"s1\"";
            }
            if (a2.loop->holdsHandoverLock()) {
                return "\"s2\"";
            }
            return "\"none\"";
        };

        auto formatDiskRecord = [&]() -> std::string {
            nexus::store::Bytes raw;
            const nexus::store::Status rs = envFs->read_all(superviseDir + "/" + kStateFile, raw);
            SuperviseState st;
            if (rs.is_ok()) {
                std::string why;
                const std::string text(reinterpret_cast<const char*>(raw.data()), raw.size());
                if (!readState(text, &st, &why)) {
                    throw std::runtime_error("corrupt supervise/state in walk: " + why);
                }
            }
            const int bid = st.build.empty() ? 0 : idByHash[st.build];
            std::set<int> sh;
            for (const std::string& h : st.shunned) {
                if (idByHash.count(h)) {
                    sh.insert(idByHash[h]);
                }
            }
            std::ostringstream out;
            out << "[build |-> " << bid << ", crashes |-> " << st.crashes << ", shunned |-> " << tlaSet(sh)
                << ", halted |-> " << tlaBool(st.halted) << ", stopped |-> " << tlaBool(st.stopped) << "]";
            return out.str();
        };

        auto formatChildRecord = [&]() -> std::string {
            const char* sname = "down";
            if (simChild.state == ChildState::Serving) {
                sname = "serving";
            } else if (simChild.state == ChildState::Exited) {
                sname = "exited";
            }
            std::ostringstream out;
            out << "[b |-> " << simChild.b << ", state |-> \"" << sname << "\", code |-> " << simChild.code << "]";
            return out.str();
        };

        std::vector<std::string> records;
        auto emit = [&](const std::string& opFields) {
            int curId = 0;
            int prevId = 0;
            readCurPrev(&curId, &prevId);
            std::ostringstream rec;
            rec << "[" << opFields << ", pcs |-> <<\"" << a1.at << "\", \"" << a2.at << "\">>, supLock |-> "
                << currentSupLock() << ", hlock |-> " << currentHLock() << ", cur |-> " << curId << ", prev |-> "
                << prevId << ", disk |-> " << formatDiskRecord() << ", child |-> " << formatChildRecord()
                << ", stopped |-> " << tlaBool(svcStopped) << ", halted |-> " << tlaBool(svcHalted) << "]";
            records.push_back(rec.str());
        };

        emit("op |-> \"init\"");

        for (std::size_t step = 0; step < maxSteps; ++step) {
            int curId = 0;
            int prevId = 0;
            readCurPrev(&curId, &prevId);

            struct Choice {
                int weight;
                std::function<void()> run;
            };
            std::vector<Choice> choices;

            // 1. EnsureSup when both actors are idle and service is neither stopped nor halted.
            if (!a1.busy && !a2.busy && !svcStopped && !svcHalted) {
                Actor* pick = (below(2) == 0) ? &a1 : &a2;
                choices.push_back({10, [&, pick] {
                                       pick->clockMs = 0;
                                       startJob(*pick, [pick] {
                                           Result r;
                                           (void)pick->loop->run(r);
                                       });
                                       emit("op |-> \"ensure\", p |-> \"" + pick->name + "\"");
                                   }});
            }

            // 2. RaceSupStart on the idle actor when the other is active.
            for (int i = 0; i < 2; ++i) {
                Actor& idleA = *actors[i];
                Actor& otherA = *actors[1 - i];
                if (!idleA.busy && otherA.busy && !svcStopped && !svcHalted) {
                    const int w = (fault == Fault::NoSupLock) ? 40 : 3;
                    choices.push_back({w, [&idleA, &emit] {
                                           idleA.clockMs = 0;
                                           startJob(idleA, [&idleA] {
                                               Result r;
                                               (void)idleA.loop->run(r);
                                           });
                                           emit("op |-> \"race_start\", p |-> \"" + idleA.name + "\"");
                                       }});
                }
            }

            // 3. Step a parked supervisor actor.
            for (Actor* ap : actors) {
                if (!ap->busy || !ap->parked) {
                    continue;
                }
                const std::string pc = ap->at;
                if (pc == "watch") {
                    const bool canWatchStep =
                        (simChild.state == ChildState::Exited) ||
                        (currentHLock() == "\"none\"" && simChild.state == ChildState::Serving && simChild.b != curId);
                    if (!canWatchStep) {
                        continue;
                    }
                }
                if (pc == "lock") {
                    const bool lockHeldByOther =
                        (a1.loop->holdsLock() && ap != &a1) || (a2.loop->holdsLock() && ap != &a2);
                    if (lockHeldByOther && fault != Fault::NoSupLock) {
                        // Either stutter at lock or time out (`giveup`).
                        choices.push_back({6, [ap, &emit] {
                                               stepParked(*ap, 6'000);
                                               emit("op |-> \"giveup\", p |-> \"" + ap->name + "\"");
                                           }});
                        choices.push_back({2, [ap, &emit] {
                                               stepParked(*ap, 10);
                                               emit("op |-> \"lock\", p |-> \"" + ap->name + "\"");
                                           }});
                        continue;
                    }
                }
                if (pc == "rb_lock" && extHoLocked && fault != Fault::NoHandoverLock) {
                    // Keep retry clock well under handover::kLockWaitMs (60s) so it retries cleanly.
                    choices.push_back({2, [ap, &emit] {
                                           stepParked(*ap, 0);
                                           emit("op |-> \"rb_lock\", p |-> \"" + ap->name + "\"");
                                       }});
                    continue;
                }
                choices.push_back({16, [ap, pc, &emit] {
                                       stepParked(*ap, 0);
                                       emit("op |-> \"" + pc + "\", p |-> \"" + ap->name + "\"");
                                   }});
            }

            // 4. ChildLateCrash when a LateBroken build is serving.
            if (simChild.state == ChildState::Serving && lateBroken.count(simChild.b) > 0) {
                choices.push_back({18, [&] {
                                       simChild.state = ChildState::Exited;
                                       simChild.code = 1;
                                       emit("op |-> \"late_crash\"");
                                   }});
            }

            // 5. External handover begin / finish.
            if (extHoLocked) {
                choices.push_back({14, [&] {
                                       const int t = extHoTarget;
                                       if (startupBroken.count(t) == 0) {
                                           int cNow = 0;
                                           int pNow = 0;
                                           readCurPrev(&cNow, &pNow);
                                           if (cNow != t) {
                                               builds::Builds b(serverDir, {});
                                               builds::Result br;
                                               if (b.makeCurrent(hashById[t], br, fault == Fault::ServeUntested) !=
                                                   builds::kExitOk) {
                                                   throw std::runtime_error("handover makeCurrent failed");
                                               }
                                           }
                                           explicitPick = hashById[t];
                                           if (fault != Fault::InMemoryState) {
                                               (void)noteHandoverTarget(*envFs, superviseDir, hashById[t]);
                                           }
                                       }
                                       extHoHold.reset();
                                       extHoLocked = false;
                                       extHoTarget = 0;
                                       emit("op |-> \"ho_finish\"");
                                   }});
            } else if (currentHLock() == "\"none\"") {
                std::vector<int> targets;
                for (int t = 1; t <= 3; ++t) {
                    if (tested.count(t) > 0 || fault == Fault::ServeUntested) {
                        targets.push_back(t);
                    }
                }
                if (fault == Fault::ServeUntested) {
                    // Prefer untested build 3 so the fault triggers promptly.
                    targets = {3};
                }
                for (int t : targets) {
                    int w = 1;
                    if (svcStopped || svcHalted) {
                        w = 12;
                    } else if (fault == Fault::ServeUntested) {
                        w = 20;
                    } else if (fault == Fault::DrainStops && simChild.state == ChildState::Serving && t != simChild.b) {
                        w = 20;
                    } else if (prevId == 0 && t != curId) {
                        // Establish a `previous` build promptly so post-start rollback paths are exercised deeply.
                        w = 10;
                    } else if (a1.busy || a2.busy) {
                        // Avoid interrupting every crash/rollback sequence before `MaxCrashesBeforeRollback` /
                        // `MaxRestarts` is reached.
                        if (fault == Fault::NoHandoverLock || fault == Fault::NoPostStartRollback ||
                            fault == Fault::PingPongRollback || fault == Fault::NoHaltBound) {
                            continue;
                        }
                        w = (below(4) == 0) ? 1 : 0;
                    }
                    if (w <= 0) {
                        continue;
                    }
                    choices.push_back({w, [&, t] {
                                           if (!envFs->lock(handoverDir, nexus::store::LockMode::Exclusive, extHoHold)
                                                    .is_ok()) {
                                               throw std::runtime_error("cannot acquire external handover lock");
                                           }
                                           extHoLocked = true;
                                           extHoTarget = t;
                                           svcStopped = false;
                                           svcHalted = false;
                                           (void)noteHandoverBegin(*envFs, superviseDir);
                                           if (simChild.state == ChildState::Serving && simChild.b != t) {
                                               simChild.state = ChildState::Exited;
                                               simChild.code = kDrainExit;
                                           }
                                           emit("op |-> \"ho_begin\", t |-> " + std::to_string(t));
                                       }});
                }
            }

            // 6. Intentional UserStop when a build is serving and no handover lock is held.
            if (simChild.state == ChildState::Serving && currentHLock() == "\"none\"") {
                int w = 1;
                if (fault == Fault::StopRestarts) {
                    w = 25;
                } else if (fault == Fault::NoHandoverLock || fault == Fault::NoPostStartRollback ||
                           fault == Fault::PingPongRollback || fault == Fault::NoHaltBound) {
                    w = 0;
                }
                if (w > 0) {
                    choices.push_back({w, [&] {
                                           (void)noteIntentionalStop(*envFs, superviseDir);
                                           simChild.state = ChildState::Exited;
                                           simChild.code = 0;
                                           emit("op |-> \"user_stop\"");
                                       }});
                }
            }

            // 7. Supervisor process death (SIGKILL). Avoid killing right before the targeted fault acts.
            if (fault == Fault::None || fault == Fault::InMemoryState) {
                for (Actor* ap : actors) {
                    if (ap->busy && ap->parked) {
                        choices.push_back({2, [&, ap] {
                                               const bool heldSup = ap->loop->holdsLock();
                                               killParked(*ap);
                                               if (heldSup) {
                                                   simChild = SimChild{};
                                               }
                                               emit("op |-> \"sup_die\", p |-> \"" + ap->name + "\"");
                                           }});
                    }
                }
            }

            if (choices.empty()) {
                break;
            }
            std::uint64_t totalW = 0;
            for (const Choice& c : choices) {
                totalW += static_cast<std::uint64_t>(c.weight);
            }
            std::uint64_t pickW = below(totalW);
            for (const Choice& c : choices) {
                if (pickW < static_cast<std::uint64_t>(c.weight)) {
                    c.run();
                    break;
                }
                pickW -= static_cast<std::uint64_t>(c.weight);
            }
        }

        cleanupThreads();

        std::ofstream out(outFile, std::ios::binary);
        out << "---- MODULE SupervisionTraceData ----\n";
        out << "TTested == " << tlaSet(tested) << "\n";
        out << "TStartupBroken == " << tlaSet(startupBroken) << "\n";
        out << "TLateBroken == " << tlaSet(lateBroken) << "\n";
        out << "TInitial == 1\n";
        out << "Trace == <<\n";
        for (std::size_t i = 0; i < records.size(); ++i) {
            out << "  " << records[i] << (i + 1 < records.size() ? "," : "") << "\n";
        }
        out << ">>\n";
        out << "====\n";
        out.close();
        if (!out) {
            throw std::runtime_error("cannot write trace file " + outPath);
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "supervision_walk failed: %s\n", e.what());
        rc = 1;
    }

    removeAllSafe(work);
    return rc;
}
