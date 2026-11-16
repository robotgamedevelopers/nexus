// Functional tests of placement (src/server/builds.h) on real files: every case runs in a directory of its own (the
// second argument), which stands in for the server's directory, never ~/.nexus. Crashes are real: a child process runs
// a verb and is killed (_exit from its hook, between two steps), and a fresh run follows. Two runs at once are two
// child processes. The lock's wait uses the real clock here, as a real run's does.
//
// Usage: builds_functional <case> <dir>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <functional>
#include <set>
#include <sstream>
#include <string>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include "server/builds.h"

namespace fs = std::filesystem;
using namespace nexus::server::builds;

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

std::string joined(const std::vector<std::string>& lines) {
    std::string out;
    for (const std::string& l : lines) {
        out += l + "\n";
    }
    return out;
}

fs::path root;
std::string serverDir;
std::string buildsDir;

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

void writeFile(const fs::path& p, const std::string& text, bool exec = false) {
    fs::create_directories(p.parent_path());
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out << text;
    out.close();
    if (!out) {
        std::fprintf(stderr, "cannot write %s\n", p.c_str());
        removeAll(root);
        std::exit(1);
    }
    fs::permissions(p, exec ? fs::perms(0755) : fs::perms(0644));
}

std::string readAll(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// A tree as a build of nexus-server is assembled: the program, a driver tree with a link, the formats file.
fs::path makeTree(const std::string& name, const std::string& program, const std::string& driver,
                  const std::string& formats = thisBuildFormats()) {
    const fs::path t = root / "src" / name;
    removeAll(t);
    writeFile(t / "bin" / "nexus-server", "#!/bin/sh\necho " + program + "\n", true);
    writeFile(t / "drivers" / "claude" / "main.js", driver + "\n");
    writeFile(t / "drivers" / "claude" / "package.json", "{}\n");
    fs::create_symlink("claude/main.js", t / "drivers" / "main.js");
    writeFile(t / "formats", formats);
    return t;
}

std::string hashOf(const fs::path& tree) {
    std::string h;
    const auto s = hashTree(tree.string(), &h);
    CHECK(s.is_ok());
    return h;
}

fs::path evidenceFor(const fs::path& tree, const std::string& name) {
    const fs::path e = root / "ev" / name;
    writeFile(e, evidenceText(Evidence{hashOf(tree), "pass", "builds_functional"}));
    return e;
}

Options plain() {
    return Options{};
}

struct Out {
    int exit = 0;
    std::string out;
    std::string err;
};

Out run(const std::vector<std::string>& args, Options o = plain()) {
    Result r;
    Out x;
    x.exit = command(serverDir, args, o, r);
    x.out = joined(r.out);
    x.err = joined(r.err);
    return x;
}

// The steps a verb takes (its hook's calls, in order), run once with nothing killed.
std::vector<std::string> steps(const std::vector<std::string>& args) {
    std::vector<std::string> seen;
    Options o;
    o.hook = [&](const std::string& s) { seen.push_back(s); };
    run(args, o);
    return seen;
}

// The verb run in a child process that is killed at its hook's call number `at` (0-based): the steps before it are
// done, the one there is not. Returns the child's wait status.
int killedAt(const std::vector<std::string>& args, std::size_t at) {
    std::fflush(stdout);
    std::fflush(stderr);
    const pid_t pid = ::fork();
    if (pid == 0) {
        std::size_t n = 0;
        Options o;
        o.hook = [&](const std::string&) {
            if (n++ == at) {
                ::_exit(9);
            }
        };
        Result r;
        command(serverDir, args, o, r);
        ::_exit(r.exit);
    }
    int status = 0;
    ::waitpid(pid, &status, 0);
    return status;
}

// Two verbs at once, in two child processes; their exit statuses.
std::pair<int, int> together(const std::vector<std::string>& a, const std::vector<std::string>& b) {
    std::fflush(stdout);
    std::fflush(stderr);
    pid_t pids[2];
    const std::vector<std::string>* args[2] = {&a, &b};
    for (int i = 0; i < 2; ++i) {
        pids[i] = ::fork();
        if (pids[i] == 0) {
            Result r;
            command(serverDir, *args[i], plain(), r);
            for (const std::string& e : r.err) {
                std::fprintf(stderr, "child %d: %s\n", i, e.c_str());
            }
            ::_exit(r.exit);
        }
    }
    int st[2] = {0, 0};
    for (int i = 0; i < 2; ++i) {
        ::waitpid(pids[i], &st[i], 0);
    }
    return {WIFEXITED(st[0]) ? WEXITSTATUS(st[0]) : -1, WIFEXITED(st[1]) ? WEXITSTATUS(st[1]) : -1};
}

// The pointer as the disk has it: the current and previous builds ("" none).
std::pair<std::string, std::string> pointer() {
    std::error_code ec;
    const fs::path active = fs::path(buildsDir) / "active";
    if (!fs::is_symlink(fs::symlink_status(active, ec))) {
        return {};
    }
    std::string c;
    std::string p;
    CHECK(readPairName(fs::read_symlink(active).string(), &c, &p));
    return {c, p};
}

std::set<std::string> leftovers() {
    std::set<std::string> out;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(buildsDir, ec)) {
        const std::string n = e.path().filename().string();
        const NameKind k = classify(n);
        if (k == NameKind::Tmp || k == NameKind::PairTmp || k == NameKind::ActiveTmp || k == NameKind::Gone) {
            out.insert(n);
        }
    }
    return out;
}

