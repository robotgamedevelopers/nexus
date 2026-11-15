// A placed build as text and decisions: see build_manifest.h.

#include "server/build_manifest.h"

#include "server/sha256.h"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <sstream>

namespace nexus::server::builds {

namespace {

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == sep) {
            out.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    out.push_back(cur);
    return out;
}

std::vector<std::string> lines(const std::string& text) {
    std::vector<std::string> out = split(text, '\n');
    if (!out.empty() && out.back().empty()) {
        out.pop_back();
    }
    return out;
}

bool readU64(const std::string& s, std::uint64_t* out) {
    if (s.empty() || s.size() > 20) {
        return false;
    }
    for (char c : s) {
        if (c < '0' || c > '9') {
            return false;
        }
    }
    errno = 0;
    char* end = nullptr;
    const unsigned long long v = std::strtoull(s.c_str(), &end, 10);
    if (errno != 0 || end == nullptr || *end != 0) {
        return false;
    }
    *out = v;
    return true;
}

bool readU32(const std::string& s, std::uint32_t* out) {
    std::uint64_t v = 0;
    if (!readU64(s, &v) || v > 0xffffffffULL) {
        return false;
    }
    *out = static_cast<std::uint32_t>(v);
    return true;
}

bool hasControl(const std::string& s) {
    for (unsigned char c : s) {
        if (c < 0x20 || c == 0x7f) {
            return true;
        }
    }
    return false;
}

} // namespace

std::string entryLine(const Entry& e) {
    switch (e.kind) {
    case EntryKind::Dir:
        return "dir\t" + e.path;
    case EntryKind::Link:
        return "link\t" + e.target + "\t" + e.path;
    case EntryKind::File:
        break;
    }
    return std::string("file\t") + (e.exec ? "x" : "-") + "\t" + std::to_string(e.size) + "\t" + e.sha + "\t" + e.path;
}

std::string entriesText(std::vector<Entry> entries) {
    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return a.path < b.path; });
    std::string out;
    for (const Entry& e : entries) {
        out += entryLine(e);
        out += '\n';
    }
    return out;
}

std::string treeHash(const std::vector<Entry>& entries, bool leaveOutDrivers) {
    if (!leaveOutDrivers) {
        return sha256Hex(entriesText(entries));
    }
    std::vector<Entry> kept;
    for (const Entry& e : entries) {
        if (e.path != "drivers" && e.path.rfind("drivers/", 0) != 0) {
            kept.push_back(e);
        }
    }
    return sha256Hex(entriesText(kept));
}

std::string pathProblem(const std::string& path) {
    if (path.empty()) {
        return "an empty path";
    }
    if (path.size() > 1024) {
        return path.substr(0, 64) + "...: a path longer than 1024 bytes";
    }
    if (hasControl(path)) {
        return "a path with a control character or a tab in it";
    }
    if (path[0] == '/') {
        return path + ": an absolute path";
    }
    const std::vector<std::string> parts = split(path, '/');
    for (const std::string& p : parts) {
        if (p.empty() || p == "." || p == "..") {
            return path + ": an empty, `.` or `..` part";
        }
    }
    if (std::find(kTopLevel.begin(), kTopLevel.end(), parts[0]) == kTopLevel.end()) {
        return path + ": a build holds only bin/, drivers/, formats and nexus.app/ at its top";
    }
    if (parts[0] == kFormatsFile && parts.size() > 1) {
        return path + ": formats is a file";
    }
    return std::string();
}

std::string linkProblem(const std::string& path, const std::string& target) {
    if (target.empty() || target.size() > 1024 || hasControl(target)) {
        return path + ": a link with an empty or unreadable target";
    }
    if (target[0] == '/') {
        return path + " -> " + target + ": a link to an absolute path, outside the build (make it relative)";
    }
    // Lexically, from the link's own directory: never above the build's top.
    std::vector<std::string> at = split(path, '/');
    at.pop_back();
    for (const std::string& p : split(target, '/')) {
        if (p.empty() || p == ".") {
            continue;
        }
        if (p == "..") {
            if (at.empty()) {
                return path + " -> " + target + ": a link that leads outside the build";
            }
            at.pop_back();
        } else {
            at.push_back(p);
        }
    }
    return std::string();
}

const Format* findFormat(const Formats& formats, const std::string& name) {
    for (const Format& f : formats) {
        if (f.name == name) {
            return &f;
        }
    }
    return nullptr;
}

