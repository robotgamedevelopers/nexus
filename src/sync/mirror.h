// The window's side of the down state: brings the window's model (model.h) up to the tabs nexus-server sends
// (sync/tabs.h). The model's transcripts only grow, so the mirror appends what it has not shown yet: each friend's
// closed entries, in order, each once. An entry still open (the friend's text as it grows: the newest entry of the
// server's tab) is not appended: the mirror holds it (open()) for the window to show after the transcript, and appends
// it once it is closed. When a state's tail starts past what the window has shown (it was away for longer than the tail
// holds), the window gets a notice of how many entries it did not receive, then the tail. The model's queue is made the
// server's: the prompts the server took from the queue's head (delivered at a turn's end) leave it, and the prompts
// queued since are appended. A tab is matched to the server's by the friend's name; the server's tabs no friend tab
// shows are left out.

#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>

#include "model.h"
#include "sync/tabs.h"

namespace nexus::sync {

// The notice for `count` entries the window did not receive.
std::string notShownNotice(std::uint64_t count);

class Mirror {
public:
    // Returns true if it changed the model or the open entry it holds.
    bool apply(Model& model, const Tabs& state);

    // What the window has shown of the server's tab behind a friend's tab (by Tab::id): the transcript index of the
    // next entry to show.
    struct Seen {
        std::uint64_t next = 0;
    };
    Seen seen(std::size_t id) const;

    // The friend's entry still growing in the server's tab behind a friend's tab (by Tab::id), as the last state
    // applied holds it: the entry at seen(id).next, still open. Null when there is none.
    const TabEntry* open(std::size_t id) const;

private:
    struct Shown {
        std::uint64_t next = 0;
        EntryRef open;
    };
    std::map<std::size_t, Shown> seen_;
};

} // namespace nexus::sync