std::set<std::string> placedBuilds() {
    std::set<std::string> out;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(buildsDir, ec)) {
        const std::string n = e.path().filename().string();
        if (classify(n) == NameKind::Build) {
            out.insert(n);
        }
    }
    return out;
}

// The program through the stable path: what it prints.
std::string runCurrent() {
    const std::string program = serverDir + "/current/bin/nexus-server";
    std::fflush(stdout);
    int fds[2];
    if (::pipe(fds) != 0) {
        return "pipe failed";
    }
    const pid_t pid = ::fork();
    if (pid == 0) {
        ::dup2(fds[1], 1);
        ::close(fds[0]);
        ::close(fds[1]);
        ::execl(program.c_str(), program.c_str(), static_cast<char*>(nullptr));
        ::_exit(127);
    }
    ::close(fds[1]);
    std::string out;
    char buf[256];
    ssize_t n = 0;
    while ((n = ::read(fds[0], buf, sizeof buf)) > 0) {
        out.append(buf, std::size_t(n));
    }
    ::close(fds[0]);
    int st = 0;
    ::waitpid(pid, &st, 0);
    return out;
}

// Every build whole, the pointer whole, nothing left over once a run has cleaned.
void allWhole() {
    const Out v = run({"verify"});
    CHECK(v.exit == kExitOk);
    if (v.exit != kExitOk) {
        std::fprintf(stderr, "%s%s", v.out.c_str(), v.err.c_str());
    }
}

void fresh() {
    removeAll(root);
    fs::create_directories(root);
    serverDir = (root / "server").string();
    buildsDir = serverDir + "/builds";
}

// A tested build placed and made current.
std::string placeCurrent(const fs::path& tree, const std::string& name) {
    const fs::path ev = evidenceFor(tree, name);
    const Out p = run({"place", tree.string(), "--evidence", ev.string()});
    CHECK(p.exit == kExitOk && has(p.out, "tested"));
    const std::string h = hashOf(tree);
    const Out c = run({"current", h});
    CHECK(c.exit == kExitOk);
    return h;
}

