// handover_walk: a seeded random walk of the real handover engine (src/server/handover.cpp) over real placed builds
// (`Builds`) and real POSIX files and OS locks (`PosixFs`), written as a trace for models/handover/HandoverTrace.tla.
//
// Two actors (a1, a2), each on its own thread with its own PosixFs and Handover instance, run `to(<build>)` and
// `resume()` against a real `<work>/builds` and `<work>/handover` directory. Every step parks at Options::hook (and on
// a busy lock retry) so the walk controls the interleaving, injects actor deaths (SIGKILL: unwinding closes the locked
// directory descriptor so the OS releases the flock) and machine crashes (all actors killed, simulated server down),
// and toggles open friend turns and drain deadlines on the simulated server. All read-only placed build folders under
// `<work>` are made writable and removed unconditionally before exit.
//
// Usage: handover_walk <seed> <steps> [--fault <no-lock|no-sync|in-memory-only|overwrite-prev|no-rollback|stop-cuts|
//                                               allow-untested|serve-broken>] <out.tla>

#include "server/builds.h"
#include "server/handover.h"
#include "store/fs.h"

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
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/file.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;
using namespace nexus::server;
using namespace nexus::server::handover;

namespace {

struct Killed {};

constexpr int kLegacyBuild = 9;

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

// Symlink-safe recursive removal that restores write bits on 0555/0444 placed build trees before unlinking.
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

struct SimServer {
    int b = 0;  // 0 (down), 1..3, or kLegacyBuild (9)
    bool draining = false;
    bool turn = false;
    bool late = false;
};

class WalkServerPort final : public ServerPort {
public:
    SimServer& srv;
    const std::map<std::string, int>& idByHash;
    const std::map<int, std::string>& hashById;
    const std::set<int>& broken;
    Fault fault;

    WalkServerPort(SimServer& s, const std::map<std::string, int>& toId, const std::map<int, std::string>& toHash,
                   const std::set<int>& brk, Fault f)
        : srv(s), idByHash(toId), hashById(toHash), broken(brk), fault(f) {}

    nexus::store::Status probe(ServerSeen* out) override {
        *out = ServerSeen{};
        if (srv.b == 0) {
            return nexus::store::Status::ok();
        }
        out->serving = true;
        if (srv.b == kLegacyBuild) {
            out->legacy = true;
            return nexus::store::Status::ok();
        }
        const auto it = hashById.find(srv.b);
        out->build = (it == hashById.end()) ? std::string() : it->second;
        out->draining = srv.draining;
        return nexus::store::Status::ok();
    }

    DrainReply requestDrain(std::string*) override {
        if (srv.b == 0) {
            return DrainReply::Closed;
        }
        if (srv.b == kLegacyBuild) {
            return DrainReply::UnknownRequest;
        }
        srv.draining = true;
        return DrainReply::Accepted;
    }

    nexus::store::Status stop() override {
        srv = SimServer{};
        return nexus::store::Status::ok();
    }

    StartReply stepStart(const std::string& expectedBuild, std::uint64_t, std::uint64_t, std::string* why) override {
        const auto it = idByHash.find(expectedBuild);
        const int id = (it == idByHash.end()) ? 0 : it->second;
        if (id == 0 || (broken.count(id) > 0 && fault != Fault::ServeBroken)) {
            *why = "build failed to reach SERVING";
            return StartReply::Failed;
        }
        srv.b = id;
        srv.draining = false;
        srv.turn = false;
        srv.late = false;
        return StartReply::Waiting;
    }

    void keepStarted() override {}
    void abortStarted() override {}
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
    std::unique_ptr<PlacementPort> placement;
    std::unique_ptr<ServerPort> server;
    std::unique_ptr<Handover> handover;
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
        throw Killed{};
    }
}

