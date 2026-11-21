// The window's link to nexus-server: a thread of its own that resolves the host, runs nexus-server new over ssh
// (link/bootstrap.h) and then keeps mosh's session (link/mosh_link.h). The window hands it prompts (send) and reads
// the server's sessions and the link's state each frame (poll); the thread wakes the window when either changed.
//
// When the session is lost (the server ended it, as one stopping or replaced does, or it has not been heard for
// kQuietMs), the thread runs the bootstrap again by itself, with the backoff sync/redial.h decides, and the new
// session goes on from the window's first action the server has not acknowledged: it sends those again under their
// own numbers, with the window's client id, and the server skips any it applied (server/server.h). Until the new
// session is heard, the snapshot keeps the last sessions the window had and its phase is not Connected, so the cursor
// is grey (sync/client.h cursorOf); nothing asks the person anything. Every wait, the bootstrap's included, is on this
// thread, never the window's. The decision is models/Reconnect.tla, bound to sync::Redial by its trace.

#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "link/bootstrap.h"
#include "sync/actions.h"
#include "sync/client.h"
#include "sync/sessions.h"

namespace nexus::link {

class ClientLink {
public:
    struct Options {
        Bootstrap bootstrap;
        std::string ip;                     // the server's numeric address; empty: resolveHost
        std::function<void()> wake;         // called on the link's thread when poll has something new
        // The window's client id (server/server.h validClientId); empty: one made for this link, "window-" and 16
        // random hex digits, so that two windows, or two launches of one, never share one.
        std::string client;
    };

    struct Snapshot {
        sync::LinkPhase phase = sync::LinkPhase::Connecting;
        std::string problem;                // with Failed: why
        std::uint64_t heardMsAgo = 0;       // with Connected: since the server was last heard
        std::uint64_t stateNum = 0;         // the number of the server's latest state
        sync::Sessions sessions;            // the server's latest state
        std::uint64_t sent = 0;             // actions handed to the link
        bool acknowledged = true;           // the server has acknowledged every one
        bool loopback = false;              // the numeric address this session dials is on loopback
        std::uint64_t reconnects = 0;       // sessions made after the first (the bootstrap run again)
    };

    explicit ClientLink(Options options);
    ~ClientLink();   // shuts the session down (mosh's shutdown, up to two seconds), then ends the thread
    ClientLink(const ClientLink&) = delete;
    ClientLink& operator=(const ClientLink&) = delete;

    // Any thread: an action for the server, sent once the session is up, in the order given.
    void send(sync::Action action);     // its num ignored: the link numbers it
    // The latest snapshot; true if it changed since the last call that returned true.
    bool poll(Snapshot* out);

private:
    void run();
    void publish(const Snapshot& s);
    void nudge();

    Options options_;
    std::mutex mutex_;
    Snapshot snapshot_;
    bool changed_ = true;
    std::vector<sync::Action> pending_;
    std::uint64_t sent_ = 0;
    std::atomic<bool> stop_{false};
    int wakeRead_ = -1;
    int wakeWrite_ = -1;
    std::thread thread_;
};

} // namespace nexus::link
