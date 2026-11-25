// Drives the real nexus::Model with a seeded random walk and writes the trace as a TLA+ module
// (WindowTraceData.tla) for models/WindowTrace.tla, under which TLC checks that the trace is a behaviour of
// models/Window.tla.
//
//   trace_walk <seed> <steps> <ais> [--no-sprint] [--ais-churn] [--faulty | --faulty-queue | --faulty-move |
//              --faulty-queue-cap | --faulty-send-trim | --faulty-send-dedup | --faulty-close |
//              --faulty-sprint-move | --faulty-sprint-close | --faulty-close-fallback | --faulty-remove-ai]
//              <out.tla>
//
// The model is made with the sprint tab first, as the window makes it, unless --no-sprint.
// With --ais-churn the AIs come and go as the server's list does (sync/client.h applyAIs): add_ai with
// each of kArrivals (TraceArrivals; a removed AI returning, a standing one and the empty name refused) and
// remove_ai at every position, among the other steps.
//
// The walk is a pure function of its arguments: its random numbers come from the seed (splitmix64), never from
// the clock or the OS. Each step calls one operation, with positions in range and out of it (move_tab's two among
// them, across zones too), drafts that are empty, all whitespace or text, and entries of every kind; it adds
// terminals (up to kMaxTerminals open), closes tabs of both kinds, and every 25th step takes a queue's oldest prompt
// (take_queued, the window's mirror's call when the server delivered it). Each record holds the operation, its
// arguments, the value it returned (and the id add returned) and the model's whole state after it: the tabs' order
// (each position's Tab::id), the selection history and, by position, each tab's kind, AI, draft, transcript and
// queue.
//
// --faulty walks FaultyModel instead: a test-only subclass whose send accepts a draft that is all whitespace but
// not empty. --faulty-queue walks FaultyQueueModel: its queue also puts the draft in the transcript. --faulty-move
// walks FaultyMoveModel: its move_tab leaves active at the old position. --faulty-queue-cap walks FaultyQueueCapModel:
// past four queued prompts its queue drops the oldest. --faulty-send-trim walks FaultySendTrimModel: its send strips
// trailing spaces. --faulty-send-dedup walks FaultySendDedupModel: a send of the text just sent appends nothing.
// --faulty-close walks FaultyCloseModel: its close_tab removes an AI's tab too. --faulty-sprint-move walks
// FaultySprintMoveModel: its move_tab puts the sprint tab in the AIs' zone. --faulty-sprint-close walks
// FaultySprintCloseModel: its close_tab removes the sprint tab too. --faulty-close-fallback walks
// FaultyCloseFallbackModel: closing the active tab ignores the selection history. --faulty-remove-ai (with
// --ais-churn) walks FaultyRemoveAIModel: its remove_ai leaves the tab's id in the selection history. TLC
// must reject each one's traces; that is the negative check that shows the binding can fail.

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
using nexus::AIInfo;
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

// The test-only move fault: move_tab reorders the tabs but leaves active at the old position, so another AI becomes
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

// The test-only close fault: close_tab takes out an AI's tab too, as if it were a terminal's. Hides Model::close_tab.
class FaultyCloseModel : public Model {
public:
    using Model::Model;
    bool close_tab(std::size_t i) {
        if (i >= tab_count() || tab(i).kind != nexus::TabKind::AI)
            return Model::close_tab(i);
        tabs_.erase(tabs_.begin() + static_cast<std::ptrdiff_t>(i));
        if (active_ > i || (active_ == i && active_ == tabs_.size()))
            active_ = active_ == 0 ? 0 : active_ - 1;
        return true;
    }
};

