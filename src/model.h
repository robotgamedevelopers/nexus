#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace nexus {

// A conversation supplied by the server's harness binding, outside the registered AIs' name space.
inline constexpr char kPrimaryChat[] = "@primary";

struct AIInfo { std::string name; std::string model; };

// Person: the person's prompt; AI: the AI's text; Notice: a line from nexus; Tool: the AI used a tool, one
// line naming it and what it does (its name, a colon, its summary).
// Bus is an envelope accepted by the server. Its text is the sender, one newline, then the display body.
// Value 4 is reserved for a durable context boundary and is never a visible entry.
enum class EntryKind { Person, AI, Notice, Tool, Bus = 5 };

inline constexpr std::string_view kAccountSwitchPrefix = "Switched to ";
struct Entry {
    EntryKind kind;
    std::string text;
    std::uint32_t accountEmailEnd = 0;  // byte offset after the address in an account-switch notice; zero for other entries
};

// A change at an index in the original transcript: insert before it, erase it, or replace it.
struct TranscriptEdit {
    std::size_t at;
    bool erase;
    std::optional<Entry> replacement;
    Entry* removed = nullptr;   // when supplied, move the erased row here
};

// Native presentation and lifecycle. The active fork separately assigns each tab type to a group.
enum class TabKind { Dashboard, AI, Terminal, Extension };

// The built-in order used by older window descriptions without tab groups.
int zone(TabKind kind);

struct TabGroup {
    std::string id;
    std::vector<std::string> types{};
    bool closable = false;
    std::vector<std::string> acceptsFrom{};
    bool operator==(const TabGroup&) const = default;
};

struct Tab {
    std::size_t id;     // who the tab is, wherever it stands, never reused: an AI's place in the constructor's list;
                        // a dashboard's from the constructor the number of those AIs plus its place in its list; for a
                        // terminal, and an AI or dashboard added later, the next id after every one handed out before
    std::string name;   // an AI's, or a dashboard's (its identity in the strip and its label); empty for a terminal
    std::string model;
    std::vector<Entry> transcript;   // a terminal's stays empty
    std::string draft;
    std::vector<std::string> queue;  // prompts queued for later, oldest first; apart from the transcript
    TabKind kind = TabKind::AI;
    std::string type{};
    std::string group{};
    std::string declaredGroup{};  // the plugin default when an older fork provides no tab_groups
    std::uint64_t transcriptGeneration = 0;
    std::string held{};  // one multiline message staged locally until release

    bool primaryChat() const { return kind == TabKind::AI && name == kPrimaryChat; }
    std::string_view displayName() const { return primaryChat() ? std::string_view("Chat") : std::string_view(name); }
};

