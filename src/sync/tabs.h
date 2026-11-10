// Down, server to client: the friend tabs as nexus-server holds them, the state mosh's Transport carries from
// nexus-server to the window (src/link). The server is its only writer. Per tab: the tail of its transcript (the newest
// entries, where an entry may grow until it is closed) and its queue. models/Link.tla is the model; tests/sync/
// link_walk.cpp binds this code to it.
//
// Cheap to copy: a state is one shared pointer to an immutable list of immutable tabs, and a tab's entries are shared
// immutable entries, so the server's store, every state mosh keeps and every client's copy share one copy of each
// entry. A change makes a new list and a new tab for the tab it touches; the rest are shared.
//
// Bounded diffs: a state holds only each transcript's tail (Tail: the newest entries up to a count and a byte budget,
// always the newest entry whole), so a diff from a state however far behind sends at most each tab's tail and its
// queue, never the whole history. The client keeps what it has seen (sync/mirror.h) and marks the entries it never
// saw. The diff from a state that holds an entry still open sends only the text that entry grew by.

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "model.h"

namespace nexus::sync {

struct TabEntry {
    EntryKind kind = EntryKind::Notice;
    std::string text;
    bool closed = true;     // an open entry may still grow by text appended to its end; a closed one never changes
    bool operator==(const TabEntry&) const = default;
};

using EntryRef = std::shared_ptr<const TabEntry>;

struct TabView {
    std::string name;                   // the friend's name
    std::uint64_t first = 0;            // the transcript index (from 0) of entries[0]
    std::vector<EntryRef> entries;      // the transcript's tail, oldest first, through its newest entry
    std::vector<std::string> queue;     // prompts queued for later, oldest first
    std::uint64_t total() const { return first + entries.size(); }  // the transcript's length
    bool operator==(const TabView& x) const;
};

using TabRef = std::shared_ptr<const TabView>;

// How much of a transcript a state carries: the newest entries, at most `entries` of them, while their text totals at
// most `bytes`; the newest entry always, whatever its size.
struct Tail {
    std::size_t entries = 16;
    std::size_t bytes = 16 * 1024;
};

// A tab's tail of a whole transcript, by those rules.
TabView tailOf(std::string name, const std::vector<EntryRef>& transcript, std::vector<std::string> queue, Tail tail);

class Tabs {
public:
    Tabs();
    // The tabs in order of name, no two with one name; throws std::logic_error otherwise.
    explicit Tabs(std::vector<TabRef> tabs);

    const std::vector<TabRef>& tabs() const { return *tabs_; }
    const TabView* find(const std::string& name) const;

    // mosh's state interface.
    // Empty when the states are equal. Otherwise, for every tab: unchanged, or its first index, and from the first
    // entry that `existing` does not hold as it is, each entry (the text it grew by, for one `existing` holds open and
    // that only grew), and its queue if that changed. A tab `existing` has and this state lacks is left out.
    std::string diff_from(const Tabs& existing) const;
    std::string init_diff() const;
    // Throws BadDiff, changing nothing, on malformed bytes or a diff from a state other than this one.
    void apply_string(const std::string& diff);
    bool operator==(const Tabs& x) const;
    bool compare(const Tabs& x) const { return !(*this == x); }  // mosh's verbose check: true when they differ
    void subtract(const Tabs*) {}   // a state is the whole picture, not a stream: nothing to forget
    void reset_input() {}

private:
    std::shared_ptr<const std::vector<TabRef>> tabs_;
};

} // namespace nexus::sync
