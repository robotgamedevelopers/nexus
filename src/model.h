#pragma once
#include <cstddef>
#include <string>
#include <vector>

namespace nexus {

struct FriendInfo { std::string name; std::string model; };

enum class EntryKind { Person, Friend, Notice };

struct Entry { EntryKind kind; std::string text; };

// What a tab is. The sprint tab, when the model has one, is first and never moves or closes; friend tabs come from the
// friend list and are never closed; terminal tabs come and go.
enum class TabKind { Sprint, Friend, Terminal };

// The zone a tab of `kind` stands in: the strip holds the zones in ascending order, and a tab moves only inside its own.
// The sprint tab is zone 0, alone; friends are zone 1 and terminals zone 2.
int zone(TabKind kind);

struct Tab {
    std::size_t id;     // who the tab is, wherever it stands, never reused: a friend's place in the constructor's list;
                        // the sprint tab's the number of friends; for a terminal the next id after every one handed out
                        // before
    std::string name;   // a friend's; empty for the sprint tab and a terminal
    std::string model;
    std::vector<Entry> transcript;   // a terminal's stays empty
    std::string draft;
    std::vector<std::string> queue;  // prompts queued for later, oldest first; apart from the transcript
    TabKind kind = TabKind::Friend;
};

class Model {
public:
    // With `sprint`, the sprint tab first, then one tab per friend, in order; without, the friend tabs alone. Active tab
    // 0. The friend list may be empty. The window always makes its model with the sprint tab (main.cpp); the model's
    // tests make it both ways, and models/Window.tla checks both (its constant Sprint).
    explicit Model(const std::vector<FriendInfo>& friends, bool sprint = false);
    std::size_t tab_count() const;
    std::size_t active() const;                  // 0 when there are no tabs
    const Tab& tab(std::size_t i) const;         // i < tab_count()
    bool select_tab(std::size_t i);              // false and no change when i >= tab_count(); else tab i is active and
                                                 // its id goes to the end of selection_history()
    bool set_draft(std::size_t i, std::string text); // false and no change unless i is a friend's tab
    bool send(std::size_t i);   // a friend's tab whose draft is not all-whitespace: append {Person, draft} to the
                                // transcript, clear that draft, true; else false, no change. Never touches a queue.
    bool queue(std::size_t i);  // a friend's tab whose draft is not all-whitespace: append the draft to that tab's queue,
                                // clear that draft, true; else false, no change. Never touches a transcript.
    bool append(std::size_t i, Entry e);         // false and no change unless i is a friend's tab
    bool move_tab(std::size_t from, std::size_t to); // false and no change unless both < tab_count() and the tabs at
                                                     // `from` and `to` are in the same zone; else the tab at `from`
                                                     // ends at `to`, the tabs between shift by one, everything the tab
                                                     // holds goes with it, and active() follows its friend
    std::size_t add_terminal();  // appends a terminal tab at the end, makes it active (its id at the end of
                                 // selection_history()), returns its new id
    bool close_tab(std::size_t i); // a terminal's tab: removes it, the tabs after shift left, its id leaves
                                   // selection_history(), true; the sprint tab, a friend's tab or i >= tab_count(): false,
                                   // no change. If it was active, the tab selected before it is active: the last id left
                                   // in selection_history(). With none left, the tab now at i, or the last tab if it was
                                   // the last, or none (0) if no tabs remain.
    // The ids of the tabs selected, most recent last, each once, open tabs only: the tab active at the start, then each
    // select_tab and add_terminal. While there are tabs, its last id is the active tab's. Kept for this run only.
    const std::vector<std::size_t>& selection_history() const;

protected:
    // Protected, not private: the trace walk's faulty models (tests/model/trace_walk.cpp) break these on purpose.
    bool friend_tab(std::size_t i) const;
    bool sendable(std::size_t i) const;
    void touch(std::size_t id);     // Window.tla's Touch: id at the end of history_, and nowhere else
    std::vector<Tab> tabs_;
    std::vector<std::size_t> history_;
    std::size_t active_ = 0;
    std::size_t next_id_ = 0;
};

} // namespace nexus