class Model {
public:
    // One tab per dashboard, in the order given, with the ids N..N+B-1 (N the number of AIs), then one tab per AI, in
    // order, with the ids 0..N-1. An empty name, or a name given before, makes no tab (the ids stay contiguous over
    // the tabs made). Active tab 0, and its id the selection history; with no tabs, active 0 and an empty history.
    // Either list may be empty. The windows make their model with no dashboards and no AIs: both come from the server
    // (main.cpp, sync/client.h applyAIs, sync/dashboards.h). models/Window.tla checks it (Init, the constant
    // BoardList).
    explicit Model(const std::vector<AIInfo>& ais, const std::vector<std::string>& dashboards = {});
    std::size_t tab_count() const;
    std::size_t active() const;                  // 0 when there are no tabs
    const Tab& tab(std::size_t i) const;         // i < tab_count()
    bool select_tab(std::size_t i);              // false and no change when i >= tab_count(); else tab i is active and
                                                 // its id goes to the end of selection_history()
    bool set_draft(std::size_t i, std::string text); // false and no change unless i is an AI's tab
    bool send(std::size_t i);   // an AI's tab whose draft is not all-whitespace: append {Person, draft} to the
                                // transcript, clear that draft, true; else false, no change. Never touches a queue.
    bool queue(std::size_t i);  // an AI's tab whose draft is not all-whitespace: append the draft to that tab's queue,
                                // clear that draft, true; else false, no change. Never touches a transcript.
    bool hold_draft(std::size_t i);
    void clear_composer(std::size_t i);
    void restore_held(std::size_t i, std::string text);
    void pop_held(std::size_t i);
    bool take_queued(std::size_t i);  // an AI's tab whose queue is not empty: its oldest queued prompt leaves the
                                      // queue (the server delivered it), true; else false, no change. Never touches a
                                      // transcript or a draft.
    void clear_output(std::size_t i);
    void clear_transcript(std::size_t i);
    bool append(std::size_t i, Entry e);         // false and no change unless i is an AI's tab
    // Edits are ordered by original index. Multiple insertions before one entry retain their order.
    void edit_transcript(std::size_t i, std::vector<TranscriptEdit> edits);
    bool move_tab(std::size_t from, std::size_t to); // false and no change unless both < tab_count() and the tabs at
                                                     // the destination group accepts the move; else the tab at `from`
                                                     // ends at `to`, the tabs between shift by one, everything the tab
                                                     // holds goes with it, and active() follows its AI
    std::uint64_t layout_revision() const { return layoutRevision_; }
    // Reconcile a newly loaded group description, preserving active identity and permitted instance moves.
    bool set_tab_groups(const std::vector<TabGroup>& groups);
    bool move_to_group(std::size_t from, const std::string& group, std::size_t slot);
    // Saved ids with their destination group, in saved order. Only groups reachable under the active fork are restored.
    void restore_tab_layout(const std::vector<std::pair<std::size_t, std::string>>& ordered);
    std::size_t open_extension(std::string type, std::string group, std::string title);
    bool edit_extension(std::size_t i, std::string text);
    std::size_t add_terminal();  // appends a terminal tab at the end, makes it active (its id at the end of
                                 // selection_history()), returns its new id
    bool dismiss_tab(std::size_t i); // window policy authorized closing this tab; preserves selection history
    bool close_tab(std::size_t i); // a terminal's tab: removes it, the tabs after shift left, its id leaves
                                   // selection_history(), true; a dashboard's, an AI's tab or i >= tab_count(): false,
                                   // no change. If it was active, the tab selected before it is active: the last id left
                                   // in selection_history(). With none left, the tab now at i, or the last tab if it was
                                   // the last, or none (0) if no tabs remain; a tab that shows so is then the history
                                   // (Window.tla ClosedHistory), which ends at the active tab while there are tabs.
    // Window.tla's AddAI: a tab for `f` at the end of the AIs' zone (before every terminal), with a new id,
    // holding nothing; true. The active tab stays active, wherever it now stands, and selection_history() is untouched,
    // but for a model with no tabs, where the new tab is active and its id the history. False and no change for an
    // empty name, or a name an AI's tab already has.
    bool add_ai(AIInfo f);
    // Window.tla's RemoveAI: i an AI's tab: removed with its transcript, draft and queue, by close_tab's rule
    // (the tabs after shift left, its id leaves selection_history(), and if it was active the tab selected before it is
    // active); true. Any other tab, or i >= tab_count(): false, no change.
    bool remove_ai(std::size_t i);
    // add_ai of each of `ais` in turn, in one pass (time linear in the tabs and the AIs, where a loop of
    // add_ai is quadratic): the same strip, active tab, history and ids as that loop. Returns the names of the
    // tabs it added, in order; they stand from the position returned in `at` (the AIs' zone's end before them).
    std::vector<std::string> add_ais(std::vector<AIInfo> ais, std::size_t* at = nullptr);
    // remove_ai of each AI's tab for which `gone` is true, from the last of them to the first, in one pass (time
    // linear in the tabs and the history): the same strip, active tab and history as that loop. `gone` is asked once
    // per AI's tab, in strip order. Returns the ids of the tabs removed, in strip order.
    std::vector<std::size_t> remove_ais(const std::function<bool(const Tab&)>& gone);
    // Window.tla's AddBoard: a tab for the dashboard `name` at the end of the dashboards' zone (after every dashboard,
    // before every AI and terminal), with a new id, holding nothing; true. The active tab stays active, wherever it now
    // stands, and selection_history() is untouched, but for a model with no tabs, where the new tab is active and its
    // id the history. False and no change for an empty name, or a name a dashboard's tab already has.
    bool add_dashboard(std::string name);
    // Window.tla's RemoveBoard: i a dashboard's tab: removed by close_tab's rule (the tabs after shift left, its id
    // leaves selection_history(), and if it was active the tab selected before it is active); true. Any other tab, or
    // i >= tab_count(): false, no change.
    bool remove_dashboard(std::size_t i);
    // The strip brought to the server's dashboard list `names` (sync/dashboards.h, in the server's order): remove_dashboard
    // of every dashboard's tab whose name is not in `names`, then add_dashboard of every name in `names` with no tab, in
    // `names`' order. The tabs that stay keep the place the person gave them; a new one goes at the dashboards' end. A
    // rename is a remove and an add. Returns the names removed, in strip order, and the names added, in the order added:
    // both empty when the strip already matched. Nothing but those calls, so Window.tla's RemoveBoard and AddBoard cover it.
    struct DashboardChanges {
        std::vector<std::string> removed;
        std::vector<std::string> added;
        bool empty() const { return removed.empty() && added.empty(); }
    };
    DashboardChanges sync_dashboards(const std::vector<std::string>& names);
    // The ids of the tabs selected, most recent last, each once, open tabs only: the tab active at the start, then each
    // select_tab and add_terminal, and a tab that shows because every tab in the history closed. While there are tabs,
    // its last id is the active tab's. Kept for this run only.
    const std::vector<std::size_t>& selection_history() const;

protected:
    // Protected, not private: the trace walk's faulty models (tests/model/trace_walk.cpp) break these on purpose.
    bool ai_tab(std::size_t i) const;
    bool sendable(std::size_t i) const;
    void touch(std::size_t id);     // Window.tla's Touch: id at the end of history_, and nowhere else
    void remove_at(std::size_t i);  // the tab at i out, by Window.tla's Removed, Forget and ClosedActive
    std::string home_group(TabKind kind, const std::string& type = {}, const std::string& declared = {}) const;
    std::size_t group_rank(const std::string& group) const;
    std::size_t group_end(const std::string& group) const;
    bool accepts(const std::string& from, const std::string& to) const;
    bool reachable(const std::string& from, const std::string& to) const;
    bool reorder(std::size_t from, std::size_t to);
    void sort_layout(const std::vector<std::pair<std::size_t, std::string>>& ordered);
    std::uint64_t layoutRevision_ = 0;
    std::vector<TabGroup> groups_;
    std::vector<std::string> legacyGroups_{"dashboards", "ais", "terminals"};
    std::vector<Tab> tabs_;
    std::vector<std::size_t> history_;
    std::size_t active_ = 0;
    std::size_t next_id_ = 0;
};

} // namespace nexus