void placeAndSwitch() {
    fresh();
    const fs::path a = makeTree("a", "program A", "driver A");
    const fs::path b = makeTree("b", "program B", "driver B");
    const std::string ha = hashOf(a);
    const std::string hb = hashOf(b);
    CHECK(ha != hb);
    CHECK(run({"list"}).out == "no builds placed in " + buildsDir + "\n");
    // Untested: placed, and never made current.
    Out o = run({"place", a.string()});
    CHECK(o.exit == kExitOk && o.out == "placed " + ha + " untested\n");
    o = run({"current", ha});
    CHECK(o.exit == kExitRefused && has(o.err, "untested"));
    CHECK(pointer().first.empty());
    // Placing it again tested is refused: a placed build is never changed.
    o = run({"place", a.string(), "--evidence", evidenceFor(a, "a").string()});
    CHECK(o.exit == kExitRefused && has(o.err, "never changed"));
    o = run({"remove", ha});
    CHECK(o.exit == kExitOk && o.out == "removed " + ha + "\n");
    o = run({"place", a.string(), "--evidence", evidenceFor(a, "a").string()});
    CHECK(o.exit == kExitOk && o.out == "placed " + ha + " tested\n");
    o = run({"place", a.string(), "--evidence", evidenceFor(a, "a").string()});
    CHECK(o.exit == kExitOk && o.out == "already placed " + ha + " tested\n");
    // The first current: no previous.
    o = run({"current", ha});
    CHECK(o.exit == kExitOk && o.out == "current " + ha + "\nprevious none\n");
    CHECK(pointer() == std::make_pair(ha, std::string()));
    CHECK(runCurrent() == "program A\n");
    CHECK(fs::read_symlink(serverDir + "/current") == "builds/active/current");
    o = run({"current", ha});
    CHECK(o.exit == kExitOk && has(o.out, "already current"));
    o = run({"back"});
    CHECK(o.exit == kExitRefused && has(o.err, "no previous"));
    // A second: current and previous in one rename.
    placeCurrent(b, "b");
    CHECK(pointer() == std::make_pair(hb, ha));
    CHECK(runCurrent() == "program B\n");
    o = run({"back"});
    CHECK(o.exit == kExitOk && o.out == "current " + ha + "\nprevious " + hb + "\n");
    CHECK(pointer() == std::make_pair(ha, hb));
    CHECK(runCurrent() == "program A\n");
    // The list: roles, tested, use, formats.
    o = run({"list"});
    CHECK(o.exit == kExitOk && has(o.out, ha + " current tested - formats:store-record=") &&
          has(o.out, hb + " previous tested - formats:"));
    allWhole();
    // Prune keeps current and previous; a third is removed with --keep 0, kept with the default.
    const fs::path c = makeTree("c", "program C", "driver C");
    CHECK(run({"place", c.string()}).exit == kExitOk);
    o = run({"prune"});
    CHECK(o.exit == kExitOk && o.out == "nothing to prune\n");
    o = run({"prune", "--keep", "0"});
    CHECK(o.exit == kExitOk && o.out == "removed " + hashOf(c) + "\n");
    CHECK(placedBuilds() == (std::set<std::string>{ha, hb}));
    o = run({"remove", ha});
    CHECK(o.exit == kExitRefused && has(o.err, "current"));
    o = run({"remove", hb});
    CHECK(o.exit == kExitRefused && has(o.err, "previous"));
    o = run({"remove", std::string(64, 'f')});
    CHECK(o.exit == kExitRefused && has(o.err, "not placed"));
    o = run({"current", std::string(64, 'f')});
    CHECK(o.exit == kExitRefused && has(o.err, "not placed"));
    o = run({"current", "nonsense"});
    CHECK(o.exit == kExitRefused);
    // Every file read-only once placed.
    struct stat st;
    CHECK(::stat((buildsDir + "/" + ha + "/bin/nexus-server").c_str(), &st) == 0 && (st.st_mode & 07777) == 0555);
    CHECK(::stat((buildsDir + "/" + ha + "/formats").c_str(), &st) == 0 && (st.st_mode & 07777) == 0444);
    CHECK(::stat((buildsDir + "/" + ha).c_str(), &st) == 0 && (st.st_mode & 07777) == 0555);
    CHECK(leftovers().empty());
}

