// The window's side of the down state: brings the window's model (model.h) up to the sessions nexus-server sends
// (sync/sessions.h). Which session a friend's tab shows is the window's own, kept here and nowhere on the server or the
// wire: the friend's first session (the lowest id) in the first state that holds one, and that one from then on. The
// model's transcripts only grow, so the mirror appends what it has not shown yet: the session's closed entries, in
// order, each once. An entry still open (the friend's text as it grows: the newest entry of the session) is not
// appended: the mirror holds it (open()) for the window to show after the transcript, and appends it once it is
// closed. When a state's tail starts past what the window has shown (it was away for longer than the tail holds), the
// window gets a notice of how many entries it did not receive, then the tail. The model's queue is made the server's:
// the prompts the server took from the queue's head (delivered at a turn's end) leave it, and the prompts queued since
// are appended. Sessions no friend tab shows are left out.

#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>

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

    // The friend's entry still growing in the session behind a friend's tab (by Tab::id), as the last state applied
    // holds it: the entry at seen(id).next, still open. Null when there is none.
    const SessionEntry* open(std::size_t id) const;

private:
    struct Shown {
        std::uint64_t next = 0;
        EntryRef open;
    };
    std::map<std::size_t, SessionId> session_;  // tab id -> the session it shows: the window's own, never sent
    std::map<std::size_t, Shown> seen_;
};

} // namespace nexus::sync
