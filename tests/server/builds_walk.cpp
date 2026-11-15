// builds_walk: a random walk of the real placement code (src/server/builds.cpp) on real files, written as a trace
// for models/placement/PlacementTrace.tla to check. Functional, not unit: real files in a directory of the test's own
// (beside the trace it writes), real locks, threads; no time (the lock wait's clock stands still and its pause is a
// step of the walk), no sockets, no network.
//
// Two actors (a1, a2), each a thread with its own Builds, run verbs (place, current, back, prune --keep 0); every step
// of the code stops at its hook (Options::hook, and the lock wait's pause) until the walk lets that actor take its next
// step, so the walk chooses the interleaving. An actor may be killed between two steps (an exception from its hook:
// its descriptors close, so the OS releases its locks, as on kill -9), and the machine may crash (every actor killed,
// the server stopped; on real files a crash keeps everything written, one of the outcomes the model allows). One
// server, s1, resolves a placed build's program path, then holds it (holdBuild) and stops.
//
// After each step the walk records what the disk and the processes show (PlacementTrace.tla says what each field is).
//
// Usage: builds_walk <seed> <steps> [--fault <no-lock|no-sync|tested-program-only|reuse-tmp|prune-in-use|two-renames>]
//                    <out.tla>

#include "server/builds.h"

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
#include <string>
#include <sys/file.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;
using namespace nexus::server::builds;