// A place killed at every step, then a fresh run: the current build still current, whole and runnable; the new build
// either placed whole or not named at all; the next run removes what was left.
void crashPlace() {
    fresh();
    const fs::path a = makeTree("a", "program A", "driver A");
    const fs::path b = makeTree("b", "program B", "driver B");
    const std::string ha = placeCurrent(a, "a");
    const std::string hb = hashOf(b);
    const std::vector<std::string> args{"place", b.string(), "--evidence", evidenceFor(b, "b").string()};
    // The steps once, on a copy that is then removed.
    const std::vector<std::string> seen = steps(args);
    CHECK(seen.size() >= 10);
    CHECK(run({"remove", hb}).exit == kExitOk);
    for (std::size_t k = 0; k < seen.size(); ++k) {
        const int st = killedAt(args, k);
        CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 9);
        CHECK(pointer() == std::make_pair(ha, std::string()));
        CHECK(runCurrent() == "program A\n");
        const bool named = placedBuilds().count(hb) != 0;
        if (named) {
            Manifest m;
            CHECK(checkBuild(buildsDir, hb, &m).is_ok() && m.tested);
        }
        // The fresh run of the same verb, itself killed at its first clean when there is something to clean.
        if (!leftovers().empty()) {
            const int st2 = killedAt(args, 2);   // lock, settle, then the first clean
            CHECK(WIFEXITED(st2) && WEXITSTATUS(st2) == 9);
        }
        const Out o = run(args);
        CHECK(o.exit == kExitOk && (has(o.out, "placed " + hb + " tested")));
        CHECK(leftovers().empty());
        allWhole();
        CHECK(pointer() == std::make_pair(ha, std::string()));
        CHECK(run({"remove", hb}).exit == kExitOk);
        std::printf("killed at step %zu (%s): %s\n", k, seen[k].c_str(), named ? "named, whole" : "not named");
    }
}

// A current killed at every step: the pointer names the old pair or the new, never a mix; both builds whole; the
// previous good build still there and runnable; a fresh run makes the switch.
void crashSwitch() {
    fresh();
    const fs::path a = makeTree("a", "program A", "driver A");
    const fs::path b = makeTree("b", "program B", "driver B");
    const fs::path c = makeTree("c", "program C", "driver C");
    placeCurrent(a, "a");
    const std::string hb = placeCurrent(b, "b");
    const std::string ha = hashOf(a);
    const std::string hc = hashOf(c);
    CHECK(run({"place", c.string(), "--evidence", evidenceFor(c, "c").string()}).exit == kExitOk);
    const std::vector<std::string> args{"current", hc};
    const std::vector<std::string> seen = steps(args);
    CHECK(run({"current", hb}).exit == kExitOk);   // each round starts from (b, c)
    for (std::size_t k = 0; k < seen.size(); ++k) {
        CHECK(run({"prune"}).exit == kExitOk);    // removes the old pair, so the steps are those counted
        const auto before = pointer();
        const int st = killedAt(args, k);
        CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 9);
        const auto after = pointer();
        CHECK(after == before || after == std::make_pair(hc, before.first));
        allWhole();
        CHECK(runCurrent() == (after.first == hc ? "program C\n" : "program B\n"));
        const Out o = run(args);
        CHECK(o.exit == kExitOk);
        CHECK(pointer().first == hc && pointer().second == hb);
        CHECK(leftovers().empty());
        // Back: previous is the last good build, and runs.
        CHECK(run({"back"}).exit == kExitOk && runCurrent() == "program B\n");
        std::printf("killed at step %zu (%s): %s\n", k, seen[k].c_str(), after == before ? "old pair" : "new pair");
    }
    (void)ha;
}