// The test-only sprint move fault: the move rule takes the sprint tab's zone for the AIs', so an AI dropped on
// position 0 goes in front of the sprint tab, and the sprint tab can be dragged among the AIs. Hides
// Model::move_tab.
class FaultySprintMoveModel : public Model {
public:
    using Model::Model;
    bool move_tab(std::size_t from, std::size_t to) {
        auto zoneOf = [this](std::size_t i) {
            return tab(i).kind == nexus::TabKind::Sprint ? nexus::zone(nexus::TabKind::AI) : nexus::zone(tab(i).kind);
        };
        if (from >= tab_count() || to >= tab_count() || zoneOf(from) != zoneOf(to))
            return false;
        if (zoneOf(from) == nexus::zone(tab(from).kind) && zoneOf(to) == nexus::zone(tab(to).kind))
            return Model::move_tab(from, to);
        auto at = [this](std::size_t i) { return tabs_.begin() + static_cast<std::ptrdiff_t>(i); };
        if (from < to)
            std::rotate(at(from), at(from + 1), at(to + 1));
        else if (to < from)
            std::rotate(at(to), at(from), at(from + 1));
        if (active_ == from)
            active_ = to;
        else if (from < active_ && active_ <= to)
            --active_;
        else if (to <= active_ && active_ < from)
            ++active_;
        return true;
    }
};

// The test-only sprint close fault: close_tab takes out the sprint tab too, as Command-W would if it checked only
// "not an AI's tab". Hides Model::close_tab.
class FaultySprintCloseModel : public Model {
public:
    using Model::Model;
    bool close_tab(std::size_t i) {
        if (i >= tab_count() || tab(i).kind != nexus::TabKind::Sprint)
            return Model::close_tab(i);
        tabs_.erase(tabs_.begin() + static_cast<std::ptrdiff_t>(i));
        if (active_ > i)
            --active_;
        else if (active_ == i && active_ == tabs_.size())
            active_ = tabs_.empty() ? 0 : tabs_.size() - 1;
        return true;
    }
};

// The test-only close fallback fault: closing the active tab ignores the selection history and keeps the rule before
// it, the tab now at its position or the last, so closing the last terminal goes back to the last AI. Hides
// Model::close_tab.
class FaultyCloseFallbackModel : public Model {
public:
    using Model::Model;
    bool close_tab(std::size_t i) {
        const bool was_active = i == active();
        if (!Model::close_tab(i))
            return false;
        if (was_active)
            active_ = i < tab_count() ? i : (tab_count() == 0 ? 0 : tab_count() - 1);
        return true;
    }
};

// The test-only remove-ai fault: remove_ai takes the tab out but leaves its id in the selection history, as if
// the history were the strip's alone to keep. Hides Model::remove_ai.
class FaultyRemoveAIModel : public Model {
public:
    using Model::Model;
    bool remove_ai(std::size_t i) {
        const std::vector<std::size_t> kept = history_;
        if (!Model::remove_ai(i))
            return false;
        history_ = kept;
        return true;
    }
};

// The test-only queue-length fault: past four queued prompts, the queue drops its oldest. Hides Model::queue.
class FaultyQueueCapModel : public Model {
public:
    using Model::Model;
    bool queue(std::size_t i) {
        if (!Model::queue(i))
            return false;
        // The fault writes the tab directly (it is not const): the drop is part of the queue, not a take.
        std::vector<std::string>& q = const_cast<nexus::Tab&>(tab(i)).queue;
        if (q.size() > 4)
            q.erase(q.begin());
        return true;
    }
};

// The test-only trim fault: send strips trailing spaces from the draft. Hides Model::send.
class FaultySendTrimModel : public Model {
public:
    using Model::Model;
    bool send(std::size_t i) {
        if (i < tab_count()) {
            std::string draft = tab(i).draft;
            while (!draft.empty() && draft.back() == ' ')
                draft.pop_back();
            set_draft(i, draft);
        }
        return Model::send(i);
    }
};

// The test-only repeat fault: a send whose text equals the tab's last Person entry clears the draft and returns true
// but appends nothing. Hides Model::send.
class FaultySendDedupModel : public Model {
public:
    using Model::Model;
    bool send(std::size_t i) {
        if (i < tab_count() && !tab(i).transcript.empty() && tab(i).transcript.back().kind == EntryKind::Person &&
                tab(i).transcript.back().text == tab(i).draft) {
            set_draft(i, "");
            return true;
        }
        return Model::send(i);
    }
};

