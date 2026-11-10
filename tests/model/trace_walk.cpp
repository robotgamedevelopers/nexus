// Drives the real nexus::Model with a seeded random walk and writes the trace as a TLA+ module
// (WindowTraceData.tla) for models/WindowTrace.tla, under which TLC checks that the trace is a behaviour of
// models/Window.tla.
//
//   trace_walk <seed> <steps> <friends> [--faulty | --faulty-queue | --faulty-move | --faulty-close] <out.tla>
//
// The walk is a pure function of its arguments: its random numbers come from the seed (splitmix64), never from
// the clock or the OS. Each step calls one operation, with positions in range and out of it (move_tab's two among
// them, across zones too), drafts that are empty, all whitespace or text, and entries of every kind; it adds
// terminals (up to kMaxTerminals open) and closes tabs of both kinds. Each record holds the operation, its arguments,
// the value it returned (and the id add returned) and the model's whole state after it: the tabs' order (each
// position's Tab::id) and, by position, each tab's kind, friend, draft, transcript and queue.
//
// --faulty walks FaultyModel instead: a test-only subclass whose send accepts a draft that is all whitespace but
// not empty. --faulty-queue walks FaultyQueueModel: its queue also puts the draft in the transcript. --faulty-move
// walks FaultyMoveModel: its move_tab leaves active at the old position. --faulty-close walks FaultyCloseModel: its
// close_tab removes a friend's tab too. TLC must reject each one's traces; that is the negative check that shows the
// binding can fail.

#include "model.h"

#include <algorithm>
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

// The test-only queue fault: queue also appends the draft to the transcript, as a Person entry. Hides Model::queue.
class FaultyQueueModel : public Model {
public:
    using Model::Model;
    bool queue(std::size_t i) {
        const std::string text = i < tab_count() ? tab(i).draft : std::string();
        if (!Model::queue(i))
            return false;
        append(i, Entry{EntryKind::Person, text});
        return true;
    }
};

// The test-only move fault: move_tab reorders the tabs but leaves active at the old position, so another friend becomes
// active. Hides Model::move_tab.
class FaultyMoveModel : public Model {
public:
    using Model::Model;
    bool move_tab(std::size_t from, std::size_t to) {
        const std::size_t was = active();
        if (!Model::move_tab(from, to))
            return false;
        select_tab(was);
        return true;
    }
};

// The test-only close fault: close_tab takes out a friend's tab too, as if it were a terminal's. Hides Model::close_tab.
class FaultyCloseModel : public Model {
public:
    using Model::Model;
    bool close_tab(std::size_t i) {
        if (i >= tab_count() || tab(i).kind != nexus::TabKind::Friend)
            return Model::close_tab(i);
        tabs_.erase(tabs_.begin() + static_cast<std::ptrdiff_t>(i));
        if (active_ > i || (active_ == i && active_ == tabs_.size()))
            active_ = active_ == 0 ? 0 : active_ - 1;
        return true;
    }
};

template <class M>
constexpr const char* kFault = std::is_same_v<M, FaultyModel> ? ", faulty model"
                             : std::is_same_v<M, FaultyQueueModel> ? ", faulty queue model"
                             : std::is_same_v<M, FaultyMoveModel> ? ", faulty move model"
                             : std::is_same_v<M, FaultyCloseModel> ? ", faulty close model" : "";

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
    {EntryKind::Friend, "later"},
};
const std::vector<FriendInfo> kFriends = {{"ada", "m1"}, {"bo", "m2"}, {"cy", "m3"}};
const std::size_t kMaxTerminals = 3;   // the most terminal tabs open at once in a walk