void crashPrune() {
    fresh();
    const fs::path a = makeTree("a", "program A", "driver A");
    const fs::path b = makeTree("b", "program B", "driver B");
    const fs::path c = makeTree("c", "program C", "driver C");
    const std::string ha = placeCurrent(a, "a");
    const std::string hb = placeCurrent(b, "b");
    const std::string hc = hashOf(c);
    const std::vector<std::string> args{"prune", "--keep", "0"};
    CHECK(run({"place", c.string()}).exit == kExitOk);
    const std::vector<std::string> seen = steps(args);
    for (std::size_t k = 0; k < seen.size(); ++k) {
        CHECK(run({"place", c.string()}).exit == kExitOk);
        const int st = killedAt(args, k);
        CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 9);
        CHECK(pointer() == std::make_pair(hb, ha));
        allWhole();
        const Out o = run(args);
        CHECK(o.exit == kExitOk);
        CHECK(placedBuilds() == (std::set<std::string>{ha, hb}) && leftovers().empty());
        std::printf("killed at step %zu (%s)\n", k, seen[k].c_str());
    }
    (void)hc;
}

void twoPlaces() {
    fresh();
    const fs::path a = makeTree("a", "program A", "driver A");
    const std::string ha = placeCurrent(a, "a");
    const fs::path b = makeTree("b", "program B", "driver B");
    const fs::path c = makeTree("c", "program C", "driver C");
    const std::string evb = evidenceFor(b, "b").string();
    const std::string evc = evidenceFor(c, "c").string();
    for (int round = 0; round < 40; ++round) {
        // The same tree twice, then two trees.
        auto [x, y] = together({"place", b.string(), "--evidence", evb}, {"place", b.string(), "--evidence", evb});
        CHECK(x == kExitOk && y == kExitOk);
        auto [p, q] = together({"place", c.string(), "--evidence", evc}, {"prune", "--keep", "0"});
        CHECK(p == kExitOk && q == kExitOk);
        CHECK(pointer() == std::make_pair(ha, std::string()));
        allWhole();
        CHECK(run({"prune", "--keep", "0"}).exit == kExitOk);
        CHECK(placedBuilds() == (std::set<std::string>{ha}) && leftovers().empty());
    }
}

void twoSwitches() {
    fresh();
    const fs::path a = makeTree("a", "program A", "driver A");
    const fs::path b = makeTree("b", "program B", "driver B");
    const std::string ha = placeCurrent(a, "a");
    const std::string hb = placeCurrent(b, "b");
    for (int round = 0; round < 40; ++round) {
        auto [x, y] = together({"current", ha}, {"current", hb});
        CHECK(x == kExitOk && y == kExitOk);
        const auto p = pointer();
        // Whichever went second is current, and the other previous: the last good build is never lost.
        CHECK((p == std::make_pair(ha, hb)) || (p == std::make_pair(hb, ha)));
        auto [u, v] = together({"back"}, {"back"});
        CHECK(u == kExitOk && v == kExitOk);
        CHECK(pointer() == p);    // two backs: there and back again
        allWhole();
        CHECK(leftovers().empty());
    }
}

