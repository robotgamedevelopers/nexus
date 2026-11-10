// The window's side of the down state: brings the window's model (model.h) up to the tabs nexus-server sends
// (sync/tabs.h). The model's transcripts and queues only grow, so the mirror appends what it has not shown yet:
// each friend's closed entries, in order, and its queued prompts. It shows an entry once it is closed (an entry still
// growing shows when it closes). When a state's tail starts past what the window has shown (it was away for longer
// than the tail holds), the window gets a notice of how many entries it did not receive, then the tail. A tab is
// matched to the server's by the friend's name; the server's tabs no friend tab shows are left out.

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
    // Returns true if it changed the model.
    bool apply(Model& model, const Tabs& state);

    // What the window has shown of the server's tab behind a friend's tab (by Tab::id): the transcript index of the
    // next entry to show, and how many queued prompts it shows.
    struct Seen {
        std::uint64_t next = 0;
        std::size_t queued = 0;
    };
    Seen seen(std::size_t id) const;

private:
    std::map<std::size_t, Seen> seen_;
};

} // namespace nexus::sync
