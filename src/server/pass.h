// The end of one pass of nexus-server's loop (server/server.cpp Server::run), in the order group commit needs: the
// batch's one flush first; only once it has returned, what the batch told the drivers is carried out
// (Drivers::release) and the new state is set on every link. A flush that fails ends the pass with neither: what
// reached the disk is unknown, and nothing of the batch may leave the server. The links' ticks, which send the state
// and mosh's acknowledgement of a window's actions, run at the top of the next pass, after this.
//
// The loop calls this, and tests/server/pass_test.cpp holds it to the order: a release or a state set before the
// flush, or after a flush that failed, fails that test. A state set before the flush is not seen on the wire (the state
// leaves on the next pass's tick, after the flush either way), so only that test sees it; a release before the flush
// is seen by the driver, and the flush run below catches it too. models/Supervisor.tla's AtMostOnce is the rule this keeps for
// the drivers (its reversed witness WSupWriteBeforeFlush is a write carried out before its step is durable), and
// tests/link/link_e2e.cpp's flush run checks it on the real server, with a flush held and then lost.

#pragma once

#include <functional>
#include <string>

namespace nexus::server {

struct PassEnd {
    std::function<bool(std::string*)> flush;    // the batch's one flush; false, with why, when it failed
    std::function<void()> release;              // carry out what the batch told the drivers
    std::function<void()> publish;              // set the state on every link
};

// False, with why, when the flush failed: nothing was released or published.
inline bool endPass(const PassEnd& end, bool changed, std::string* why) {
    if (!end.flush(why)) {
        return false;
    }
    end.release();
    if (changed) {
        end.publish();
    }
    return true;
}

} // namespace nexus::server