void refusals() {
    fresh();
    const fs::path a = makeTree("a", "program A", "driver A");
    const std::string ha = placeCurrent(a, "a");
    // A driver changed after its tests: the evidence names the old bytes.
    const fs::path b = makeTree("b", "program B", "driver B");
    const fs::path evb = evidenceFor(b, "b");
    writeFile(b / "drivers" / "claude" / "main.js", "driver B, changed\n");
    Out o = run({"place", b.string(), "--evidence", evb.string()});
    CHECK(o.exit == kExitRefused && has(o.err, "other bytes"));
    CHECK(placedBuilds() == (std::set<std::string>{ha}) && leftovers().empty());
    // A stale mark: another tree's evidence.
    o = run({"place", b.string(), "--evidence", evidenceFor(a, "a").string()});
    CHECK(o.exit == kExitRefused && has(o.err, "other bytes"));
    // A failing result, evidence that does not read.
    writeFile(root / "ev" / "fail", "nexus-test-evidence 1\ntree " + hashOf(b) + "\nresult fail\n");
    o = run({"place", b.string(), "--evidence", (root / "ev" / "fail").string()});
    CHECK(o.exit == kExitRefused && has(o.err, "fail"));
    writeFile(root / "ev" / "junk", "hello\n");
    o = run({"place", b.string(), "--evidence", (root / "ev" / "junk").string()});
    CHECK(o.exit == kExitRefused);
    // What a build cannot be: another top-level name, no program, no formats, formats that do not read, a link out.
    const fs::path d = makeTree("d", "program D", "driver D");
    writeFile(d / "README", "x\n");
    o = run({"place", d.string()});
    CHECK(o.exit == kExitRefused && has(o.err, "README"));
    fs::remove(d / "README");
    fs::create_symlink("../../../etc", d / "drivers" / "out");
    o = run({"place", d.string()});
    CHECK(o.exit == kExitRefused);
    fs::remove(d / "drivers" / "out");
    fs::remove(d / "formats");
    o = run({"place", d.string()});
    CHECK(o.exit == kExitRefused && has(o.err, "formats"));
    writeFile(d / "formats", "format x 1\n");
    o = run({"place", d.string()});
    CHECK(o.exit == kExitRefused);
    writeFile(d / "formats", thisBuildFormats());
    fs::permissions(d / "bin" / "nexus-server", fs::perms(0644));
    o = run({"place", d.string()});
    CHECK(o.exit == kExitRefused && has(o.err, "executable"));
    CHECK(leftovers().empty());
    // The formats rule: a build that cannot read what the current one writes.
    const fs::path old = makeTree("old", "program old", "driver old",
                                  "format store-record 2 2\nformat store-snapshot 4 4\nformat config 2 2\n");
    o = run({"place", old.string(), "--evidence", evidenceFor(old, "old").string()});
    CHECK(o.exit == kExitOk);
    o = run({"current", hashOf(old)});
    CHECK(o.exit == kExitRefused && has(o.err, "store-record") && has(o.err, "could not read"));
    CHECK(pointer().first == ha);
    // A tampered file in a placed build: verify says damaged, and it is not made current.
    const fs::path e = makeTree("e", "program E", "driver E");
    CHECK(run({"place", e.string(), "--evidence", evidenceFor(e, "e").string()}).exit == kExitOk);
    const std::string he = hashOf(e);
    const fs::path victim = fs::path(buildsDir) / he / "drivers" / "claude" / "main.js";
    fs::permissions(victim.parent_path(), fs::perms::owner_all, fs::perm_options::add);
    fs::permissions(victim, fs::perms::owner_write, fs::perm_options::add);
    writeFile(victim, "driver E, tampered\n");
    fs::permissions(victim, fs::perms(0444));
    o = run({"verify", he});
    CHECK(o.exit == kExitFailed && has(o.out, "damaged " + he));
    o = run({"current", he});
    CHECK(o.exit == kExitFailed && has(o.err, "damaged"));
    CHECK(pointer().first == ha);
    // A tested mark copied into an untested build's manifest is not its own: the manifest no longer reads.
    const fs::path f = makeTree("f", "program F", "driver F");
    CHECK(run({"place", f.string()}).exit == kExitOk);
    const std::string hf = hashOf(f);
    const fs::path man = fs::path(buildsDir) / hf / kManifest;
    fs::permissions(man.parent_path(), fs::perms::owner_all, fs::perm_options::add);
    std::string text = readAll(man);
    text.insert(text.find("dir\t"), "tested " + ha + " pass copied\n");
    fs::permissions(man, fs::perms::owner_write, fs::perm_options::add);
    writeFile(man, text);
    o = run({"current", hf});
    CHECK(o.exit == kExitFailed && has(o.err, "damaged"));
    CHECK(pointer().first == ha);
    // Usage.
    CHECK(run({"place"}).exit == kExitUsage);
    CHECK(run({"prune", "--keep", "x"}).exit == kExitUsage);
    CHECK(run({"nonsense"}).exit == kExitUsage);
}

