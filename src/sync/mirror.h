// The window's side of the down state: brings the window's model (model.h) up to the sessions nexus-server sends
// (sync/sessions.h). Which session a friend's tab shows is the window's own, kept here and nowhere on the server or the
// wire: the friend's first session (the lowest id) in the first state that holds one, and that one from then on. The
// mirror appends the session's closed entries in order, each once. Changes to the server's bus sender list can hide
// or reveal bus rows within those entries. An entry still open (the friend's text as it grows) is not
// appended: the mirror holds it (open()) for the window to show after the transcript, and appends it once it is
// closed. When a state's tail starts past what the window has shown (it was away for longer than the tail holds), the
// window gets a notice of how many entries it did not receive, then the tail. The model's queue is made the server's:
// the prompts the server took from the queue's head (delivered at a turn's end) leave it, and the prompts queued since
// are appended. Sessions no friend tab shows are left out.

#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "model.h"
#include "sync/sessions.h"

namespace nexus::sync {

// The notice for `count` entries the window did not receive.
std::string notShownNotice(std::uint64_t count);

class Mirror {
public:
    // Returns true if it changed the model or the open entry it holds.
    bool apply(Model& model, const Sessions& state);

    // The session a friend's tab (by Tab::id) shows: 0 until a state has held one of the friend's.
    SessionId session(std::size_t id) const;

    // What the window has shown of the session behind a friend's tab (by Tab::id): the transcript index of the next
    // entry to show.
    struct Seen {
        std::uint64_t next = 0;
    };
    Seen seen(std::size_t id) const;
    bool missingHistory(std::size_t id) const {
        const auto it = seen_.find(id);
        return it != seen_.end() && it->second.missingHistory;
    }

    // The friend's entry still growing in the session behind a friend's tab (by Tab::id), as the last state applied
    // holds it: the entry at seen(id).next, still open. Null when there is none.
    const SessionEntry* open(std::size_t id) const;

    // What the friend of the session behind a friend's tab (by Tab::id) is doing, as the last state applied that held
    // the session says: null until a state has held it, or when the last state applied did not.
    std::uint32_t activeAgents(std::size_t id) const;
    const Activity* activity(std::size_t id) const;

    // Forgets the tab (by Tab::id) whose friend left the server's list (sync/client.h applyFriends): the session it
    // showed and what it showed of it. Ids are never reused, so a friend that comes back has a new tab, which starts
    // from the session's first entries as any new tab does.
    void forget(std::size_t id);
    void clearOpen(std::size_t tabId);

private:
    struct BusSlot {
        std::uint64_t rawIndex;
        std::size_t ordinaryBefore;    // independent of any insertions or removals of other bus rows
        bool visible;
        std::unique_ptr<Entry> withdrawn{};  // a row this window previously displayed, retained through a roster outage
    };
    struct Shown {
        std::uint64_t next = 0;
        std::uint64_t epoch = 0;
        EntryRef open;
        std::uint32_t agents = 0;
        bool missingHistory = false;
        bool held = false;              // the last state applied held the session
        Activity activity = Activity::Idle;
        std::optional<std::vector<std::string>> busSenders;
        std::vector<BusSlot> bus;
        std::size_t visibleBus = 0;
        std::uint64_t tailFirst = 0;
    };
    static bool reconcileBus(Model& model, std::size_t tab, Shown& seen, const SessionView& view);
    std::map<std::size_t, SessionId> session_;  // tab id -> the session it shows: the window's own, never sent
    std::map<std::size_t, Shown> seen_;
};

} // namespace nexus::sync
