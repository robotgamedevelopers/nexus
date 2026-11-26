// Drives the real nexus::Model with a seeded random walk and writes the trace as a TLA+ module
// (WindowTraceData.tla) for models/WindowTrace.tla, under which TLC checks that the trace is a behaviour of
// models/Window.tla.
//
//   trace_walk <seed> <steps> <ais> [--boards <0..3>] [--ais-churn] [--boards-churn] [--faulty | --faulty-queue |
//              --faulty-move | --faulty-queue-cap | --faulty-send-trim | --faulty-send-dedup | --faulty-close |
//              --faulty-board-move | --faulty-board-close | --faulty-close-fallback | --faulty-remove-ai |
//              --faulty-add-board | --faulty-remove-board | --faulty-sync-keep] <out.tla>
//
// The model is made with the dashboards of kBoardLists[--boards] first (1 unless given: one dashboard, as a server
// with one dashboard line makes the window's strip; 0 is the window's model before the server's list arrives; 3 is a
// list with an empty and a repeated name, which the constructor skips), then the walk's AIs.
// With --ais-churn the AIs come and go as the server's list does (sync/client.h applyAIs): add_ai with
// each of kArrivals (TraceArrivals; a removed AI returning, a standing one and the empty name refused) and
// remove_ai at every position, among the other steps. With --boards-churn the dashboards do (Model::sync_dashboards):
// add_dashboard with each of kBoardArrivals (TraceBoardArrivals) and remove_dashboard at every position, and every
// 50th step a sync_dashboards to a list drawn from them (its removes and adds recorded as the steps they are, and the
// walk fails if the model does not end as those steps leave it, or if the strip's dashboards are then not the list's).
//
// The walk is a pure function of its arguments: its random numbers come from the seed (splitmix64), never from
// the clock or the OS. Each step calls one operation, with positions in range and out of it (move_tab's two among
// them, across zones too), drafts that are empty, all whitespace or text, and entries of every kind; it adds
// terminals (up to kMaxTerminals open), closes tabs of every kind, and every 25th step takes a queue's oldest prompt
// (take_queued, the window's mirror's call when the server delivered it). Each record holds the operation, its
// arguments, the value it returned (and the id add returned) and the model's whole state after it: the tabs' order
// (each position's Tab::id), the selection history and, by position, each tab's kind, name and model, draft,
// transcript and queue.
//
// --faulty walks FaultyModel instead: a test-only subclass whose send accepts a draft that is all whitespace but
// not empty. --faulty-queue walks FaultyQueueModel: its queue also puts the draft in the transcript. --faulty-move
// walks FaultyMoveModel: its move_tab leaves active at the old position. --faulty-queue-cap walks FaultyQueueCapModel:
// past four queued prompts its queue drops the oldest. --faulty-send-trim walks FaultySendTrimModel: its send strips
// trailing spaces. --faulty-send-dedup walks FaultySendDedupModel: a send of the text just sent appends nothing.
// --faulty-close walks FaultyCloseModel: its close_tab removes an AI's tab too. --faulty-board-move walks
// FaultyBoardMoveModel: its move_tab puts a dashboard in the AIs' zone. --faulty-board-close walks
// FaultyBoardCloseModel: its close_tab removes a dashboard's tab too. --faulty-close-fallback walks
// FaultyCloseFallbackModel: closing the active tab ignores the selection history. --faulty-remove-ai (with
// --ais-churn) walks FaultyRemoveAIModel: its remove_ai leaves the tab's id in the selection history.
// --faulty-add-board (with --boards-churn) walks FaultyAddBoardModel: its add_dashboard puts the tab at the AIs' end.
// --faulty-remove-board (with --boards-churn) walks FaultyRemoveBoardModel: its remove_dashboard leaves the tab's id in
// the selection history. TLC must reject each one's traces; that is the negative check that shows the binding can
// fail. --faulty-sync-keep (with --boards-churn) walks FaultySyncKeepModel: its sync_dashboards adds the list's new
// names but removes none. Its steps are each a lawful add, so TLC could not see it: the walk itself fails at the first
// sync whose list drops a dashboard (the check after each sync below), which ctest's
// trace_walk_sync_keep_refused requires.

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