// A build a running server holds is never removed: prune keeps it, remove refuses it; once it is let go, it goes.
void pruneInUse() {
    fresh();
    const fs::path a = makeTree("a", "program A", "driver A");
    const fs::path b = makeTree("b", "program B", "driver B");
    const fs::path c = makeTree("c", "program C", "driver C");
    const std::string ha = placeCurrent(a, "a");
    const std::string hb = placeCurrent(b, "b");
    CHECK(run({"place", c.string()}).exit == kExitOk);
    const std::string hc = hashOf(c);
    BuildHold hold;
    char* real = ::realpath(buildsDir.c_str(), nullptr);
    const std::string program = std::string(real) + "/" + hc + "/" + kProgram;
    std::free(real);
    CHECK(holdBuild(program, hold).is_ok() && hold.hash() == hc);
    // In another process, as a real prune is.
    auto [x, y] = together({"prune", "--keep", "0"}, {"list"});
    CHECK(x == kExitOk && y == kExitOk);
    CHECK(placedBuilds().count(hc) == 1);
    Out o = run({"list"});
    CHECK(has(o.out, hc + " - untested in-use"));
    o = run({"remove", hc});
    CHECK(o.exit == kExitRefused && has(o.err, "in use"));
    o = run({"prune", "--keep", "0"});
    CHECK(o.exit == kExitOk && has(o.out, "kept " + hc));
    hold.release();
    o = run({"prune", "--keep", "0"});
    CHECK(o.exit == kExitOk && o.out == "removed " + hc + "\n");
    CHECK(placedBuilds() == (std::set<std::string>{ha, hb}));
}

void diskFull() {
    fresh();
    const fs::path a = makeTree("a", "program A", "driver A");
    const std::string ha = placeCurrent(a, "a");
    const fs::path b = makeTree("b", "program B", "driver B");
    for (std::uint64_t limit : {std::uint64_t(1), std::uint64_t(20), std::uint64_t(40)}) {
        Options o;
        o.test_disk_full_after = limit;
        Result r;
        const int code = command(serverDir, {"place", b.string()}, o, r);
        CHECK(code == kExitFailed && has(joined(r.err), "disk is full"));
        CHECK(placedBuilds() == (std::set<std::string>{ha}) && leftovers().empty());
    }
    CHECK(run({"place", b.string()}).exit == kExitOk);
    allWhole();
}

void unkeptLocks() {
    fresh();
    const fs::path a = makeTree("a", "program A", "driver A");
    Options o;
    o.test_locks_ignored = true;
    Result r;
    CHECK(command(serverDir, {"place", a.string()}, o, r) == kExitFailed && has(joined(r.err), "not kept"));
    Options n;
    n.test_fs_type = "nfs";
    Result r2;
    CHECK(command(serverDir, {"place", a.string()}, n, r2) == kExitFailed && has(joined(r2.err), "NFS"));
    Result r3;
    CHECK(command(serverDir, {"list"}, n, r3) == kExitFailed);
    CHECK(placedBuilds().empty());
    // A held lock: busy after the wait, said in one line.
    const int fd = ::open(buildsDir.c_str(), O_RDONLY | O_DIRECTORY);
    CHECK(fd >= 0 && ::flock(fd, LOCK_EX) == 0);
    std::fflush(stdout);
    const pid_t pid = ::fork();
    if (pid == 0) {
        Options w;
        w.lock_wait_ms = 200;
        Result rw;
        command(serverDir, {"place", a.string()}, w, rw);
        ::_exit(rw.exit == kExitBusy && has(joined(rw.err), "holds the builds lock") ? 0 : 1);
    }
    int st = 0;
    ::waitpid(pid, &st, 0);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
    ::close(fd);
}