bool readFormats(const std::string& text, Formats* out, std::string* why) {
    out->clear();
    std::size_t n = 0;
    for (const std::string& line : lines(text)) {
        ++n;
        if (line.empty() || line[0] == '#') {
            continue;
        }
        const std::vector<std::string> f = split(line, ' ');
        Format fmt;
        const std::string at = "formats line " + std::to_string(n) + ": ";
        if (f.size() != 4 || f[0] != "format" || f[1].empty() || hasControl(f[1]) || !readU32(f[2], &fmt.writes) ||
            !readU32(f[3], &fmt.reader)) {
            *why = at + "`" + line + "` is not `format <name> <writes> <reader>` (two numbers)";
            return false;
        }
        fmt.name = f[1];
        if (fmt.reader > fmt.writes) {
            *why = at + fmt.name + "'s reader " + std::to_string(fmt.reader) + " is past what it writes, " +
                   std::to_string(fmt.writes);
            return false;
        }
        if (findFormat(*out, fmt.name) != nullptr) {
            *why = at + fmt.name + " is given twice";
            return false;
        }
        out->push_back(fmt);
    }
    return true;
}

std::string formatsText(const Formats& formats) {
    std::string out;
    for (const Format& f : formats) {
        out += "format " + f.name + " " + std::to_string(f.writes) + " " + std::to_string(f.reader) + "\n";
    }
    return out;
}

std::string formatsRefusal(const Formats& target, const std::string& targetHash, const Formats& current,
                           const std::string& currentHash) {
    for (const std::string& name : kCheckedFormats) {
        const Format* c = findFormat(current, name);
        if (c == nullptr) {
            continue;   // the current build states none: nothing it writes to check
        }
        const Format* t = findFormat(target, name);
        if (t == nullptr) {
            return "build " + targetHash + " states no " + name + " format, and the current build " + currentHash +
                   " writes " + name + " " + std::to_string(c->writes) + "; a build that does not say it reads "
                   "that is not made current";
        }
        if (t->writes < c->reader) {
            return "build " + targetHash + " reads " + name + " up to version " + std::to_string(t->writes) +
                   ", and the current build " + currentHash + " writes version " + std::to_string(c->writes) +
                   ", which needs a reader of at least " + std::to_string(c->reader) + "; it could not read what "
                   "the current build wrote, so it is not made current (a rollback past a format change waits for "
                   "the store's own check, docs/builds.md)";
        }
    }
    return std::string();
}

bool readEvidence(const std::string& text, Evidence* out, std::string* why) {
    *out = Evidence();
    const std::vector<std::string> ls = lines(text);
    if (ls.empty() || ls[0] != "nexus-test-evidence 1") {
        *why = "the evidence's first line must be `nexus-test-evidence 1`";
        return false;
    }
    for (std::size_t i = 1; i < ls.size(); ++i) {
        const std::string& l = ls[i];
        const std::size_t sp = l.find(' ');
        const std::string key = l.substr(0, sp);
        const std::string value = sp == std::string::npos ? std::string() : l.substr(sp + 1);
        if (key == "tree" && isHash(value)) {
            out->tree = value;
        } else if (key == "result" && !value.empty() && value.find(' ') == std::string::npos) {
            out->result = value;
        } else if (key == "run" && !hasControl(value)) {
            out->run = value;
        } else if (!l.empty()) {
            *why = "evidence line " + std::to_string(i + 1) + ": `" + l + "` is not `tree <hash>`, `result <word>` "
                   "or `run <text>`";
            return false;
        }
    }
    if (out->tree.empty() || out->result.empty()) {
        *why = "the evidence names no tree or no result (it needs `tree <hash>` and `result pass`)";
        return false;
    }
    return true;
}

std::string evidenceText(const Evidence& e) {
    return "nexus-test-evidence 1\ntree " + e.tree + "\nresult " + e.result + "\nrun " + e.run + "\n";
}

std::string manifestText(const Manifest& m) {
    std::string out = "nexus-build 1\ntree " + m.tree + "\n" + formatsText(m.formats);
    if (m.tested) {
        out += "tested " + m.tree + " pass " + m.run + "\n";
    }
    out += entriesText(m.entries);
    out += "end\n";
    return out;
}

