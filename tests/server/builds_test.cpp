// Unit tests of placement's text and decisions (src/server/build_manifest.h, src/server/sha256.h): the hash, the
// manifest and its reading, the evidence, the formats rule, the names in the builds directory, prune's choice and the
// build a program path is in. Logic only: no files, no clock, no sockets. The filesystem verbs are
// builds_functional's; the steps' order is the model's (models/placement) and its trace walk's.
//
// Usage: builds_test <group>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "server/build_manifest.h"
#include "server/builds.h"
#include "server/sha256.h"

namespace {

using namespace nexus::server;
using namespace nexus::server::builds;

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

const std::string kH1(64, 'a');
const std::string kH2(64, 'b');
const std::string kH3(64, 'c');

Entry file(const std::string& path, const std::string& bytes, bool exec = false) {
    Entry e;
    e.kind = EntryKind::File;
    e.path = path;
    e.exec = exec;
    e.size = bytes.size();
    e.sha = sha256Hex(bytes);
    return e;
}
Entry dirEntry(const std::string& path) {
    Entry e;
    e.kind = EntryKind::Dir;
    e.path = path;
    return e;
}
Entry link(const std::string& path, const std::string& target) {
    Entry e;
    e.kind = EntryKind::Link;
    e.path = path;
    e.target = target;
    return e;
}

std::vector<Entry> tree(const std::string& program, const std::string& driver) {
    return {dirEntry("bin"), file("bin/nexus-server", program, true), dirEntry("drivers"),
            dirEntry("drivers/claude"), file("drivers/claude/main.js", driver), link("drivers/main.js", "claude/main.js"),
            file("formats", "format store-record 3 3\n")};
}

// FIPS 180-4's examples, and a message across a block's edge.
void sha() {
    CHECK(sha256Hex("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(sha256Hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(sha256Hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    Sha256 split;
    const std::string million(1000000, 'a');
    split.update(million.data(), 333333);
    split.update(million.data() + 333333, million.size() - 333333);
    CHECK(split.hex() == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

void manifest() {
    const std::vector<Entry> a = tree("program 1", "driver 1");
    // The hash is of every entry, in path order whatever order they were found in.
    std::vector<Entry> shuffled(a.rbegin(), a.rend());
    CHECK(treeHash(a) == treeHash(shuffled));
    CHECK(isHash(treeHash(a)));
    // The driver tree is in the hash: a driver changed is another build.
    CHECK(treeHash(a) != treeHash(tree("program 1", "driver 2")));
    CHECK(treeHash(a) != treeHash(tree("program 2", "driver 1")));
    // The reversed fault's hash leaves it out (and so cannot tell those two apart).
    CHECK(treeHash(a, true) == treeHash(tree("program 1", "driver 2"), true));
    // A mode is in it, and a link's target.
    std::vector<Entry> b = a;
    b[1].exec = false;
    CHECK(treeHash(a) != treeHash(b));
    b = a;
    b[5].target = "claude/other.js";
    CHECK(treeHash(a) != treeHash(b));

    Manifest m;
    m.entries = a;
    m.tree = treeHash(a);
    std::string why;
    CHECK(readFormats("format store-record 3 3\n", &m.formats, &why));
    m.tested = true;
    m.run = "ctest 120 passed";
    const std::string text = manifestText(m);
    Manifest back;
    CHECK(readManifest(text, &back, &why));
    CHECK(back.tree == m.tree && back.tested && back.run == m.run && back.formats == m.formats);
    CHECK(back.entries.size() == a.size());
    // Incomplete: no end, or cut anywhere before it.
    const std::string noEnd = text.substr(0, text.rfind("end\n"));
    CHECK(!readManifest(noEnd, &back, &why) && has(why, "incomplete"));
    for (std::size_t cut = 0; cut < text.size() - 1; cut += 7) {
        CHECK(!readManifest(text.substr(0, cut), &back, &why));
    }
    // Its tree must be the hash of what it lists: an entry changed, added or removed is refused.
    std::string tampered = text;
    const std::size_t at = tampered.find(sha256Hex("driver 1"));
    tampered.replace(at, 64, sha256Hex("driver 2"));
    CHECK(!readManifest(tampered, &back, &why) && has(why, "not the hash"));
    // A tested line for another tree (a stale mark) is refused.
    std::string stale = text;
    const std::size_t tl = stale.find("tested ");
    stale.replace(tl + 7, 64, kH1);
    CHECK(!readManifest(stale, &back, &why) && has(why, "tested"));
    // A line it does not know, and one after end.
    CHECK(!readManifest(text + "more\n", &back, &why) && has(why, "after `end`"));
    CHECK(!readManifest("nexus-build 2\n", &back, &why));
    // Untested: no tested line, and the manifest says so.
    m.tested = false;
    CHECK(readManifest(manifestText(m), &back, &why) && !back.tested);

    // Paths: inside the build, under its top-level names, nothing odd.
    CHECK(pathProblem("bin/nexus-server").empty());
    CHECK(pathProblem("nexus.app/Contents/MacOS/nexus").empty());
    CHECK(!pathProblem("").empty());
    CHECK(!pathProblem("/bin/x").empty());
    CHECK(!pathProblem("bin/../x").empty());
    CHECK(!pathProblem("bin/./x").empty());
    CHECK(!pathProblem("bin//x").empty());
    CHECK(!pathProblem("etc/passwd").empty());
    CHECK(!pathProblem("manifest").empty());
    CHECK(!pathProblem(std::string("bin/a\tb")).empty());
    CHECK(!pathProblem(std::string("bin/a\nb")).empty());
    CHECK(!pathProblem("bin/" + std::string(1100, 'x')).empty());
    // Links stay inside the build.
    CHECK(linkProblem("drivers/main.js", "claude/main.js").empty());
    CHECK(linkProblem("drivers/claude/x", "../main.js").empty());
    CHECK(!linkProblem("drivers/main.js", "/etc/passwd").empty());
    CHECK(!linkProblem("drivers/main.js", "../../outside").empty());
    CHECK(!linkProblem("bin/x", "../../../etc").empty());
}

void evidence() {
    Evidence e{kH1, "pass", "ctest: 120 tests, 0 failed"};
    Evidence back;
    std::string why;
    CHECK(readEvidence(evidenceText(e), &back, &why));
    CHECK(back.tree == kH1 && back.result == "pass" && back.run == e.run);
    CHECK(!readEvidence("", &back, &why));
    CHECK(!readEvidence("nexus-test-evidence 1\nresult pass\n", &back, &why) && has(why, "tree"));
    CHECK(!readEvidence("nexus-test-evidence 1\ntree " + kH1 + "\n", &back, &why) && has(why, "result"));
    CHECK(!readEvidence("nexus-test-evidence 1\ntree abc\nresult pass\n", &back, &why));
    CHECK(!readEvidence("nexus-test-evidence 2\ntree " + kH1 + "\nresult pass\n", &back, &why));
    CHECK(!readEvidence("nexus-test-evidence 1\ntree " + kH1 + "\nresult pass\nwhat x\n", &back, &why));
    // A failing result reads (place refuses it, saying what it said).
    CHECK(readEvidence("nexus-test-evidence 1\ntree " + kH1 + "\nresult fail\n", &back, &why) &&
          back.result == "fail");
}

void formats() {
    Formats f;
    std::string why;
    CHECK(readFormats("# a comment\n\nformat store-record 3 3\nformat config 2 2\n", &f, &why));
    CHECK(f.size() == 2 && findFormat(f, "config") != nullptr && findFormat(f, "config")->writes == 2);
    CHECK(readFormats(formatsText(f), &f, &why) && f.size() == 2);
    CHECK(!readFormats("format store-record 3\n", &f, &why));
    CHECK(!readFormats("format store-record 3 4\n", &f, &why));      // a reader past what it writes
    CHECK(!readFormats("format a 1 1\nformat a 2 1\n", &f, &why));   // twice
    CHECK(!readFormats("version 3\n", &f, &why));
    // This build's own file reads, and names the checked formats.
    CHECK(readFormats(thisBuildFormats(), &f, &why));
    for (const std::string& n : kCheckedFormats) {
        CHECK(findFormat(f, n) != nullptr);
    }
    CHECK(findFormat(f, "link") != nullptr && findFormat(f, "driver-protocol") != nullptr);

    // The rule: the target must read what the current build writes, per checked format.
    Formats cur;
    Formats tgt;
    CHECK(readFormats("format store-record 4 4\nformat config 2 2\nformat link 9 9\n", &cur, &why));
    CHECK(readFormats("format store-record 4 3\nformat config 2 2\nformat link 1 1\n", &tgt, &why));
    CHECK(formatsRefusal(tgt, kH2, cur, kH1).empty());              // the link is negotiated, not checked
    CHECK(readFormats("format store-record 3 3\nformat config 2 2\n", &tgt, &why));
    std::string r = formatsRefusal(tgt, kH2, cur, kH1);
    CHECK(has(r, "store-record") && has(r, "at least 4") && has(r, kH2));
    // A newer target, reading the current's older writes, is fine.
    CHECK(formatsRefusal(cur, kH1, tgt, kH2).empty());
    // A target that states none of a format the current writes is refused.
    CHECK(readFormats("format store-record 4 4\n", &tgt, &why));
    CHECK(has(formatsRefusal(tgt, kH2, cur, kH1), "states no config"));
    // A current that states nothing has nothing to check.
    CHECK(formatsRefusal(tgt, kH2, Formats{}, kH1).empty());
}

void names() {
    CHECK(classify(kH1) == NameKind::Build);
    CHECK(classify("active") == NameKind::Active);
    CHECK(classify(".tmp-12-1") == NameKind::Tmp);
    CHECK(classify(".pair-12-1") == NameKind::PairTmp);
    CHECK(classify(".active-12-1") == NameKind::ActiveTmp);
    CHECK(classify(".gone-12-1") == NameKind::Gone);
    CHECK(classify(pairName(kH1, kH2)) == NameKind::Pair);
    CHECK(classify(pairName(kH1, "")) == NameKind::Pair);
    CHECK(classify("pair-" + kH1) == NameKind::Other);
    CHECK(classify("pair-" + kH1 + "-xyz") == NameKind::Other);
    CHECK(classify(std::string(64, 'A')) == NameKind::Other);   // a hash is lowercase
    CHECK(classify("notes.txt") == NameKind::Other);
    std::string c;
    std::string p;
    CHECK(readPairName(pairName(kH1, kH2), &c, &p) && c == kH1 && p == kH2);
    CHECK(readPairName(pairName(kH1, ""), &c, &p) && c == kH1 && p.empty());
    CHECK(pairName(kH1, "") == "pair-" + kH1 + "-none");
    CHECK(!readPairName("pair-" + kH1 + "-" + kH2 + "x", &c, &p));
}

void prune() {
    const std::vector<Placed> all{{kH1, 10}, {kH2, 20}, {kH3, 30}, {std::string(64, 'd'), 40},
                                  {std::string(64, 'e'), 50}};
    // Current and previous never; then the newest `keep` of the rest kept; the rest oldest first.
    std::vector<std::string> out = pruneCandidates(all, std::string(64, 'e'), kH1, 1);
    CHECK((out == std::vector<std::string>{kH2, kH3}));
    out = pruneCandidates(all, std::string(64, 'e'), kH1, 0);
    CHECK((out == std::vector<std::string>{kH2, kH3, std::string(64, 'd')}));
    out = pruneCandidates(all, std::string(64, 'e'), kH1, 9);
    CHECK(out.empty());
    out = pruneCandidates(all, kH3, "", 0);
    CHECK(out.size() == 4 && out.front() == kH1);
    // Ties on time go by hash, whatever order the directory gave.
    const std::vector<Placed> tie{{kH2, 5}, {kH1, 5}, {kH3, 1}};
    const std::vector<Placed> tie2{{kH1, 5}, {kH3, 1}, {kH2, 5}};
    CHECK(pruneCandidates(tie, "", "", 1) == pruneCandidates(tie2, "", "", 1));
}

void program() {
    CHECK(buildOfProgram("/home/me/.nexus/server/builds/" + kH1 + "/bin/nexus-server") == kH1);
    CHECK(buildOfProgram("/home/me/build/bin/nexus-server").empty());
    CHECK(buildOfProgram("/home/me/.nexus/server/builds/" + kH1 + "/bin/nexus").empty());
    CHECK(buildOfProgram("/home/me/.nexus/server/builds/.tmp-1-2/bin/nexus-server").empty());
    CHECK(buildOfProgram("/home/me/.nexus/server/other/" + kH1 + "/bin/nexus-server").empty());
    CHECK(buildOfProgram("nexus-server").empty());
    // Help: every verb has its own, each stating its exit statuses.
    for (const char* v : {"place", "hash", "formats", "list", "verify", "current", "back", "prune", "remove"}) {
        CHECK(!help(v).empty() && has(help(v), "Exit"));
    }
    CHECK(help("nonsense").empty());
    CHECK(has(help(""), "place") && has(help(""), "prune"));
}

} // namespace

int main(int argc, char** argv) {
    if (argc > 2) {
        std::fprintf(stderr, "usage: builds_test [sha|manifest|evidence|formats|names|prune|program]\n");
        return 2;
    }
    // No group: every group (the sanitizer jobs run it so).
    const std::string g = argc == 2 ? argv[1] : "all";
    if (g == "all") {
        sha();
        manifest();
        evidence();
        formats();
        names();
        prune();
        program();
    } else if (g == "sha") {
        sha();
    } else if (g == "manifest") {
        manifest();
    } else if (g == "evidence") {
        evidence();
    } else if (g == "formats") {
        formats();
    } else if (g == "names") {
        names();
    } else if (g == "prune") {
        prune();
    } else if (g == "program") {
        program();
    } else {
        std::fprintf(stderr, "builds_test: no group %s\n", g.c_str());
        return 2;
    }
    if (failures != 0) {
        std::fprintf(stderr, "builds_test %s: %d failed\n", g.c_str(), failures);
        return 1;
    }
    std::printf("builds_test %s: ok\n", g.c_str());
    return 0;
}