void holdCase() {
    fresh();
    const fs::path a = makeTree("a", "program A", "driver A");
    const fs::path b = makeTree("b", "program B", "driver B");
    placeCurrent(a, "a");
    CHECK(run({"place", b.string()}).exit == kExitOk);
    const std::string hb = hashOf(b);
    char* real = ::realpath(buildsDir.c_str(), nullptr);
    const std::string folder = std::string(real) + "/" + hb;
    std::free(real);
    BuildHold hold;
    // Not a placed build (a build tree): held is nothing, and fine.
    CHECK(holdBuild((a / "bin" / "nexus-server").string(), hold).is_ok() && hold.hash().empty());
    // Through the stable path, resolved: the current build.
    char* cur = ::realpath((serverDir + "/current/bin/nexus-server").c_str(), nullptr);
    CHECK(cur != nullptr);
    if (cur != nullptr) {
        CHECK(holdBuild(cur, hold).is_ok() && hold.hash() == hashOf(a));
        std::free(cur);
    }
    hold.release();
    // A prune holding it exclusive: the start is refused once the wait is out.
    const int fd = ::open(folder.c_str(), O_RDONLY | O_DIRECTORY);
    CHECK(fd >= 0 && ::flock(fd, LOCK_EX) == 0);
    std::uint64_t now = 0;
    auto s = holdBuild(folder + "/" + kProgram, hold, [&] { return now; }, [&](std::uint64_t ms) { now += ms; }, 100);
    CHECK(!s.is_ok() && has(s.message(), "being removed"));
    ::close(fd);
    // Removed (including Linux's /proc/self/exe paths after a rename to .gone-* or unlink): refused.
    CHECK(run({"remove", hb}).exit == kExitOk);
    s = holdBuild(folder + "/" + kProgram, hold);
    CHECK(!s.is_ok());
    const std::string buildsReal = folder.substr(0, folder.rfind('/'));
    CHECK(!holdBuild(buildsReal + "/.gone-1-1/" + kProgram, hold).is_ok());
    CHECK(!holdBuild(folder + "/" + kProgram + " (deleted)", hold).is_ok());
    CHECK(holdBuild((a / "bin" / "nexus-server").string() + " (deleted)", hold).is_ok() && hold.hash().empty());
    // Two servers of one build: both hold it.
    char* again = ::realpath((serverDir + "/current/bin/nexus-server").c_str(), nullptr);
    BuildHold second;
    if (again != nullptr) {
        CHECK(holdBuild(again, hold).is_ok() && holdBuild(again, second).is_ok());
        std::free(again);
    }
    // A dangling symlink alongside read-only placed builds does not stop removeAll from cleaning the test root.
    fs::create_symlink("missing/target", root / "00-dangling");
}

void helpCase() {
    fresh();
    for (const char* v : {"place", "hash", "formats", "list", "verify", "current", "back", "prune", "remove"}) {
        const Out o = run({"help", v});
        CHECK(o.exit == kExitOk && has(o.out, "Exit"));
        const Out h = run({v, "--help"});
        CHECK(h.exit == kExitOk && h.out == o.out);
    }
    CHECK(run({"help", "nonsense"}).exit == kExitUsage);
    const Out f = run({"formats"});
    CHECK(f.exit == kExitOk && f.out == thisBuildFormats());
    const fs::path a = makeTree("a", "program A", "driver A");
    const Out h = run({"hash", a.string()});
    CHECK(h.exit == kExitOk && h.out == hashOf(a) + "\n");
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: builds_functional <case> <dir>\n");
        return 2;
    }
    const std::string c = argv[1];
    root = fs::absolute(argv[2]);
    const std::vector<std::pair<std::string, std::function<void()>>> cases{
        {"place_and_switch", placeAndSwitch}, {"crash_place", crashPlace}, {"crash_switch", crashSwitch},
        {"crash_prune", crashPrune}, {"two_places", twoPlaces}, {"two_switches", twoSwitches},
        {"refusals", refusals}, {"prune_in_use", pruneInUse}, {"disk_full", diskFull},
        {"unkept_locks", unkeptLocks}, {"hold", holdCase}, {"help", helpCase}};
    for (const auto& [name, fn] : cases) {
        if (name == c) {
            fn();
            removeAll(root);
            if (failures != 0) {
                std::fprintf(stderr, "builds_functional %s: %d failed\n", c.c_str(), failures);
                return 1;
            }
            std::printf("builds_functional %s: ok\n", c.c_str());
            return 0;
        }
    }
    std::fprintf(stderr, "builds_functional: no case %s\n", c.c_str());
    return 2;
}
