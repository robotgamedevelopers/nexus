// Drives the real nexus::Model with a seeded random walk and writes the trace as a TLA+ module
// (WindowTraceData.tla) for models/WindowTrace.tla, under which TLC checks that the trace is a behaviour of
// models/Window.tla.
//
//   trace_walk <seed> <steps> <friends> [--faulty] <out.tla>
//
// The walk is a pure function of its arguments: its random numbers come from the seed (splitmix64), never from
// the clock or the OS. Each step calls one operation, with indices in range and out of it, drafts that are empty,
// all whitespace or text, and entries of every kind. Each record holds the operation, its arguments, the value it
// returned and the model's whole state after it.
//
// --faulty walks FaultyModel instead: a test-only subclass whose send accepts a draft that is all whitespace but
// not empty. TLC must reject its traces; that is the negative check that shows the binding can fail.

#include "model.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <type_traits>
#include <vector>

namespace {

using nexus::Entry;
using nexus::EntryKind;
using nexus::FriendInfo;
using nexus::Model;

// The test-only fault. Hides Model::send; the walk is a template, so it calls this one.
class FaultyModel : public Model {
public:
    using Model::Model;
    bool send(std::size_t i) {
        if (i < tab_count() && !tab(i).draft.empty()) {
            std::string text = tab(i).draft;
            append(i, Entry{EntryKind::Person, text});
            set_draft(i, "");
            return true;
        }
        return Model::send(i);
    }
};

struct Rng {
    std::uint64_t state;
    std::uint64_t next() {
        std::uint64_t z = (state += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    std::size_t below(std::size_t n) { return static_cast<std::size_t>(next() % n); }
};

// The argument domains. TraceDrafts, TraceBlank and TraceEntries in the trace are these.
const std::vector<std::string> kDrafts = {"", " ", " \t\n", "\r\f", "hi", "two words", "say \"hi\" \\ ok"};
const std::vector<std::string> kBlank = {"", " ", " \t\n", "\r\f"};
const std::vector<Entry> kEntries = {
    {EntryKind::Friend, "ok"},
    {EntryKind::Notice, ""},
    {EntryKind::Person, " "},
    {EntryKind::Queued, "later"},
};
const std::vector<FriendInfo> kFriends = {{"ada", "m1"}, {"bo", "m2"}, {"cy", "m3"}};

const char* kind_name(EntryKind k) {
    switch (k) {
    case EntryKind::Person: return "Person";
    case EntryKind::Queued: return "Queued";
    case EntryKind::Friend: return "Friend";
    case EntryKind::Notice: return "Notice";
    }
    return "?";
}

std::string str(const std::string& s) {
    std::string out = "\"";
    for (char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\t': out += "\\t"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\f': out += "\\f"; break;
        default: out += c; break;
        }
    }
    return out + "\"";
}

std::string entry(const Entry& e) {
    return std::string("[kind |-> \"") + kind_name(e.kind) + "\", text |-> " + str(e.text) + "]";
}

const char* const kNoEntry = "[kind |-> \"None\", text |-> \"\"]";

template <class M>
std::string state(const M& m) {
    std::ostringstream o;
    o << "friends |-> <<";
    for (std::size_t j = 0; j < m.tab_count(); ++j)
        o << (j ? ", " : "") << "[name |-> " << str(m.tab(j).name) << ", model |-> " << str(m.tab(j).model) << "]";
    o << ">>, active |-> " << m.active() << ", draft |-> <<";
    for (std::size_t j = 0; j < m.tab_count(); ++j)
        o << (j ? ", " : "") << str(m.tab(j).draft);
    o << ">>, transcript |-> <<";
    for (std::size_t j = 0; j < m.tab_count(); ++j) {
        o << (j ? ", " : "") << "<<";
        const auto& t = m.tab(j).transcript;
        for (std::size_t k = 0; k < t.size(); ++k)
            o << (k ? ", " : "") << entry(t[k]);
        o << ">>";
    }
    o << ">>";
    return o.str();
}

std::string record(const char* op, std::size_t i, const std::string& text, const std::string& ent, bool ret,
                   const std::string& st) {
    std::ostringstream o;
    o << "[op |-> \"" << op << "\", i |-> " << i << ", text |-> " << str(text) << ", entry |-> " << ent
      << ", ret |-> " << (ret ? "TRUE" : "FALSE") << ", " << st << "]";
    return o.str();
}

template <class M>
int walk(std::uint64_t seed, std::size_t steps, std::size_t friend_count, const char* path) {
    std::vector<FriendInfo> friends(kFriends.begin(), kFriends.begin() + static_cast<std::ptrdiff_t>(friend_count));
    M m(friends);
    Rng rng{seed};
    const std::size_t index_count = friend_count + 2; // every tab, and two indices past the last

    std::vector<std::string> records;
    records.push_back(record("init", 0, "", kNoEntry, true, state(m)));
    std::size_t accepted = 0;
    for (std::size_t s = 0; s < steps; ++s) {
        const std::size_t i = rng.below(index_count);
        bool ret = false;
        const char* op = "";
        std::string text;
        std::string ent = kNoEntry;
        switch (rng.below(5)) {
        case 0: op = "select"; ret = m.select_tab(i); break;
        case 1:
            op = "set_draft";
            text = kDrafts[rng.below(kDrafts.size())];
            ret = m.set_draft(i, text);
            break;
        case 2: op = "send"; ret = m.send(i); break;
        case 3: op = "queue"; ret = m.queue(i); break;
        default: {
            op = "append";
            const Entry& e = kEntries[rng.below(kEntries.size())];
            ent = entry(e);
            ret = m.append(i, e);
            break;
        }
        }
        accepted += ret ? 1 : 0;
        records.push_back(record(op, i, text, ent, ret, state(m)));
    }

    std::ofstream out(path);
    if (!out) {
        std::fprintf(stderr, "trace_walk: cannot write %s\n", path);
        return 1;
    }
    out << "---- MODULE WindowTraceData ----\n";
    out << "\\* Written by tests/model/trace_walk: seed " << seed << ", " << steps << " steps, " << friend_count
        << " friends" << (std::is_same_v<M, FaultyModel> ? ", FAULTY model" : "") << ".\n";
    out << "EXTENDS Integers\n";
    out << "TraceFriends == <<";
    for (std::size_t j = 0; j < friends.size(); ++j)
        out << (j ? ", " : "") << "[name |-> " << str(friends[j].name) << ", model |-> " << str(friends[j].model)
            << "]";
    out << ">>\n";
    out << "TraceIdx == 0.." << index_count - 1 << "\n";
    out << "TraceDrafts == {";
    for (std::size_t k = 0; k < kDrafts.size(); ++k)
        out << (k ? ", " : "") << str(kDrafts[k]);
    out << "}\nTraceBlank == {";
    for (std::size_t k = 0; k < kBlank.size(); ++k)
        out << (k ? ", " : "") << str(kBlank[k]);
    out << "}\nTraceEntries == {";
    for (std::size_t k = 0; k < kEntries.size(); ++k)
        out << (k ? ", " : "") << entry(kEntries[k]);
    out << "}\nTrace == <<\n";
    for (std::size_t k = 0; k < records.size(); ++k)
        out << "  " << records[k] << (k + 1 < records.size() ? ",\n" : "\n");
    out << ">>\n====\n";
    out.close();
    if (!out) {
        std::fprintf(stderr, "trace_walk: writing %s failed\n", path);
        return 1;
    }
    std::printf("trace_walk: seed %llu, %zu steps, %zu friends%s: %zu accepted, %zu refused -> %s\n",
                static_cast<unsigned long long>(seed), steps, friend_count,
                std::is_same_v<M, FaultyModel> ? ", faulty model" : "", accepted, steps - accepted, path);
    return 0;
}

bool parse(const char* s, std::uint64_t& v) {
    char* end = nullptr;
    v = std::strtoull(s, &end, 10);
    return end != s && *end == '\0';
}

} // namespace

int main(int argc, char** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);
    bool faulty = false;
    std::vector<std::string> rest;
    for (const std::string& a : args) {
        if (a == "--faulty")
            faulty = true;
        else
            rest.push_back(a);
    }
    std::uint64_t seed = 0, steps = 0, friend_count = 0;
    if (rest.size() != 4 || !parse(rest[0].c_str(), seed) || !parse(rest[1].c_str(), steps) ||
        !parse(rest[2].c_str(), friend_count) || friend_count > kFriends.size()) {
        std::fprintf(stderr, "usage: trace_walk <seed> <steps> <friends 0..%zu> [--faulty] <out.tla>\n",
                     kFriends.size());
        return 2;
    }
    if (faulty)
        return walk<FaultyModel>(seed, steps, friend_count, rest[3].c_str());
    return walk<Model>(seed, steps, friend_count, rest[3].c_str());
}
