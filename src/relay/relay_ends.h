// The two ends of the relay's pipe (relay/relay_wire.h) as bytes in and bytes out, with no OS and no time, so that both
// are unit-tested everywhere: the window's (RelayClient, under relay/relay_port.h on Windows) and the relay's
// (RelayServer, under src/link/relay_main.cpp in WSL2).

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "link_port.h"
#include "relay/relay_wire.h"

namespace nexus::relay {

// The window's end. Not thread-safe: relay/relay_port.h holds it under a lock.
class RelayClient {
public:
    // A relay process started: the bytes to write to it first (this end's Hello). The state shown keeps the sessions the
    // window had, its phase Connecting, until the relay's first state arrives.
    std::string open();
    // An action for the server: the bytes to write. Its number is the relay's link's (relay/relay_life.h).
    std::string send(const sync::Action& action);
    std::string history(sync::SessionId id, std::uint64_t epoch, std::uint64_t before);
    // Bytes the relay wrote. False, with why, once they are not the relay protocol (a Hello not this build's, a frame
    // that does not parse or apply): the caller ends that relay, and the reason is the state's problem.
    bool feed(const char* data, std::size_t size, std::string* why);
    // The relay process ended (or was ended for `why`): the phase is Failed with why, the sessions stay as they were.
    void ended(const std::string& why);
    // The latest state; true if it changed since the last call that returned true.
    bool poll(LinkState* out);
    // The latest state, changed or not.
    void peek(LinkState* out) const { *out = shown_; }
    // The relay's Hello was read and found this build's.
    bool helloRead() const { return helloRead_; }
    // The first of this relay's actions the server had not applied when it built the latest state, in the relay's
    // link's numbers (sync::Sessions::nextAction); 0 before a state, or in a format without it.
    std::uint64_t nextAction() const { return states_.state().sessions.nextAction(); }

private:
    void show(LinkState state);

    FrameReader reader_;
    StateReader states_;
    bool helloRead_ = false;
    LinkState shown_;
    bool changed_ = true;
};

// The relay's end.
class RelayServer {
public:
    struct Request {
        enum class Kind { Action, History } kind = Kind::Action;
        sync::Action action;            // with Action
        sync::SessionId session = 0;    // with History: requestHistory's arguments
        std::uint64_t epoch = 0;
        std::uint64_t before = 0;
    };

    // The bytes to write first (this end's Hello).
    std::string open() const { return helloFrame(); }
    // Bytes the window wrote. False, with why, once they are not the relay protocol: the relay then ends.
    bool feed(const char* data, std::size_t size, std::string* why);
    // The window's requests read since the last call, in the order the window made them.
    std::vector<Request> take();
    // The bytes of a frame for the link's state.
    std::string state(const LinkState& s) { return writer_.frame(s); }

private:
    FrameReader reader_;
    StateWriter writer_;
    bool helloRead_ = false;
    std::vector<Request> requests_;
};

} // namespace nexus::relay