// The test-only dashboard move fault: the move rule takes the dashboards' zone for the AIs', so an AI dropped on a
// dashboard goes in front of it, and a dashboard can be dragged among the AIs. Hides Model::move_tab.
class FaultyBoardMoveModel : public Model {
public:
    using Model::Model;
    bool move_tab(std::size_t from, std::size_t to) {
        auto zoneOf = [this](std::size_t i) {
            return tab(i).kind == nexus::TabKind::Dashboard ? nexus::zone(nexus::TabKind::AI)
                                                            : nexus::zone(tab(i).kind);
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

// The test-only dashboard close fault: close_tab takes out a dashboard's tab too, as Command-W would if it checked
// only "not an AI's tab". Hides Model::close_tab.
class FaultyBoardCloseModel : public Model {
public:
    using Model::Model;
    bool close_tab(std::size_t i) {
        if (i >= tab_count() || tab(i).kind != nexus::TabKind::Dashboard)
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

// The test-only add-dashboard fault: add_dashboard puts the new tab at the end of the AIs' zone (add_ai's place) in
// place of the dashboards' end, so it stands behind the AIs. Hides Model::add_dashboard.
class FaultyAddBoardModel : public Model {
public:
    using Model::Model;
    bool add_dashboard(std::string name) {
        if (!Model::add_dashboard(std::move(name)))
            return false;
        std::size_t at = 0;
        std::size_t end = 0;
        for (std::size_t t = 0; t < tab_count(); ++t) {
            if (tab(t).id == next_id_ - 1)
                at = t;
            if (tab(t).kind == nexus::TabKind::Dashboard || tab(t).kind == nexus::TabKind::AI)
                end = t;
        }
        reorder(at, end);
        return true;
    }
};

// The test-only remove-dashboard fault: remove_dashboard takes the tab out but leaves its id in the selection history.
// Hides Model::remove_dashboard.
class FaultyRemoveBoardModel : public Model {
public:
    using Model::Model;
    bool remove_dashboard(std::size_t i) {
        const std::vector<std::size_t> kept = history_;
        if (!Model::remove_dashboard(i))
            return false;
        history_ = kept;
        return true;
    }
};

// The test-only sync fault: sync_dashboards adds the list's new names but never removes a dashboard the list dropped.
// Hides Model::sync_dashboards.
class FaultySyncKeepModel : public Model {
public:
    using Model::Model;
    DashboardChanges sync_dashboards(const std::vector<std::string>& names) {
        DashboardChanges changes;
        for (const std::string& name : names) {
            if (add_dashboard(name))
                changes.added.push_back(name);
        }
        return changes;
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
                             : std::is_same_v<M, FaultyBoardMoveModel> ? ", faulty board-move model"
                             : std::is_same_v<M, FaultyBoardCloseModel> ? ", faulty board-close model"
                             : std::is_same_v<M, FaultyAddBoardModel> ? ", faulty add-board model"
                             : std::is_same_v<M, FaultyRemoveBoardModel> ? ", faulty remove-board model"
                             : std::is_same_v<M, FaultySyncKeepModel> ? ", faulty sync-keep model"
                             : std::is_same_v<M, FaultyCloseFallbackModel> ? ", faulty close-fallback model"
                             : std::is_same_v<M, FaultyRemoveAIModel> ? ", faulty remove-ai model" : "";

const char* tab_kind_name(nexus::TabKind k) {
    switch (k) {
    case nexus::TabKind::Extension: return "\"Extension\"";
    case nexus::TabKind::Dashboard: return "\"Dashboard\"";
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
// The constructor's dashboards, by --boards: none (the window's model before the server's list arrives), one, two,
// and three among an empty and a repeated name, which make no tab (Window.tla's Unique).
const std::vector<std::vector<std::string>> kBoardLists = {
    {}, {"sprint"}, {"sprint", "fleet"}, {"sprint", "", "fleet", "sprint", "CI runs"}};
// The names add_dashboard is tried with when the dashboards churn (TraceBoardArrivals): those of the start, so that a
// removed one comes back and a standing one is refused, one more, and the empty name, refused.
const std::vector<std::string> kBoardArrivals = {"sprint", "fleet", "CI runs", "notes", ""};

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
                   bool ret, std::size_t id, const std::string& st, const AIInfo& f = AIInfo{},
                   const std::string& board = {}) {
    std::ostringstream o;
    o << "[op |-> \"" << op << "\", i |-> " << i << ", j |-> " << j << ", text |-> " << str(text) << ", entry |-> " << ent
      << ", ai |-> " << aiRecord(f) << ", board |-> " << str(board) << ", ret |-> " << (ret ? "TRUE" : "FALSE")
      << ", id |-> " << id << ", " << st << "]";
    return o.str();
}

// The dashboards' names standing in m, in strip order.
template <class M>
std::vector<std::string> board_names(const M& m) {
    std::vector<std::string> names;
    for (std::size_t t = 0; t < m.tab_count(); ++t) {
        if (m.tab(t).kind == nexus::TabKind::Dashboard)
            names.push_back(m.tab(t).name);
    }
    return names;
}

template <class M>
int walk(std::uint64_t seed, std::size_t steps, std::size_t ai_count, std::size_t board_list, bool churn,
         bool board_churn, const char* path) {
    std::vector<AIInfo> ais(kAIs.begin(), kAIs.begin() + static_cast<std::ptrdiff_t>(ai_count));
    const std::vector<std::string>& boards = kBoardLists[board_list];
    M m(ais, boards);
    Rng rng{seed};
    // The tabs no close can take out; with the AIs churning, the most AIs' tabs at once is every arrival's, and with
    // the dashboards churning, the most dashboards' tabs every named arrival's.
    const std::size_t start_boards = board_names(m).size();
    const std::size_t fixed_tabs = (churn ? std::max(ai_count, kArrivals.size() - 1) : ai_count) +
        (board_churn ? std::max(start_boards, kBoardArrivals.size() - 1) : start_boards);
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
        // With the AIs churning, two more kinds: add_ai (10) and remove_ai (11); with the dashboards churning, two more:
        // add_dashboard (12) and remove_dashboard (13).
        std::size_t kind = rng.below(10 + (churn ? 2 : 0) + (board_churn ? 2 : 0));
        if (!churn && kind >= 10)
            kind += 2;
        // At most kMaxTerminals terminals at once: an add beyond that is a close instead.
        std::size_t terminals = 0;
        for (std::size_t t = 0; t < m.tab_count(); ++t)
            terminals += m.tab(t).kind == nexus::TabKind::Terminal ? 1 : 0;
        if (kind == 5 && terminals >= kMaxTerminals)
            kind = 8;
        AIInfo arrival;
        std::string board;
        switch (kind) {
        case 12:
            op = "add_board";
            board = kBoardArrivals[rng.below(kBoardArrivals.size())];
            ret = m.add_dashboard(board);
            // The id of the dashboard's new tab: the dashboards' names are each on one tab.
            for (std::size_t t = 0; ret && t < m.tab_count(); ++t) {
                if (m.tab(t).kind == nexus::TabKind::Dashboard && m.tab(t).name == board)
                    id = m.tab(t).id;
            }
            i = 0;
            break;
        case 13: {
            op = "remove_board";
            const bool is_board = i < m.tab_count() && m.tab(i).kind == nexus::TabKind::Dashboard;
            id = is_board ? m.tab(i).id : 0;
            board = is_board ? m.tab(i).name : std::string();
            ret = m.remove_dashboard(i);
            if (!ret) {
                id = 0;
                board.clear();
            }
            break;
        }
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
        records.push_back(record(op, i, j, text, ent, ret, id, state(m), arrival, board));
        // Every 25th step, the oldest queued prompt of the tab at that position leaves (the server delivered it): a
        // step of its own that draws no random number, so every walk's other steps are as they were, and seldom
        // enough that queues still grow long.
        if (s % 25 == 24) {
            const bool took = m.take_queued(i);
            accepted += took ? 1 : 0;
            records.push_back(record("take", i, 0, "", kNoEntry, took, 0, state(m)));
        }
        // Every 50th step with the dashboards churning, the server's list arrives: sync_dashboards to a list drawn from
        // the arrivals (each in or out, in a drawn order). It is only remove_dashboard and add_dashboard calls, so the
        // walk replays it on a copy one call at a time to record each as its step, and checks that the copy ends as the
        // model does.
        if (board_churn && s % 50 == 49) {
            std::vector<std::string> list;
            for (const std::string& name : kBoardArrivals) {
                if (rng.below(2) == 0)
                    list.insert(list.begin() + static_cast<std::ptrdiff_t>(rng.below(list.size() + 1)), name);
            }
            M replay = m;
            const auto changes = m.sync_dashboards(list);
            // Then the strip's dashboards are the server's list: each name in it once (the empty name is no dashboard,
            // add_dashboard refuses it), and no other. A sync that keeps a dropped dashboard or misses a new one fails
            // here, whatever the steps it reports.
            std::vector<std::string> want;
            for (const std::string& name : list) {
                if (!name.empty())
                    want.push_back(name);
            }
            std::vector<std::string> have = board_names(m);
            std::sort(want.begin(), want.end());
            std::sort(have.begin(), have.end());
            if (have != want) {
                std::fprintf(stderr, "trace_walk: after sync_dashboards at step %zu the strip's dashboards are not the "
                             "server's list\n", s);
                return 1;
            }
            for (const std::string& name : changes.removed) {
                std::size_t at = 0;
                while (!(replay.tab(at).kind == nexus::TabKind::Dashboard && replay.tab(at).name == name))
                    ++at;
                const std::size_t gone = replay.tab(at).id;
                const bool done = replay.remove_dashboard(at);
                accepted += done ? 1 : 0;
                last_index = std::max(last_index, at);
                records.push_back(record("remove_board", at, 0, "", kNoEntry, done, gone, state(replay), AIInfo{}, name));
            }
            for (const std::string& name : changes.added) {
                const bool done = replay.add_dashboard(name);
                accepted += done ? 1 : 0;
                std::size_t added = 0;
                for (std::size_t t = 0; t < replay.tab_count(); ++t) {
                    if (replay.tab(t).kind == nexus::TabKind::Dashboard && replay.tab(t).name == name)
                        added = replay.tab(t).id;
                }
                records.push_back(record("add_board", 0, 0, "", kNoEntry, done, added, state(replay), AIInfo{}, name));
            }
            // A faulty model's own remove_dashboard or add_dashboard differs from the Model calls inside
            // sync_dashboards on purpose: its walk goes on from the replay, whose steps are the ones recorded.
            if (std::is_same_v<M, Model> && state(replay) != state(m)) {
                std::fprintf(stderr, "trace_walk: sync_dashboards at step %zu is not its removes and adds\n", s);
                return 1;
            }
            m = replay;
        }
    }

    std::ofstream out(path);
    if (!out) {
        std::fprintf(stderr, "trace_walk: cannot write %s\n", path);
        return 1;
    }
    out << "---- MODULE WindowTraceData ----\n";
    out << "\\* Written by tests/model/trace_walk: seed " << seed << ", " << steps << " steps, " << ai_count
        << " AIs, dashboard list " << board_list << (churn ? ", AIs churning" : "")
        << (board_churn ? ", dashboards churning" : "") << kFault<M> << ".\n";
    out << "EXTENDS Integers\n";
    out << "TraceBoards == <<";
    for (std::size_t k = 0; k < boards.size(); ++k)
        out << (k ? ", " : "") << str(boards[k]);
    out << ">>\n";
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
    out << "TraceBoardArrivals == {";
    if (board_churn) {
        for (std::size_t k = 0; k < kBoardArrivals.size(); ++k)
            out << (k ? ", " : "") << str(kBoardArrivals[k]);
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
    std::printf("trace_walk: seed %llu, %zu steps, %zu AIs, dashboard list %zu%s%s%s: %zu accepted, %zu refused, "
                "longest queue %zu -> %s\n", static_cast<unsigned long long>(seed), records.size() - 1, ai_count,
                board_list, churn ? ", AIs churning" : "", board_churn ? ", dashboards churning" : "", kFault<M>,
                accepted, records.size() - 1 - accepted, longest, path);
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
        "--faulty-send-trim", "--faulty-send-dedup", "--faulty-close", "--faulty-board-move", "--faulty-board-close",
        "--faulty-close-fallback", "--faulty-remove-ai", "--faulty-add-board", "--faulty-remove-board",
        "--faulty-sync-keep"};
    std::string fault;
    int faultCount = 0;
    std::uint64_t board_list = 1;
    bool boards_ok = true;
    bool churn = false;
    bool board_churn = false;
    std::vector<std::string> rest;
    for (std::size_t k = 0; k < args.size(); ++k) {
        const std::string& a = args[k];
        if (a == "--boards") {
            boards_ok = k + 1 < args.size() && parse(args[k + 1].c_str(), board_list) && board_list < kBoardLists.size();
            ++k;
        } else if (a == "--ais-churn") {
            churn = true;
        } else if (a == "--boards-churn") {
            board_churn = true;
        } else if (std::find(faults.begin(), faults.end(), a) != faults.end()) {
            fault = a;
            ++faultCount;
        } else {
            rest.push_back(a);
        }
    }
    std::uint64_t seed = 0, steps = 0, ai_count = 0;
    if (rest.size() != 4 || !boards_ok || !parse(rest[0].c_str(), seed) || !parse(rest[1].c_str(), steps) ||
        !parse(rest[2].c_str(), ai_count) || ai_count > kAIs.size() || faultCount > 1 ||
        (fault == "--faulty-remove-ai" && !churn) ||
        ((fault == "--faulty-add-board" || fault == "--faulty-remove-board" || fault == "--faulty-sync-keep") &&
         !board_churn)) {
        std::fprintf(stderr, "usage: trace_walk <seed> <steps> <ais 0..%zu> [--boards <0..%zu>] [--ais-churn] "
                     "[--boards-churn] [--faulty | --faulty-queue | --faulty-move | --faulty-queue-cap | "
                     "--faulty-send-trim | --faulty-send-dedup | --faulty-close | --faulty-board-move | "
                     "--faulty-board-close | --faulty-close-fallback | --faulty-remove-ai (with --ais-churn) | "
                     "--faulty-add-board | --faulty-remove-board | --faulty-sync-keep (with --boards-churn)] "
                     "<out.tla>\n",
                     kAIs.size(), kBoardLists.size() - 1);
        return 2;
    }
    const char* out = rest[3].c_str();
    const std::size_t b = static_cast<std::size_t>(board_list);
    if (fault == "--faulty-sync-keep")
        return walk<FaultySyncKeepModel>(seed, steps, ai_count, b, churn, board_churn, out);
    if (fault == "--faulty-remove-board")
        return walk<FaultyRemoveBoardModel>(seed, steps, ai_count, b, churn, board_churn, out);
    if (fault == "--faulty-add-board")
        return walk<FaultyAddBoardModel>(seed, steps, ai_count, b, churn, board_churn, out);
    if (fault == "--faulty-remove-ai")
        return walk<FaultyRemoveAIModel>(seed, steps, ai_count, b, churn, board_churn, out);
    if (fault == "--faulty-close-fallback")
        return walk<FaultyCloseFallbackModel>(seed, steps, ai_count, b, churn, board_churn, out);
    if (fault == "--faulty-board-move")
        return walk<FaultyBoardMoveModel>(seed, steps, ai_count, b, churn, board_churn, out);
    if (fault == "--faulty-board-close")
        return walk<FaultyBoardCloseModel>(seed, steps, ai_count, b, churn, board_churn, out);
    if (fault == "--faulty-close")
        return walk<FaultyCloseModel>(seed, steps, ai_count, b, churn, board_churn, out);
    if (fault == "--faulty-send-dedup")
        return walk<FaultySendDedupModel>(seed, steps, ai_count, b, churn, board_churn, out);
    if (fault == "--faulty-queue-cap")
        return walk<FaultyQueueCapModel>(seed, steps, ai_count, b, churn, board_churn, out);
    if (fault == "--faulty-send-trim")
        return walk<FaultySendTrimModel>(seed, steps, ai_count, b, churn, board_churn, out);
    if (fault == "--faulty-move")
        return walk<FaultyMoveModel>(seed, steps, ai_count, b, churn, board_churn, out);
    if (fault == "--faulty-queue")
        return walk<FaultyQueueModel>(seed, steps, ai_count, b, churn, board_churn, out);
    if (fault == "--faulty")
        return walk<FaultyModel>(seed, steps, ai_count, b, churn, board_churn, out);
    return walk<Model>(seed, steps, ai_count, b, churn, board_churn, out);
}