template <class M>
constexpr const char* kFault = std::is_same_v<M, FaultyModel> ? ", faulty model"
                             : std::is_same_v<M, FaultyQueueModel> ? ", faulty queue model"
                             : std::is_same_v<M, FaultyMoveModel> ? ", faulty move model"
                             : std::is_same_v<M, FaultyQueueCapModel> ? ", faulty queue-length model"
                             : std::is_same_v<M, FaultySendTrimModel> ? ", faulty send-trim model"
                             : std::is_same_v<M, FaultySendDedupModel> ? ", faulty send-dedup model"
                             : std::is_same_v<M, FaultyCloseModel> ? ", faulty close model"
                             : std::is_same_v<M, FaultySprintMoveModel> ? ", faulty sprint-move model"
                             : std::is_same_v<M, FaultySprintCloseModel> ? ", faulty sprint-close model"
                             : std::is_same_v<M, FaultyCloseFallbackModel> ? ", faulty close-fallback model"
                             : std::is_same_v<M, FaultyRemoveAIModel> ? ", faulty remove-ai model" : "";

const char* tab_kind_name(nexus::TabKind k) {
    switch (k) {
    case nexus::TabKind::Extension: return "\"Extension\"";
    case nexus::TabKind::Sprint: return "\"Sprint\"";
    case nexus::TabKind::AI: return "\"AI\"";
    case nexus::TabKind::Terminal: return "\"Terminal\"";
    }
    return "\"?\"";
}

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

// The argument domains. TraceDrafts, TraceBlank and TraceEntries in the trace are these. Text with whitespace before,
// after and around it shows a send or queue that trims; drawing from few texts repeats equal sends.
const std::vector<std::string> kDrafts = {"", " ", " \t\n", "\r\f", "\v", "hi", "two words", "say \"hi\" \\ ok",
    " hi", "hi ", "\thi \n", "hi  "};
const std::vector<std::string> kBlank = {"", " ", " \t\n", "\r\f", "\v"};
const std::vector<Entry> kEntries = {
    {EntryKind::AI, "ok"},
    {EntryKind::Notice, ""},
    {EntryKind::Person, " "},
    {EntryKind::AI, "later"},
};
const std::vector<AIInfo> kAIs = {{"ada", "m1"}, {"bo", "m2"}, {"cy", "m3"}};
// The AIs add_ai is tried with when the AIs churn (TraceArrivals): those of the start, so that a removed
// one comes back and a standing one is refused, one more, and the empty name, refused.
const std::vector<AIInfo> kArrivals = {{"ada", "m1"}, {"bo", "m2"}, {"cy", "m3"}, {"dee", ""}, {"", ""}};
const std::size_t kMaxTerminals = 3;   // the most terminal tabs open at once in a walk