const char* kind_name(EntryKind k) {
    switch (k) {
    case EntryKind::Person: return "Person";
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
    o << "order |-> <<";
    for (std::size_t j = 0; j < m.tab_count(); ++j)
        o << (j ? ", " : "") << m.tab(j).id;
    o << ">>, kind |-> <<";
    for (std::size_t j = 0; j < m.tab_count(); ++j)
        o << (j ? ", " : "") << (m.tab(j).kind == nexus::TabKind::Friend ? "\"Friend\"" : "\"Terminal\"");
    o << ">>, friends |-> <<";
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
    o << ">>, queue |-> <<";
    for (std::size_t j = 0; j < m.tab_count(); ++j) {
        o << (j ? ", " : "") << "<<";
        const auto& q = m.tab(j).queue;
        for (std::size_t k = 0; k < q.size(); ++k)
            o << (k ? ", " : "") << str(q[k]);
        o << ">>";
    }
    o << ">>";
    return o.str();
}

std::string record(const char* op, std::size_t i, std::size_t j, const std::string& text, const std::string& ent,
                   bool ret, std::size_t id, const std::string& st) {
    std::ostringstream o;
    o << "[op |-> \"" << op << "\", i |-> " << i << ", j |-> " << j << ", text |-> " << str(text) << ", entry |-> " << ent
      << ", ret |-> " << (ret ? "TRUE" : "FALSE") << ", id |-> " << id << ", " << st << "]";
    return o.str();
}

template <class M>
int walk(std::uint64_t seed, std::size_t steps, std::size_t friend_count, const char* path) {
    std::vector<FriendInfo> friends(kFriends.begin(), kFriends.begin() + static_cast<std::ptrdiff_t>(friend_count));
    M m(friends);
    Rng rng{seed};
    std::size_t last_index = friend_count + 1; // the largest position drawn: TraceIdx runs to it

    std::vector<std::string> records;
    records.push_back(record("init", 0, 0, "", kNoEntry, true, 0, state(m)));
    std::size_t accepted = 0;
    for (std::size_t s = 0; s < steps; ++s) {
        // every tab, and two positions past the last
        const std::size_t index_count = m.tab_count() + 2;
        const std::size_t i = rng.below(index_count);
        std::size_t j = 0;
        std::size_t id = 0;
        bool ret = false;
        const char* op = "";
        std::string text;
        std::string ent = kNoEntry;
        std::size_t kind = rng.below(8);
        // At most kMaxTerminals terminals at once: an add beyond that is a close instead.
        if (kind == 5 && m.tab_count() >= friend_count + kMaxTerminals)
            kind = 6;
        switch (kind) {
        case 0: op = "select"; ret = m.select_tab(i); break;
        case 1:
            op = "set_draft";
            text = kDrafts[rng.below(kDrafts.size())];
            ret = m.set_draft(i, text);
            break;
        case 2: op = "send"; ret = m.send(i); break;
        case 3: op = "queue"; ret = m.queue(i); break;
        case 4:
            op = "move";
            j = rng.below(index_count);
            ret = m.move_tab(i, j);
            break;
        case 5:
            op = "add";
            id = m.add_terminal();
            ret = true;
            break;
        case 6: op = "close"; ret = m.close_tab(i); break;
        default: {
            op = "append";
            const Entry& e = kEntries[rng.below(kEntries.size())];
            ent = entry(e);
            ret = m.append(i, e);
            break;
        }
        }
        accepted += ret ? 1 : 0;
        last_index = std::max(last_index, std::max(i, j));
        records.push_back(record(op, i, j, text, ent, ret, id, state(m)));
    }

    std::ofstream out(path);
    if (!out) {
        std::fprintf(stderr, "trace_walk: cannot write %s\n", path);
        return 1;
    }
    out << "---- MODULE WindowTraceData ----\n";
    out << "\\* Written by tests/model/trace_walk: seed " << seed << ", " << steps << " steps, " << friend_count
        << " friends" << kFault<M> << ".\n";
    out << "EXTENDS Integers\n";
    out << "TraceFriends == <<";
    for (std::size_t j = 0; j < friends.size(); ++j)
        out << (j ? ", " : "") << "[name |-> " << str(friends[j].name) << ", model |-> " << str(friends[j].model)
            << "]";
    out << ">>\n";
    out << "TraceIdx == 0.." << last_index << "\n";
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
                static_cast<unsigned long long>(seed), steps, friend_count, kFault<M>, accepted, steps - accepted, path);
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
    bool faultyQueue = false;
    bool faultyMove = false;
    bool faultyClose = false;
    std::vector<std::string> rest;
    for (const std::string& a : args) {
        if (a == "--faulty")
            faulty = true;
        else if (a == "--faulty-close")
            faultyClose = true;
        else if (a == "--faulty-queue")
            faultyQueue = true;
        else if (a == "--faulty-move")
            faultyMove = true;
        else
            rest.push_back(a);
    }
    std::uint64_t seed = 0, steps = 0, friend_count = 0;
    if (rest.size() != 4 || !parse(rest[0].c_str(), seed) || !parse(rest[1].c_str(), steps) ||
        !parse(rest[2].c_str(), friend_count) || friend_count > kFriends.size() ||
        int(faulty) + int(faultyQueue) + int(faultyMove) + int(faultyClose) > 1) {
        std::fprintf(stderr, "usage: trace_walk <seed> <steps> <friends 0..%zu> "
                     "[--faulty | --faulty-queue | --faulty-move | --faulty-close] <out.tla>\n", kFriends.size());
        return 2;
    }
    if (faultyClose)
        return walk<FaultyCloseModel>(seed, steps, friend_count, rest[3].c_str());
    if (faultyMove)
        return walk<FaultyMoveModel>(seed, steps, friend_count, rest[3].c_str());
    if (faultyQueue)
        return walk<FaultyQueueModel>(seed, steps, friend_count, rest[3].c_str());
    if (faulty)
        return walk<FaultyModel>(seed, steps, friend_count, rest[3].c_str());
    return walk<Model>(seed, steps, friend_count, rest[3].c_str());
}