void actorMain(Actor& a) {
    for (;;) {
        std::function<void()> job;
        {
            std::unique_lock<std::mutex> lk(gm);
            gcv.wait(lk, [&] { return a.hasJob || a.quit; });
            if (!a.hasJob) {
                return;
            }
            job = std::move(a.job);
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
        ++a.events;
        gcv.notify_all();
    }
}

void advanceActor(Actor& a, bool start, std::function<void()> job = {}) {
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
    advanceActor(a, false);
}

std::string setLiteral(const std::set<int>& s) {
    std::string out = "{";
    for (int x : s) {
        if (out.size() > 1) {
            out += ", ";
        }
        out += std::to_string(x);
    }
    out += "}";
    return out;
}

std::string boolLiteral(bool v) {
    return v ? "TRUE" : "FALSE";
}

Fault parseFault(const std::string& name) {
    if (name == "no-lock") {
        return Fault::NoLock;
    }
    if (name == "no-sync") {
        return Fault::NoSync;
    }
    if (name == "in-memory-only") {
        return Fault::InMemoryOnly;
    }
    if (name == "overwrite-prev") {
        return Fault::OverwritePrev;
    }
    if (name == "no-rollback") {
        return Fault::NoRollback;
    }
    if (name == "stop-cuts") {
        return Fault::StopCuts;
    }
    if (name == "allow-untested") {
        return Fault::AllowUntested;
    }
    if (name == "serve-broken") {
        return Fault::ServeBroken;
    }
    throw std::runtime_error("unknown fault `" + name + "`");
}

int runWalk(std::uint64_t seed, int maxSteps, Fault fault, const fs::path& outPath, const fs::path& work) {
    gRng = seed;
    const std::string serverDir = (work / "server").string();
    const std::string buildsDir = serverDir + "/builds";
    const std::string handoverDir = serverDir + "/handover";
    fs::create_directories(handoverDir);
    fs::permissions(work, fs::perms(0700));
    fs::permissions(serverDir, fs::perms(0700));
    fs::permissions(handoverDir, fs::perms(0700));

    // Scenario per seed:
    //   seed 1 (and allow-untested): Tested = {1, 2}, Broken = {2}, 3 is untested; StartSrv = 1
    //   seed 3:                      Tested = {1, 2, 3}, Broken = {3}, StartSrv = LegacyBuild (9)
    //   other seeds:                 Tested = {1, 2, 3}, Broken = {3}, StartSrv = 1
    std::set<int> tested = (seed == 1 || fault == Fault::AllowUntested) ? std::set<int>{1, 2} : std::set<int>{1, 2, 3};
    std::set<int> broken = (seed == 1 || fault == Fault::AllowUntested) ? std::set<int>{2} : std::set<int>{3};
    const int initial = 1;
    const int startSrv = (seed == 3 && fault == Fault::None) ? kLegacyBuild : initial;

    std::map<int, std::string> hashOf;
    std::map<std::string, int> idOf;
    {
        builds::Builds setup(serverDir, {});
        const std::string formats = builds::thisBuildFormats();
        for (int b = 1; b <= 3; ++b) {
            const fs::path src = work / ("src-" + std::to_string(b));
            writeTreeFile(src / "bin/nexus-server", "#!/bin/sh\n# build " + std::to_string(b) + "\nexit 0\n", true);
            writeTreeFile(src / "drivers/claude/main.js", "// driver " + std::to_string(b) + "\n", false);
            writeTreeFile(src / "formats", formats, false);
            std::string h;
            const nexus::store::Status hs = builds::hashTree(src.string(), &h);
            if (!hs.is_ok()) {
                throw std::runtime_error("hashTree failed: " + hs.message());
            }
            std::string evPath;
            if (tested.count(b) > 0) {
                const fs::path ep = work / ("ev-" + std::to_string(b));
                writeTreeFile(ep, builds::evidenceText({h, "pass", "handover_walk"}), false);
                evPath = ep.string();
            }
            builds::Result r;
            if (setup.place(src.string(), evPath, r) != builds::kExitOk) {
                throw std::runtime_error("Builds::place failed for build " + std::to_string(b));
            }
            hashOf[b] = h;
            idOf[h] = b;
        }
        builds::Result r;
        if (setup.makeCurrent(hashOf[initial], r) != builds::kExitOk) {
            throw std::runtime_error("Builds::makeCurrent(initial) failed");
        }
    }

    SimServer srv;
    srv.b = startSrv;

    Actor actors[2];
    actors[0].name = "a1";
    actors[1].name = "a2";
    for (Actor& a : actors) {
        nexus::store::PosixOptions fsOpts;
        fsOpts.now_ms = [&a] { return a.clockMs; };
        fsOpts.sleep_ms = [&a](std::uint64_t ms) { a.clockMs += ms; };
        a.fs = nexus::store::make_posix_fs(fsOpts);
        a.placement = makeBuildsPlacement(serverDir, {});
        a.server = std::make_unique<WalkServerPort>(srv, idOf, hashOf, broken, fault);

        Options hOpts;
        hOpts.fault = fault;
        hOpts.lock_wait_ms = 30;
        hOpts.start_wait_ms = 40;
        hOpts.drain_wait_ms = 100000;
        hOpts.poll_ms = 10;
        hOpts.now_ms = [&a] { return a.clockMs; };
        hOpts.sleep_ms = [&a](std::uint64_t ms) {
            a.clockMs += ms;
            if (a.handover->phase() == "lock") {
                park(a, "lock");
            }
        };
        hOpts.hook = [&a](const std::string& step) { park(a, step); };
        a.handover = std::make_unique<Handover>(*a.fs, *a.placement, *a.server, handoverDir, hOpts);
        a.thread = std::thread(actorMain, std::ref(a));
    }

    struct StopThreads {
        Actor (&actors)[2];
        ~StopThreads() {
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
        }
    } threadGuard{actors};

    auto lockHolder = [&]() -> std::string {
        const int fd = ::open(handoverDir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (fd < 0) {
            return "none";
        }
        const bool free = (::flock(fd, LOCK_EX | LOCK_NB) == 0);
        if (free) {
            ::flock(fd, LOCK_UN);
        }
        ::close(fd);
        if (free) {
            return "none";
        }
        for (const Actor& a : actors) {
            if (a.busy && a.handover->holdsLock()) {
                return a.name;
            }
        }
        return "?";
    };

    std::vector<std::string> records;
    auto record = [&](const std::string& op, const std::string& p = "", const std::string& v = "", int t = 0) {
        int cur = 0;
        int prev = 0;
        std::error_code ec;
        const fs::path active = fs::path(buildsDir) / "active";
        if (fs::is_symlink(fs::symlink_status(active, ec))) {
            std::string c;
            std::string pv;
            const std::string pair = fs::read_symlink(active, ec).string();
            if (builds::readPairName(pair, &c, &pv)) {
                cur = idOf.count(c) ? idOf[c] : 0;
                prev = (!pv.empty() && idOf.count(pv)) ? idOf[pv] : 0;
            }
        }

        bool hasState = false;
        int stTgt = 0;
        int stPrev = 0;
        std::string stStage = "switching";
        int left = 0;
        for (fs::directory_iterator it(handoverDir, ec), end; !ec && it != end; it.increment(ec)) {
            const std::string n = it->path().filename().string();
            if (n.rfind(kStateTmpPrefix, 0) == 0) {
                ++left;
            } else if (n == kStateFile) {
                std::ifstream in(it->path(), std::ios::binary);
                std::ostringstream ss;
                ss << in.rdbuf();
                StateRecord rec;
                std::string why;
                if (readState(ss.str(), &rec, &why)) {
                    hasState = true;
                    stTgt = idOf.count(rec.target) ? idOf[rec.target] : 0;
                    stPrev = (!rec.previous.empty() && idOf.count(rec.previous)) ? idOf[rec.previous] : 0;
                    stStage = stageName(rec.stage);
                }
            }
        }

        const std::string pcs = "<<\"" + (actors[0].busy ? actors[0].at : std::string("idle")) + "\", \"" +
                                (actors[1].busy ? actors[1].at : std::string("idle")) + "\">>";
        const std::string srvRec = "[b |-> " + std::to_string(srv.b) + ", draining |-> " + boolLiteral(srv.draining) +
                                   ", turn |-> " + boolLiteral(srv.turn) + ", late |-> " + boolLiteral(srv.late) + "]";

        std::ostringstream line;
        line << "[op |-> \"" << op << "\""
             << ", p |-> \"" << p << "\""
             << ", v |-> \"" << v << "\""
             << ", t |-> " << t
             << ", pcs |-> " << pcs
             << ", lock |-> \"" << lockHolder() << "\""
             << ", cur |-> " << cur
             << ", prev |-> " << prev
             << ", hasState |-> " << boolLiteral(hasState)
             << ", stTgt |-> " << stTgt
             << ", stPrev |-> " << stPrev
             << ", stStage |-> \"" << stStage << "\""
             << ", left |-> " << left
             << ", srv |-> " << srvRec << "]";
        records.push_back(line.str());
    };

    record("init");

    for (int step = 0; step < maxSteps; ++step) {
        // Environment actions on the simulated server (open/close turns, drain deadline, drain exit).
        const std::uint64_t draw = below(100);
        if (srv.b != 0 && srv.draining && (!srv.turn || srv.late) && draw < 35) {
            srv = SimServer{};
            record("srv_exit");
            continue;
        }
        if (srv.b != 0 && srv.draining && !srv.late && draw < 50) {
            srv.late = true;
            record("drain_late");
            continue;
        }
        if (srv.b != 0 && !srv.turn && draw < 12) {
            srv.turn = true;
            record("turn_open");
            continue;
        }
        if (srv.turn && draw >= 12 && draw < 24) {
            srv.turn = false;
            record("turn_end");
            continue;
        }
        if ((actors[0].busy || actors[1].busy) && draw >= 96) {
            for (Actor& a : actors) {
                killActor(a);
            }
            srv = SimServer{};
            record("crash");
            continue;
        }

        const int idx = static_cast<int>(below(2));
        Actor& a = actors[idx];
        if (!a.busy) {
            const bool resumeVerb = (below(4) == 0);
            if (resumeVerb) {
                a.clockMs = 0;
                advanceActor(a, true, [&a] {
                    Result r;
                    (void)a.handover->resume(r);
                });
                record("start", a.name, "resume", 0);
            } else {
                const int target = static_cast<int>(below(3)) + 1;
                const std::string h = hashOf[target];
                a.clockMs = 0;
                advanceActor(a, true, [&a, h] {
                    Result r;
                    (void)a.handover->to(h, r);
                });
                record("start", a.name, "to", target);
            }
            continue;
        }

        if (a.at != "lock" && below(18) == 0) {
            killActor(a);
            record("die", a.name);
            continue;
        }

        const std::string before = a.at;
        advanceActor(a, false);
        const std::string after = a.busy ? a.at : std::string("idle");
        std::string op = before;
        if (before == "lock" && after == "idle") {
            op = "giveup";
        } else if (before == "start") {
            op = "start_srv";
        }
        record(op, a.name);
    }

    std::ofstream out(outPath, std::ios::binary);
    out << "---- MODULE HandoverTraceData ----\n"
        << "TTested == " << setLiteral(tested) << "\n"
        << "TBroken == " << setLiteral(broken) << "\n"
        << "TInitial == " << initial << "\n"
        << "TStartSrv == " << startSrv << "\n"
        << "Trace == <<\n";
    for (std::size_t i = 0; i < records.size(); ++i) {
        out << "    " << records[i] << (i + 1 < records.size() ? "," : "") << "\n";
    }
    out << ">>\n====\n";
    out.close();
    if (!out) {
        throw std::runtime_error("cannot write trace " + outPath.string());
    }
    std::printf("handover_walk seed %llu: %zu records written to %s\n", static_cast<unsigned long long>(seed),
                records.size(), outPath.string().c_str());
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 4 && argc != 6) {
        std::fprintf(stderr, "usage: handover_walk <seed> <steps> [--fault <name>] <out.tla>\n");
        return 2;
    }
    const std::uint64_t seed = std::strtoull(argv[1], nullptr, 10);
    const int steps = std::atoi(argv[2]);
    Fault fault = Fault::None;
    std::string outArg;
    if (argc == 4) {
        outArg = argv[3];
    } else {
        if (std::strcmp(argv[3], "--fault") != 0) {
            std::fprintf(stderr, "usage: handover_walk <seed> <steps> [--fault <name>] <out.tla>\n");
            return 2;
        }
        try {
            fault = parseFault(argv[4]);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "handover_walk: %s\n", e.what());
            return 2;
        }
        outArg = argv[5];
    }

    const fs::path outPath = fs::absolute(outArg);
    if (outPath.has_parent_path()) {
        fs::create_directories(outPath.parent_path());
    }
    const fs::path work = outPath.parent_path() / ("walk-files-" + std::to_string(::getpid()));
    removeAllSafe(work);
    struct CleanWork {
        fs::path path;
        ~CleanWork() { removeAllSafe(path); }
    } cleanGuard{work};

    try {
        return runWalk(seed, steps, fault, outPath, work);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "handover_walk: %s\n", e.what());
        return 1;
    }
}