const char* kind_name(EntryKind k) {
    switch (k) {
    case EntryKind::Person: return "Person";
    case EntryKind::AI: return "AI";
    case EntryKind::Notice: return "Notice";
    case EntryKind::Tool: return "Tool";
    case EntryKind::Bus: return "Bus";
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
        o << (j ? ", " : "") << tab_kind_name(m.tab(j).kind);
    o << ">>, history |-> <<";
    const auto& h = m.selection_history();
    for (std::size_t k = 0; k < h.size(); ++k)
        o << (k ? ", " : "") << h[k];
    o << ">>, ais |-> <<";
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

std::string aiRecord(const AIInfo& f) {
    return "[name |-> " + str(f.name) + ", model |-> " + str(f.model) + "]";
}

std::string record(const char* op, std::size_t i, std::size_t j, const std::string& text, const std::string& ent,
                   bool ret, std::size_t id, const std::string& st, const AIInfo& f = AIInfo{}) {
    std::ostringstream o;
    o << "[op |-> \"" << op << "\", i |-> " << i << ", j |-> " << j << ", text |-> " << str(text) << ", entry |-> " << ent
      << ", ai |-> " << aiRecord(f) << ", ret |-> " << (ret ? "TRUE" : "FALSE") << ", id |-> " << id << ", "
      << st << "]";
    return o.str();
}

template <class M>
int walk(std::uint64_t seed, std::size_t steps, std::size_t ai_count, bool sprint, bool churn, const char* path) {
    std::vector<AIInfo> ais(kAIs.begin(), kAIs.begin() + static_cast<std::ptrdiff_t>(ai_count));
    M m(ais, sprint);
    Rng rng{seed};
    // The tabs no close can take out; with the AIs churning, the most AIs' tabs at once is every arrival's.
    const std::size_t fixed_tabs = (churn ? std::max(ai_count, kArrivals.size() - 1) : ai_count) +
        (sprint ? 1 : 0);
    std::size_t last_index = fixed_tabs + 1; // the largest position drawn: TraceIdx runs to it

    std::vector<std::string> records;
    records.push_back(record("init", 0, 0, "", kNoEntry, true, 0, state(m)));
    std::size_t accepted = 0;
    std::size_t longest = 0;    // the longest queue the walk reached
    std::size_t sentTab = 0;    // where the last accepted send went, and its text: for sending the same text again
    std::string sentText;
    std::size_t queuedTab = 0;  // and the same for the last accepted queue
    std::string queuedText;
    for (std::size_t s = 0; s < steps; ++s) {
        // every tab, and two positions past the last
        const std::size_t index_count = m.tab_count() + 2;
        std::size_t i = rng.below(index_count);
        std::size_t j = 0;
        std::size_t id = 0;
        bool ret = false;
        const char* op = "";
        std::string text;
        std::string ent = kNoEntry;
        // With the AIs churning, two more kinds: add_ai (10) and remove_ai (11).
        std::size_t kind = rng.below(churn ? 12 : 10);
        // At most kMaxTerminals terminals at once: an add beyond that is a close instead.
        std::size_t terminals = 0;
        for (std::size_t t = 0; t < m.tab_count(); ++t)
            terminals += m.tab(t).kind == nexus::TabKind::Terminal ? 1 : 0;
        if (kind == 5 && terminals >= kMaxTerminals)
            kind = 8;
        AIInfo arrival;
        switch (kind) {
        case 10:
            op = "add_ai";
            arrival = kArrivals[rng.below(kArrivals.size())];
            ret = m.add_ai(arrival);
            // The id of the AI's new tab: the AI's names are each on one tab.
            for (std::size_t t = 0; ret && t < m.tab_count(); ++t) {
                if (m.tab(t).kind == nexus::TabKind::AI && m.tab(t).name == arrival.name)
                    id = m.tab(t).id;
            }
            i = 0;
            break;
        case 11:
            op = "remove_ai";
            id = i < m.tab_count() && m.tab(i).kind == nexus::TabKind::AI ? m.tab(i).id : 0;
            ret = m.remove_ai(i);
            if (!ret)
                id = 0;
            break;
        case 0: op = "select"; ret = m.select_tab(i); break;
        case 1:
            op = "set_draft";
            text = kDrafts[rng.below(kDrafts.size())];
            ret = m.set_draft(i, text);
            break;
        case 6:
            // The same text again, to the same tab, at once: set_draft as its own step, then the send below.
            i = sentTab;
            ret = m.set_draft(i, sentText);
            last_index = std::max(last_index, i);
            records.push_back(record("set_draft", i, 0, sentText, kNoEntry, ret, 0, state(m)));
            accepted += ret ? 1 : 0;
            ++s;
            [[fallthrough]];
        case 2:
            op = "send";
            ret = m.send(i);
            if (ret) {
                sentTab = i;
                sentText = m.tab(i).transcript.back().text;
            }
            break;
        case 7:
            // The same prompt queued again, to the same tab: queues grow long.
            i = queuedTab;
            ret = m.set_draft(i, queuedText);
            last_index = std::max(last_index, i);
            records.push_back(record("set_draft", i, 0, queuedText, kNoEntry, ret, 0, state(m)));
            accepted += ret ? 1 : 0;
            ++s;
            [[fallthrough]];
        case 3:
            op = "queue";
            ret = m.queue(i);
            if (ret) {
                queuedTab = i;
                queuedText = m.tab(i).queue.back();
            }
            break;
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
        case 8: op = "close"; ret = m.close_tab(i); break;
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
        for (std::size_t t = 0; t < m.tab_count(); ++t)
            longest = std::max(longest, m.tab(t).queue.size());
        records.push_back(record(op, i, j, text, ent, ret, id, state(m), arrival));
        // Every 25th step, the oldest queued prompt of the tab at that position leaves (the server delivered it): a
        // step of its own that draws no random number, so every walk's other steps are as they were, and seldom
        // enough that queues still grow long.
        if (s % 25 == 24) {
            const bool took = m.take_queued(i);
            accepted += took ? 1 : 0;
            records.push_back(record("take", i, 0, "", kNoEntry, took, 0, state(m)));
        }
    }

    std::ofstream out(path);
    if (!out) {
        std::fprintf(stderr, "trace_walk: cannot write %s\n", path);
        return 1;
    }
    out << "---- MODULE WindowTraceData ----\n";
    out << "\\* Written by tests/model/trace_walk: seed " << seed << ", " << steps << " steps, " << ai_count
        << " AIs" << (sprint ? "" : ", no sprint tab") << (churn ? ", AIs churning" : "") << kFault<M>
        << ".\n";
    out << "EXTENDS Integers\n";
    out << "TraceSprint == " << (sprint ? "TRUE" : "FALSE") << "\n";
    out << "TraceAIs == <<";
    for (std::size_t j = 0; j < ais.size(); ++j)
        out << (j ? ", " : "") << "[name |-> " << str(ais[j].name) << ", model |-> " << str(ais[j].model)
            << "]";
    out << ">>\n";
    out << "TraceArrivals == {";
    if (churn) {
        for (std::size_t k = 0; k < kArrivals.size(); ++k)
            out << (k ? ", " : "") << aiRecord(kArrivals[k]);
    }
    out << "}\n";
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
    std::printf("trace_walk: seed %llu, %zu steps, %zu AIs%s%s%s: %zu accepted, %zu refused, longest queue %zu -> "
                "%s\n", static_cast<unsigned long long>(seed), records.size() - 1, ai_count,
                sprint ? "" : ", no sprint tab", churn ? ", AIs churning" : "", kFault<M>, accepted,
                records.size() - 1 - accepted, longest, path);
    return 0;
}

bool parse(const char* s, std::uint64_t& v) {
    char* end = nullptr;
    v = std::strtoull(s, &end, 10);
    return end != s && *end == '\0';
}

int pluginTabs(const char* path) {
    nexus::Model model({});
    std::vector<nexus::TabGroup> groups{{"writing", {"editor"}}, {"tools", {"counter"}}};
    model.set_tab_groups(groups);
    std::ofstream out(path);
    out << "---- MODULE PluginTabTraceData ----\nTrace == <<";
    auto record = [&](const char* op, std::size_t a, std::size_t b) {
        out << "[op |-> \"" << op << "\", a |-> " << a << ", b |-> " << b << ", tabs |-> <<";
        for (std::size_t i = 0; i < model.tab_count(); ++i) {
            if (i) out << ",";
            out << model.tab(i).id + 1;
        }
        out << ">>, groups |-> <<";
        for (std::size_t i = 0; i < model.tab_count(); ++i) {
            if (i) out << ",";
            out << "\"" << model.tab(i).group << "\"";
        }
        out << ">>]";
    };
    record("init", 0, 0);
    for (const auto group : {"writing", "tools", "writing"}) {
        auto id = model.open_extension(std::string(group) == "writing" ? "editor" : "counter", group, "title");
        out << ",\n";
        record("open", id + 1, 0);
    }
    for (auto to : {1u, 2u}) {
        model.move_tab(0, to);
        out << ",\n";
        record("move", 1, to + 1);
    }
    groups[0].acceptsFrom = {"tools"};
    model.set_tab_groups(groups);
    out << ",\n";
    record("reload", 1, 0);
    model.move_tab(2, 0);
    out << ",\n";
    record("move", 3, 1);
    groups[0].acceptsFrom.clear();
    model.set_tab_groups(groups);
    out << ",\n";
    record("reload", 0, 0);
    model.close_tab(0);
    out << ",\n";
    record("close", 1, 0);
    out << ">>\n====\n";
    out.close();
    return out ? 0 : 1;
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 3 && std::string(argv[1]) == "--plugin-tabs")
        return pluginTabs(argv[2]);
    std::vector<std::string> args(argv + 1, argv + argc);
    const std::vector<std::string> faults = {"--faulty", "--faulty-queue", "--faulty-move", "--faulty-queue-cap",
        "--faulty-send-trim", "--faulty-send-dedup", "--faulty-close", "--faulty-sprint-move", "--faulty-sprint-close",
        "--faulty-close-fallback", "--faulty-remove-ai"};
    std::string fault;
    int faultCount = 0;
    bool sprint = true;
    bool churn = false;
    std::vector<std::string> rest;
    for (const std::string& a : args) {
        if (a == "--no-sprint") {
            sprint = false;
        } else if (a == "--ais-churn") {
            churn = true;
        } else if (std::find(faults.begin(), faults.end(), a) != faults.end()) {
            fault = a;
            ++faultCount;
        } else {
            rest.push_back(a);
        }
    }
    std::uint64_t seed = 0, steps = 0, ai_count = 0;
    if (rest.size() != 4 || !parse(rest[0].c_str(), seed) || !parse(rest[1].c_str(), steps) ||
        !parse(rest[2].c_str(), ai_count) || ai_count > kAIs.size() || faultCount > 1 ||
        (fault == "--faulty-remove-ai" && !churn)) {
        std::fprintf(stderr, "usage: trace_walk <seed> <steps> <ais 0..%zu> [--no-sprint] [--ais-churn] "
                     "[--faulty | --faulty-queue | --faulty-move | --faulty-queue-cap | --faulty-send-trim | "
                     "--faulty-send-dedup | --faulty-close | --faulty-sprint-move | --faulty-sprint-close | "
                     "--faulty-close-fallback | --faulty-remove-ai (with --ais-churn)] <out.tla>\n",
                     kAIs.size());
        return 2;
    }
    const char* out = rest[3].c_str();
    if (fault == "--faulty-remove-ai")
        return walk<FaultyRemoveAIModel>(seed, steps, ai_count, sprint, churn, out);
    if (fault == "--faulty-close-fallback")
        return walk<FaultyCloseFallbackModel>(seed, steps, ai_count, sprint, churn, out);
    if (fault == "--faulty-sprint-move")
        return walk<FaultySprintMoveModel>(seed, steps, ai_count, sprint, churn, out);
    if (fault == "--faulty-sprint-close")
        return walk<FaultySprintCloseModel>(seed, steps, ai_count, sprint, churn, out);
    if (fault == "--faulty-close")
        return walk<FaultyCloseModel>(seed, steps, ai_count, sprint, churn, out);
    if (fault == "--faulty-send-dedup")
        return walk<FaultySendDedupModel>(seed, steps, ai_count, sprint, churn, out);
    if (fault == "--faulty-queue-cap")
        return walk<FaultyQueueCapModel>(seed, steps, ai_count, sprint, churn, out);
    if (fault == "--faulty-send-trim")
        return walk<FaultySendTrimModel>(seed, steps, ai_count, sprint, churn, out);
    if (fault == "--faulty-move")
        return walk<FaultyMoveModel>(seed, steps, ai_count, sprint, churn, out);
    if (fault == "--faulty-queue")
        return walk<FaultyQueueModel>(seed, steps, ai_count, sprint, churn, out);
    if (fault == "--faulty")
        return walk<FaultyModel>(seed, steps, ai_count, sprint, churn, out);
    return walk<Model>(seed, steps, ai_count, sprint, churn, out);
}