bool readManifest(const std::string& text, Manifest* out, std::string* why, bool leaveOutDrivers) {
    *out = Manifest();
    const std::vector<std::string> ls = lines(text);
    if (ls.empty() || ls[0] != "nexus-build 1") {
        *why = "the manifest's first line is not `nexus-build 1`";
        return false;
    }
    bool ended = false;
    std::string formats;
    for (std::size_t i = 1; i < ls.size(); ++i) {
        const std::string& l = ls[i];
        const std::string at = "manifest line " + std::to_string(i + 1) + ": ";
        if (ended) {
            *why = at + "a line after `end`";
            return false;
        }
        if (l == "end") {
            ended = true;
            continue;
        }
        if (l.rfind("tree ", 0) == 0) {
            out->tree = l.substr(5);
            if (!isHash(out->tree)) {
                *why = at + "the tree is not a hash";
                return false;
            }
            continue;
        }
        if (l.rfind("format ", 0) == 0) {
            formats += l + "\n";
            continue;
        }
        if (l.rfind("tested ", 0) == 0) {
            const std::vector<std::string> f = split(l, ' ');
            if (f.size() < 3 || f[1] != out->tree || f[2] != "pass") {
                *why = at + "a `tested` line that is not `tested <this tree> pass <run>`";
                return false;
            }
            out->tested = true;
            out->run = l.substr(std::min(l.size(), 7 + f[1].size() + 6));
            continue;
        }
        const std::vector<std::string> f = split(l, '\t');
        Entry e;
        bool ok = false;
        if (f[0] == "dir" && f.size() == 2) {
            e.kind = EntryKind::Dir;
            e.path = f[1];
            ok = true;
        } else if (f[0] == "file" && f.size() == 5 && (f[1] == "x" || f[1] == "-") && readU64(f[2], &e.size) &&
                   isHash(f[3])) {
            e.kind = EntryKind::File;
            e.exec = f[1] == "x";
            e.sha = f[3];
            e.path = f[4];
            ok = true;
        } else if (f[0] == "link" && f.size() == 3) {
            e.kind = EntryKind::Link;
            e.target = f[1];
            e.path = f[2];
            ok = linkProblem(e.path, e.target).empty();
        }
        if (!ok || !pathProblem(e.path).empty()) {
            *why = at + "not an entry line (`dir`, `file` or `link`, tab-separated, with a path a build may hold)";
            return false;
        }
        out->entries.push_back(e);
    }
    if (!ended) {
        *why = "the manifest has no `end` line: it is incomplete";
        return false;
    }
    if (!readFormats(formats, &out->formats, why)) {
        *why = "the manifest's " + *why;
        return false;
    }
    if (out->tree.empty() || treeHash(out->entries, leaveOutDrivers) != out->tree) {
        *why = "the manifest's tree is not the hash of the entries it lists";
        return false;
    }
    return true;
}

bool isHash(const std::string& s) {
    if (s.size() != 64) {
        return false;
    }
    for (char c : s) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
            return false;
        }
    }
    return true;
}

NameKind classify(const std::string& name) {
    auto starts = [&](const char* p) { return name.rfind(p, 0) == 0; };
    if (isHash(name)) {
        return NameKind::Build;
    }
    if (name == kActive) {
        return NameKind::Active;
    }
    if (starts(kTmpPrefix)) {
        return NameKind::Tmp;
    }
    if (starts(kPairTmpPrefix)) {
        return NameKind::PairTmp;
    }
    if (starts(kActiveTmpPrefix)) {
        return NameKind::ActiveTmp;
    }
    if (starts(kGonePrefix)) {
        return NameKind::Gone;
    }
    std::string c;
    std::string p;
    if (readPairName(name, &c, &p)) {
        return NameKind::Pair;
    }
    return NameKind::Other;
}

std::string pairName(const std::string& current, const std::string& previous) {
    return std::string(kPairPrefix) + current + "-" + (previous.empty() ? std::string("none") : previous);
}

bool readPairName(const std::string& name, std::string* current, std::string* previous) {
    const std::string pre = kPairPrefix;
    if (name.rfind(pre, 0) != 0 || name.size() < pre.size() + 64 + 1) {
        return false;
    }
    const std::string c = name.substr(pre.size(), 64);
    if (!isHash(c) || name[pre.size() + 64] != '-') {
        return false;
    }
    const std::string p = name.substr(pre.size() + 65);
    if (p != "none" && !isHash(p)) {
        return false;
    }
    *current = c;
    *previous = p == "none" ? std::string() : p;
    return true;
}

std::vector<std::string> pruneCandidates(std::vector<Placed> builds, const std::string& current,
                                         const std::string& previous, std::size_t keep) {
    std::vector<Placed> rest;
    for (const Placed& b : builds) {
        if (b.hash != current && b.hash != previous) {
            rest.push_back(b);
        }
    }
    // Newest first, ties by hash, so the choice is the same however the directory lists them.
    std::sort(rest.begin(), rest.end(), [](const Placed& a, const Placed& b) {
        return a.placedNs != b.placedNs ? a.placedNs > b.placedNs : a.hash < b.hash;
    });
    std::vector<std::string> out;
    for (std::size_t i = keep; i < rest.size(); ++i) {
        out.push_back(rest[i].hash);
    }
    std::reverse(out.begin(), out.end());   // oldest first
    return out;
}

std::string buildOfProgram(const std::string& path) {
    const std::string tail = std::string("/") + kProgram;
    if (path.size() < tail.size() + 64 + 1 + 7 ||
        path.compare(path.size() - tail.size(), tail.size(), tail) != 0) {
        return std::string();
    }
    const std::string folder = path.substr(0, path.size() - tail.size());
    const std::size_t slash = folder.rfind('/');
    if (slash == std::string::npos) {
        return std::string();
    }
    const std::string hash = folder.substr(slash + 1);
    const std::string parent = folder.substr(0, slash);
    const std::string builds = std::string("/") + kBuildsDir;
    if (!isHash(hash) || parent.size() < builds.size() ||
        parent.compare(parent.size() - builds.size(), builds.size(), builds) != 0) {
        return std::string();
    }
    return hash;
}

} // namespace nexus::server::builds
