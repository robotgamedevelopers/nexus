// The link to nexus-server as the window's frame sees it (frame_host.h): the latest state, read without waiting, and
// actions handed over without waiting. link/client_port.h is the one over the link's thread (link/client.h); on Windows
// relay/relay_port.h is the one over the link's thread run inside WSL2 (nexus-link-relay, relay/relay_wire.h). No Prax
// and no OS here, so that the relay, which builds without the window, serves the same interface.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "sync/actions.h"
#include "sync/client.h"
#include "sync/sessions.h"

namespace nexus {

// The link to nexus-server as the window's frame sees it: the latest state, read without waiting, and actions handed
// over without waiting. link/client_port.h is the one over the link's thread (link/client.h).
struct LinkState {
    sync::LinkPhase phase = sync::LinkPhase::Connecting;
    std::string problem;                // with Failed: why
    std::uint64_t heardMsAgo = 0;       // with Connected: since the server was last heard
    sync::Sessions sessions;            // the server's latest state
    bool loopback = false;              // link connected to a numeric loopback address
    // The numbers (send's) of actions the link lost before the server acknowledged them, oldest first, each in exactly
    // one poll: they may not have reached it, and were not sent again. Only the native window on Windows has a link
    // that can lose them (its link in WSL2 ended: relay/relay_life.h); a link that redials sends them again itself.
    std::vector<std::uint64_t> cut;
};

struct LinkPort {
    virtual ~LinkPort() = default;
    // The latest state; true if it changed since the last call that returned true. Never waits.
    virtual bool poll(LinkState* out) = 0;
    virtual void requestHistory(sync::SessionId, std::uint64_t, std::uint64_t) {}
    // An action for the server, in order. Never waits.
    virtual std::uint64_t send(sync::Action action) = 0;
};

} // namespace nexus