namespace {

struct Killed {};

std::uint64_t rngState = 0;
std::uint64_t next() {
    std::uint64_t z = (rngState += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}
std::uint64_t below(std::uint64_t n) {
    return next() % n;
}

[[noreturn]] void die(const std::string& why) {
    std::fprintf(stderr, "builds_walk: %s\n", why.c_str());
    std::exit(1);
}

std::mutex gm;
std::condition_variable gcv;

struct Actor {
    std::string name;
    bool busy = false;          // running a verb
    bool parked = false;        // stopped at a hook
    bool go = false;
    bool kill = false;
    bool quit = false;
    std::uint64_t events = 0;   // each park and each end of a verb
    std::string at = "idle";
    std::function<void()> job;
    bool hasJob = false;
    std::unique_ptr<Builds> builds;
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
        ++a.events;
        gcv.notify_all();
    }
}

// Lets the actor take one step (or start its job), and waits until it has stopped at its next hook or ended.
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

void writeFile(const fs::path& p, const std::string& text, bool exec) {
    fs::create_directories(p.parent_path());
    std::ofstream out(p, std::ios::binary);
    out << text;
    out.close();
    if (!out) {
        die("cannot write " + p.string());
    }
    fs::permissions(p, exec ? fs::perms(0755) : fs::perms(0644));
}

void removeAll(const fs::path& p) {
    std::error_code ec;
    if (!fs::exists(fs::symlink_status(p, ec))) {
        return;
    }
    for (auto it = fs::recursive_directory_iterator(p, ec); !ec && it != fs::recursive_directory_iterator();
         it.increment(ec)) {
        if (it->is_directory(ec) && !it->is_symlink(ec)) {
            fs::permissions(it->path(), fs::perms::owner_all, fs::perm_options::add, ec);
        }
    }
    fs::permissions(p, fs::perms::owner_all, fs::perm_options::add, ec);
    fs::remove_all(p, ec);
    if (ec) {
        die("cannot remove " + p.string() + ": " + ec.message());
    }
}

std::string readAll(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

int prog(int t) {
    return t == 2 ? 2 : 1;
}
int drv(int t) {
    return t == 1 ? 1 : 2;
}

std::string setText(const std::set<int>& s) {
    std::string out = "{";
    for (int x : s) {
        out += (out.size() > 1 ? ", " : "") + std::to_string(x);
    }
    return out + "}";
}

struct Walk {
    fs::path work;
    std::string serverDir;
    std::string buildsDir;
    Fault fault = Fault::None;
    int initial = 1;
    std::set<int> others{2};
    std::set<int> tested{1, 2};
    std::map<int, std::string> hashOf;
    Actor actors[2];
    // The server: down, resolved (its program's path found) or up (holding its build).
    std::string srv = "down";
    std::string srvProgram;
    BuildHold hold;
    std::vector<std::string> records;
    std::size_t folders = 5;

    fs::path src(int t) const { return work / "src" / std::to_string(t); }
    fs::path evidence(int t) const { return work / "ev" / std::to_string(t); }

    // The tree a build's name is: by its hash, and where two trees share one (a hash that leaves part out), by what
    // its folder holds.
    int treeOf(const std::string& hash) const {
        std::vector<int> c;
        for (const auto& [t, h] : hashOf) {
            if (h == hash) {
                c.push_back(t);
            }
        }
        if (c.size() == 1) {
            return c[0];
        }
        const fs::path f = fs::path(buildsDir) / hash;
        for (int t : c) {
            if (readAll(f / "bin" / "nexus-server") == readAll(src(t) / "bin" / "nexus-server") &&
                readAll(f / "drivers" / "claude" / "main.js") == readAll(src(t) / "drivers" / "claude" / "main.js")) {
                return t;
            }
        }
        return 0;
    }

    std::string lockHolder() const {
        const int fd = ::open(buildsDir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (fd < 0) {
            die("cannot open " + buildsDir);
        }
        const bool free = ::flock(fd, LOCK_EX | LOCK_NB) == 0;
        ::close(fd);
        if (free) {
            return "none";
        }
        for (const Actor& a : actors) {
            if (a.builds->holdsLock()) {
                return a.name;
            }
        }
        return "?";
    }

    void record(const std::string& op, const std::string& p, const std::string& v = "", int t = 0, int e = 0) {
        int cur = 0;
        int prev = 0;
        std::error_code ec;
        const fs::path active = fs::path(buildsDir) / "active";
        std::string pair;
        if (fs::is_symlink(fs::symlink_status(active, ec))) {
            std::string c;
            std::string pv;
            pair = fs::read_symlink(active, ec).string();
            if (readPairName(pair, &c, &pv)) {
                cur = treeOf(c);
                prev = pv.empty() ? 0 : treeOf(pv);
            }
        }
        std::set<int> named;
        std::set<int> whole;
        std::set<int> marks;
        int left = 0;
        for (const auto& entry : fs::directory_iterator(buildsDir)) {
            const std::string n = entry.path().filename().string();
            const NameKind k = classify(n);
            if (k == NameKind::Tmp || k == NameKind::PairTmp || k == NameKind::ActiveTmp || k == NameKind::Gone ||
                (k == NameKind::Pair && n != pair)) {
                ++left;
            }
            if (k != NameKind::Build) {
                continue;
            }
            const int tree = treeOf(n);
            if (tree == 0) {
                continue;
            }
            named.insert(tree);
            Manifest m;
            if (checkBuild(buildsDir, n, &m, fault).is_ok()) {
                whole.insert(tree);
            }
            std::ifstream in(entry.path() / kManifest);
            std::string line;
            while (std::getline(in, line)) {
                if (line.rfind("tested ", 0) == 0) {
                    marks.insert(tree);
                }
            }
        }
        std::string pcs = "<<";
        for (const Actor& a : actors) {
            pcs += std::string(pcs.size() > 2 ? ", " : "") + "\"" + (a.busy ? a.at : std::string("idle")) + "\"";
        }
        pcs += ">>";
        records.push_back("[op |-> \"" + op + "\", p |-> \"" + p + "\", v |-> \"" + v + "\", t |-> " +
                          std::to_string(t) + ", e |-> " + std::to_string(e) + ", pcs |-> " + pcs + ", lock |-> \"" +
                          lockHolder() + "\", cur |-> " + std::to_string(cur) + ", prev |-> " + std::to_string(prev) +
                          ", named |-> " + setText(named) + ", whole |-> " + setText(whole) + ", marks |-> " +
                          setText(marks) + ", left |-> " + std::to_string(left) + ", srv |-> \"" + srv + "\"]");
    }

    void setup() {
        removeAll(work);
        serverDir = (work / "server").string();
        for (int t = 1; t <= 3; ++t) {
            writeFile(src(t) / "bin" / "nexus-server", "#!/bin/sh\necho program " + std::to_string(prog(t)) + "\n",
                      true);
            writeFile(src(t) / "drivers" / "claude" / "main.js", "driver " + std::to_string(drv(t)) + "\n", false);
            fs::create_symlink("claude/main.js", src(t) / "drivers" / "main.js");
            writeFile(src(t) / "formats", thisBuildFormats(), false);
            std::string h;
            const auto s = hashTree(src(t).string(), &h, fault);
            if (!s.is_ok()) {
                die(s.message());
            }
            hashOf[t] = h;
        }
        for (int t : tested) {
            writeFile(evidence(t), evidenceText(Evidence{hashOf[t], "pass", "builds_walk"}), false);
        }
        Options plain;
        plain.fault = fault;
        Builds b(serverDir, plain);
        Result r;
        if (b.place(src(initial).string(), evidence(initial).string(), r) != kExitOk ||
            b.makeCurrent(hashOf[initial], r) != kExitOk) {
            die("setup: " + (r.err.empty() ? std::string("?") : r.err.back()));
        }
        for (int t : others) {
            if (b.place(src(t).string(), tested.count(t) != 0 ? evidence(t).string() : std::string(), r) != kExitOk) {
                die("setup: " + (r.err.empty() ? std::string("?") : r.err.back()));
            }
        }
        char* real = ::realpath(serverDir.c_str(), nullptr);
        if (real == nullptr) {
            die("realpath " + serverDir);
        }
        serverDir = real;
        std::free(real);
        buildsDir = serverDir + "/builds";
        for (int i = 0; i < 2; ++i) {
            Actor& a = actors[i];
            a.name = i == 0 ? "a1" : "a2";
            Options o;
            o.fault = fault;
            o.hook = [&a](const std::string& step) { park(a, step); };
            o.sleep_ms = [&a](std::uint64_t) { park(a, "lock"); };
            o.now_ms = [] { return std::uint64_t(1000); };
            a.builds = std::make_unique<Builds>(serverDir, o);
            a.thread = std::thread([&a] { actorLoop(a); });
        }
    }

    void start(Actor& a) {
        const std::uint64_t pick = below(100);
        std::string v;
        int t = 0;
        int e = 0;
        std::function<void()> job;
        Builds* b = a.builds.get();
        if (pick < 40) {
            v = "place";
            t = int(below(3)) + 1;
            const std::uint64_t ec = below(4);
            if (ec == 0) {
                e = 0;
            } else if (ec == 1) {
                std::vector<int> ts(tested.begin(), tested.end());
                e = ts[below(ts.size())];
            } else {
                e = tested.count(t) != 0 ? t : 0;
            }
            if (fault == Fault::TestedProgramOnly && below(2) == 0) {
                t = 3;
                e = 1;
            }
            const std::string tree = src(t).string();
            const std::string evf = e == 0 ? std::string() : evidence(e).string();
            job = [b, tree, evf] {
                Result r;
                b->place(tree, evf, r);
            };
        } else if (pick < 70) {
            v = "switch";
            t = int(below(3)) + 1;
            const std::string h = hashOf[t];
            job = [b, h] {
                Result r;
                b->makeCurrent(h, r);
            };
        } else if (pick < 80) {
            v = "back";
            job = [b] {
                Result r;
                b->back(r);
            };
        } else {
            v = "prune";
            job = [b] {
                Result r;
                b->prune(0, r);
            };
        }
        advance(a, true, job);
        record("start", a.name, v, t, e);
    }

    void step(Actor& a) {
        const std::string op = a.at;
        if (op == "mktmp" || op == "pmk") {
            ++folders;
        }
        advance(a, false);
        record(op, a.name);
    }

    void crash() {
        for (Actor& a : actors) {
            killActor(a);
        }
        hold.release();
        srv = "down";
        record("crash", "none");
    }

    void server() {
        if (srv == "down") {
            std::vector<int> named;
            for (const auto& entry : fs::directory_iterator(buildsDir)) {
                const std::string n = entry.path().filename().string();
                if (classify(n) == NameKind::Build && treeOf(n) != 0) {
                    named.push_back(treeOf(n));
                }
            }
            if (named.empty()) {
                return;
            }
            std::sort(named.begin(), named.end());   // the same choice whatever order the directory lists them in
            const int t = named[below(named.size())];
            srvProgram = buildsDir + "/" + hashOf[t] + "/" + kProgram;
            srv = "resolved";
            record("resolve", "s1", "", t, 0);
        } else if (srv == "resolved") {
            const auto s = holdBuild(srvProgram, hold, [] { return std::uint64_t(1000); }, [](std::uint64_t) {}, 0);
            srv = s.is_ok() && !hold.hash().empty() ? "up" : "down";
            record("hold", "s1");
        } else {
            hold.release();
            srv = "down";
            record("stop", "s1");
        }
    }

    void run(std::size_t steps) {
        record("init", "none");
        while (records.size() < steps) {
            // The choices and their weights: start a verb, take a step, kill an actor, crash, the server's next move.
            std::vector<std::pair<int, std::function<void()>>> c;
            // A wait on a lock another actor holds is a step that changes nothing: rarer, so that verbs get through.
            for (Actor& a : actors) {
                if (!a.busy) {
                    c.push_back({4, [this, &a] { start(a); }});
                } else {
                    const bool waiting = a.at == "lock" && lockHolder() != "none";
                    c.push_back({waiting ? 2 : 24, [this, &a] { step(a); }});
                    c.push_back({1, [this, &a] {
                                     killActor(a);
                                     record("die", a.name);
                                 }});
                }
            }
            c.push_back({1, [this] { crash(); }});
            c.push_back({srv == "resolved" ? 3 : 1, [this] { server(); }});
            int total = 0;
            for (const auto& x : c) {
                total += x.first;
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
        for (Actor& a : actors) {
            killActor(a);
            {
                std::unique_lock<std::mutex> lk(gm);
                a.quit = true;
            }
            gcv.notify_all();
            a.thread.join();
        }
        hold.release();
    }

    void write(const std::string& out) const {
        std::ofstream f(out);
        f << "---- MODULE PlacementTraceData ----\n";
        f << "\\* Written by tests/server/builds_walk.cpp; checked by PlacementTrace.tla.\n";
        f << "TInitial == " << initial << "\n";
        f << "TOthers == " << setText(others) << "\n";
        f << "TTested == " << setText(tested) << "\n";
        f << "TFolders == " << folders << "\n";
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
        die("usage: builds_walk <seed> <steps> [--fault <name>] <out.tla>");
    }
    static const std::map<std::string, Fault> faults{
        {"", Fault::None}, {"no-lock", Fault::NoLock}, {"no-sync", Fault::NoSync},
        {"tested-program-only", Fault::TestedProgramOnly}, {"reuse-tmp", Fault::ReuseTmp},
        {"prune-in-use", Fault::PruneInUse}, {"two-renames", Fault::TwoRenames}};
    const auto f = faults.find(faultName);
    if (f == faults.end()) {
        die("no fault `" + faultName + "`");
    }
    static Walk w;
    w.fault = f->second;
    if (w.fault == Fault::TestedProgramOnly) {
        // Tree 1 is tested and never placed; tree 3 has its program and another driver tree.
        w.initial = 2;
        w.others = {};
    }
    rngState = std::strtoull(plain[0].c_str(), nullptr, 10);
    const std::size_t steps = std::strtoull(plain[1].c_str(), nullptr, 10);
    w.work = fs::path(plain[2]).parent_path() / "walk-files";
    w.setup();
    w.run(steps);
    w.write(plain[2]);
    std::printf("builds_walk: seed %s, %zu steps, %zu folder ids, fault %s, written to %s\n", plain[0].c_str(),
                w.records.size(), w.folders, faultName.empty() ? "none" : faultName.c_str(), plain[2].c_str());
    return 0;
}
